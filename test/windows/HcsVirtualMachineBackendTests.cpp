// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "HcsVirtualMachineBackend.h"

using wsl::windows::common::vm::hcs::BuildConfiguration;
namespace schema = wsl::windows::common::hcs;
namespace helpers = wsl::windows::common::helpers;
using helpers::WindowsBuildNumbers;

namespace {

constexpr UINT64 c_mib = 1024 * 1024;
constexpr UINT64 c_gib = 1024 * c_mib;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

VmCreateRequest CreateRequest()
{
    VmCreateRequest request;
    THROW_IF_FAILED(CoCreateGuid(&request.Identity.VmId));
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()));
    request.Identity.UserToken = wil::shared_handle{token.release()};
    request.Owner = L"BackendTest";
    request.Processor.Count = 2;
    request.Memory.SizeBytes = 512 * c_mib;
    request.Boot.KernelPath = L"C:\\images\\kernel";
    request.Boot.InitrdPath = L"C:\\images\\initrd";
    request.Boot.KernelCommandLine = L"initrd=\\initrd init=/caller/init console=ttyS0 custom=value";
    return request;
}

VmCreateRequest CreateRunnableRequest()
{
    auto request = CreateRequest();
    const auto basePath = wsl::windows::common::wslutil::GetBasePath();
    request.Boot.KernelPath = basePath / L"kernel";
    request.Boot.InitrdPath = basePath / L"initrd.img";
    request.Boot.KernelCommandLine = L"initrd=\\initrd.img panic=-1";
    return request;
}

HRESULT ConfigurationResult(const VmCreateRequest& Request)
{
    return wil::ResultFromException([&] { BuildConfiguration(Request); });
}

VmBootDiskRequest CreateDisk(std::wstring Key)
{
    VmBootDiskRequest disk;
    disk.Key = std::move(Key);
    disk.Disk.Source = VmVirtualDiskSource{L"C:\\images\\disk.vhdx"};
    return disk;
}

} // namespace

namespace HcsVirtualMachineBackendTests {

class HcsVirtualMachineBackendTests
{
    WSL_TEST_CLASS(HcsVirtualMachineBackendTests)

    WSL2_TEST_METHOD(StartsAndTerminatesWithoutGuestHandshake)
    {
        auto backend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto terminationEvent = backend->GetTerminationEvent();
        auto secondTerminationEvent = backend->GetTerminationEvent();
        VERIFY_ARE_NOT_EQUAL(terminationEvent.get(), secondTerminationEvent.get());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(terminationEvent.get(), 0));

        backend->Start();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(terminationEvent.get(), 100));
        backend->Terminate();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(terminationEvent.get(), 0));
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(secondTerminationEvent.get(), 0));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), wil::ResultFromException([&] { backend->Start(); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), wil::ResultFromException([&] { backend->Terminate(); }));

        backend.reset();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(terminationEvent.get(), 0));
    }

    WSL2_TEST_METHOD(TerminatesBeforeStart)
    {
        auto backend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto terminationEvent = backend->GetTerminationEvent();
        backend->Terminate();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(terminationEvent.get(), 0));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), wil::ResultFromException([&] { backend->Start(); }));
    }

    WSL2_TEST_METHOD(StartsWithRequestedCreationSettings)
    {
        auto request = CreateRunnableRequest();
        request.Memory.AllowOvercommit = VmFeatureRequest::Required;
        request.Memory.DeferredCommit = VmFeatureRequest::Required;
        request.Memory.ColdDiscard = VmFeatureRequest::Required;
        request.Memory.SmallPages = VmSmallPageMemoryRequest{4, 4, VmSelectionPolicy::Preferred};
        request.Memory.Mmio = VmMmioRequest{16 * c_gib, 36};
        request.HostingProcessNameSuffix = VmRequestedValue<std::wstring>{L"BackendTest", VmSelectionPolicy::Preferred};
        request.EnablePlan9 = true;
        request.EnableBattery = true;
        request.Boot.Method = VmBootMethod::Uefi;
        request.Boot.UefiRootPath = request.Boot.KernelPath.parent_path();
        auto backend = HcsVirtualMachineBackend::Create(request);
        const auto terminationEvent = backend->GetTerminationEvent();
        backend->Start();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(terminationEvent.get(), 100));
        backend->Terminate();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(terminationEvent.get(), 0));
    }

    TEST_METHOD(PreservesCallerInputsWithoutGuestPolicy)
    {
        const auto request = CreateRequest();
        const auto configuration = BuildConfiguration(request);
        const auto& description = configuration.Description;
        const auto& settings = configuration.Settings;
        const auto& vm = settings.VirtualMachine;
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, description.Identity.VmId));
        VERIFY_ARE_EQUAL(request.Owner, settings.Owner);
        VERIFY_ARE_EQUAL(UINT32{2}, settings.SchemaVersion.Major);
        VERIFY_ARE_EQUAL(UINT32{3}, settings.SchemaVersion.Minor);
        VERIFY_IS_TRUE(settings.ShouldTerminateOnLastHandleClosed);
        VERIFY_ARE_EQUAL(request.Processor.Count, vm.ComputeTopology.Processor.Count);
        VERIFY_ARE_EQUAL(request.Memory.SizeBytes / c_mib, vm.ComputeTopology.Memory.SizeInMB);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, description.Boot.KernelCommandLine);
        VERIFY_IS_TRUE(description.BootDisks.empty());
        VERIFY_IS_TRUE(vm.Devices.Scsi.at("0").Attachments.empty());
        VERIFY_IS_TRUE(vm.Devices.ComPorts.empty());
        VERIFY_IS_FALSE(vm.Devices.VirtioSerial.has_value());
        VERIFY_IS_FALSE(vm.DebugOptions.BugcheckSavedStateFileName.has_value());

        const nlohmann::json json = settings;
        const auto& processor = json.at("VirtualMachine").at("ComputeTopology").at("Processor");
        VERIFY_IS_FALSE(processor.contains("ExposeVirtualizationExtensions"));
        VERIFY_IS_FALSE(processor.contains("EnablePerfmonPmu"));
        VERIFY_IS_FALSE(processor.contains("EnablePerfmonLbr"));
        VERIFY_IS_FALSE(json.at("VirtualMachine").at("Devices").contains("Plan9"));
        VERIFY_IS_FALSE(json.at("VirtualMachine").at("Devices").contains("Battery"));
        // Existing HCS callers retain their default devices.
        const nlohmann::json defaultDevices = schema::Devices{};
        VERIFY_IS_TRUE(defaultDevices.contains("Plan9"));
        VERIFY_IS_TRUE(defaultDevices.contains("Battery"));

        if constexpr (wsl::shared::Arm64)
        {
            VERIFY_ARE_EQUAL(VmBootMethod::Uefi, description.Boot.Method);
            VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, vm.Chipset.Uefi->BootThis.OptionalData);
        }
        else
        {
            VERIFY_ARE_EQUAL(VmBootMethod::LinuxDirect, description.Boot.Method);
            VERIFY_ARE_EQUAL(request.Boot.KernelPath.native(), vm.Chipset.LinuxKernelDirect->KernelFilePath);
            VERIFY_ARE_EQUAL(request.Boot.InitrdPath.native(), vm.Chipset.LinuxKernelDirect->InitRdPath);
            VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, vm.Chipset.LinuxKernelDirect->KernelCmdLine);
        }

        const auto tokenUser = wil::get_token_information<TOKEN_USER>(request.Identity.UserToken.get());
        wil::unique_hlocal_string sid;
        THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(tokenUser->User.Sid, &sid));
        const auto expected = std::format(L"D:P(A;;FA;;;SY)(A;;FA;;;{})", sid.get());
        VERIFY_ARE_EQUAL(expected, vm.Devices.HvSocket.HvSocketConfig.DefaultBindSecurityDescriptor);
        VERIFY_ARE_EQUAL(expected, vm.Devices.HvSocket.HvSocketConfig.DefaultConnectSecurityDescriptor);
    }

    TEST_METHOD(UsesCallerDirectoryForFirmwareBoot)
    {
        auto request = CreateRequest();
        request.Boot.Method = VmBootMethod::Uefi;
        const auto configuration = BuildConfiguration(request);
        const auto& chipset = configuration.Settings.VirtualMachine.Chipset;
        VERIFY_IS_FALSE(chipset.LinuxKernelDirect.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L"C:\\images"}, chipset.Uefi->BootThis.VmbFsRootPath);
        VERIFY_ARE_EQUAL(std::wstring{L"\\kernel"}, chipset.Uefi->BootThis.DevicePath);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, chipset.Uefi->BootThis.OptionalData);
        request.Boot.InitrdPath = L"C:\\other\\initrd";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(UsesExplicitFirmwareRootForNestedBootFiles)
    {
        auto request = CreateRequest();
        request.Boot.Method = VmBootMethod::Uefi;
        request.Boot.UefiRootPath = L"C:\\IMAGES";
        request.Boot.KernelPath = L"C:\\images\\kernels\\kernel";
        request.Boot.InitrdPath = L"C:\\images\\ramdisks\\initrd";
        request.Boot.KernelCommandLine = L"initrd=\\ramdisks\\initrd caller=value";
        const auto configuration = BuildConfiguration(request);
        const auto& boot = configuration.Settings.VirtualMachine.Chipset.Uefi->BootThis;
        VERIFY_ARE_EQUAL(std::wstring{L"C:\\IMAGES"}, boot.VmbFsRootPath);
        VERIFY_ARE_EQUAL(std::wstring{L"\\kernels\\kernel"}, boot.DevicePath);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, boot.OptionalData);
        request.Boot.UefiRootPath = L"C:\\images\\";
        VERIFY_SUCCEEDED(ConfigurationResult(request));
        request.Boot.KernelPath = L"C:\\images-other\\kernel";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Boot.KernelPath = L"C:\\images\\kernels\\..\\..\\kernel";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Boot.KernelPath = L"C:\\images\\kernels\\kernel";
        request.Boot.InitrdPath = L"D:\\images\\initrd";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Boot.InitrdPath.clear();
        VERIFY_SUCCEEDED(ConfigurationResult(request));
        request.Boot.UefiRootPath = L"relative";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Boot.UefiRootPath = L"C:\\images";
        if constexpr (!wsl::shared::Arm64)
        {
            request.Boot.Method = VmBootMethod::LinuxDirect;
            VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        }
    }

    TEST_METHOD(SerializesKernelCommandLineVerbatimForBothBootMethods)
    {
        auto request = CreateRequest();
        for (const auto method : {VmBootMethod::LinuxDirect, VmBootMethod::Uefi})
        {
            if (wsl::shared::Arm64 && method == VmBootMethod::LinuxDirect)
            {
                continue;
            }

            request.Boot.Method = method;
            for (const auto commandLine :
                 {L"",
                  L"  console=caller panic=0 panic=-1 key=\"two words\"  ",
                  L"initrd=\\initrd init=/caller/init page_reporting.order=5 swiotlb=32768"})
            {
                request.Boot.KernelCommandLine = commandLine;
                const auto configuration = BuildConfiguration(request);
                const auto json =
                    nlohmann::json::parse(wsl::shared::string::WideToMultiByte(wsl::shared::ToJsonW(configuration.Settings)));
                const auto& chipset = json.at("VirtualMachine").at("Chipset");
                const auto& serializedCommandLine = method == VmBootMethod::Uefi
                                                        ? chipset.at("Uefi").at("BootThis").at("OptionalData")
                                                        : chipset.at("LinuxKernelDirect").at("KernelCmdLine");
                VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(commandLine), serializedCommandLine.get<std::string>());
                VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, configuration.Description.Boot.KernelCommandLine);
            }
        }
    }

    TEST_METHOD(EnablesOnlyRequestedDefaultDevices)
    {
        auto request = CreateRequest();
        request.EnablePlan9 = true;
        auto devices = BuildConfiguration(request).Settings.VirtualMachine.Devices;
        VERIFY_IS_TRUE(devices.Plan9.has_value());
        VERIFY_IS_FALSE(devices.Battery.has_value());
        request.EnableBattery = true;
        const nlohmann::json both = BuildConfiguration(request).Settings.VirtualMachine.Devices;
        VERIFY_IS_TRUE(both.at("Plan9").is_object());
        VERIFY_IS_TRUE(both.at("Battery").is_object());
        request.EnablePlan9 = false;
        devices = BuildConfiguration(request).Settings.VirtualMachine.Devices;
        VERIFY_IS_FALSE(devices.Plan9.has_value());
        VERIFY_IS_TRUE(devices.Battery.has_value());
    }

    TEST_METHOD(UsesCallerMmioWindowAndAddressLimit)
    {
        auto request = CreateRequest();
        const auto defaults = BuildConfiguration(request);
        VERIFY_IS_FALSE(defaults.Settings.VirtualMachine.ComputeTopology.Memory.HighMmioGapInMB.has_value());
        VERIFY_IS_FALSE(defaults.Settings.VirtualMachine.ComputeTopology.Memory.HighMmioBaseInMB.has_value());
        request.Memory.Mmio = VmMmioRequest{24 * c_gib, 36};
        const auto configuration = BuildConfiguration(request);
        const auto& memory = configuration.Settings.VirtualMachine.ComputeTopology.Memory;
        VERIFY_ARE_EQUAL(UINT64{24 * 1024}, memory.HighMmioGapInMB.value());
        VERIFY_ARE_EQUAL(UINT64{40 * 1024}, memory.HighMmioBaseInMB.value());
        VERIFY_ARE_EQUAL(24 * c_gib, configuration.Description.Memory.HighMmioSizeBytes.value());
        VERIFY_ARE_EQUAL(40 * c_gib, configuration.Description.Memory.HighMmioBaseBytes.value());
        const nlohmann::json json = memory;
        VERIFY_ARE_EQUAL(UINT64{24 * 1024}, json.at("HighMmioGapInMB").get<UINT64>());
        VERIFY_ARE_EQUAL(UINT64{40 * 1024}, json.at("HighMmioBaseInMB").get<UINT64>());
        request.Memory.Mmio->MaximumGuestAddressBits.reset();
        const auto hostBase = BuildConfiguration(request);
        VERIFY_IS_TRUE(hostBase.Settings.VirtualMachine.ComputeTopology.Memory.HighMmioGapInMB.has_value());
        VERIFY_IS_FALSE(hostBase.Settings.VirtualMachine.ComputeTopology.Memory.HighMmioBaseInMB.has_value());
        VERIFY_IS_FALSE(hostBase.Description.Memory.HighMmioBaseBytes.has_value());
    }

    TEST_METHOD(RejectsInvalidMmioWindows)
    {
        auto request = CreateRequest();
        request.Memory.Mmio = VmMmioRequest{0, 36};
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Memory.Mmio->HighWindowSizeBytes = 16 * c_gib + 1;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Memory.Mmio->HighWindowSizeBytes = 16 * c_gib;
        request.Memory.Mmio->MaximumGuestAddressBits = 64;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Memory.Mmio->MaximumGuestAddressBits = 32;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Memory.Mmio->MaximumGuestAddressBits = 36;
        request.Memory.Mmio->HighWindowSizeBytes = 60 * c_gib;
        VERIFY_SUCCEEDED(ConfigurationResult(request));
        request.Memory.Mmio->HighWindowSizeBytes += c_mib;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Memory.Mmio->HighWindowSizeBytes = 64 * c_gib;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(GatesSmallPagesAndFaultClustersTogether)
    {
        auto request = CreateRequest();
        request.Memory.SmallPages = VmSmallPageMemoryRequest{4, 3, VmSelectionPolicy::Preferred};
        const auto configuration = BuildConfiguration(request);
        const auto& memory = configuration.Settings.VirtualMachine.ComputeTopology.Memory;
        const bool supported = helpers::IsSmallPageMemorySupported(helpers::GetWindowsVersion());
        VERIFY_ARE_EQUAL(supported, configuration.Description.Memory.SmallPages);
        VERIFY_ARE_EQUAL(supported, memory.BackingPageSize.has_value());
        VERIFY_ARE_EQUAL(supported, memory.FaultClusterSizeShift.has_value());
        VERIFY_ARE_EQUAL(supported, memory.DirectMapFaultClusterSizeShift.has_value());
        VERIFY_ARE_EQUAL(supported, configuration.Description.Memory.FaultClusterSizeShift.has_value());
        VERIFY_ARE_EQUAL(supported, configuration.Description.Memory.DirectMapFaultClusterSizeShift.has_value());
        if (supported)
        {
            VERIFY_ARE_EQUAL(schema::MemoryBackingPageSize::Small, memory.BackingPageSize.value());
            VERIFY_ARE_EQUAL(UINT32{4}, memory.FaultClusterSizeShift.value());
            VERIFY_ARE_EQUAL(UINT32{3}, memory.DirectMapFaultClusterSizeShift.value());
            VERIFY_ARE_EQUAL(UINT32{4}, configuration.Description.Memory.FaultClusterSizeShift.value());
            VERIFY_ARE_EQUAL(UINT32{3}, configuration.Description.Memory.DirectMapFaultClusterSizeShift.value());
        }
        request.Memory.SmallPages->Policy = VmSelectionPolicy::Required;
        VERIFY_ARE_EQUAL(supported ? S_OK : c_notSupported, ConfigurationResult(request));
        request.Memory.SmallPages->Policy = static_cast<VmSelectionPolicy>(100);
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(PreservesSmallPageWindowsCompatibilityGate)
    {
        const auto supported = [](ULONG Build, DWORD Revision) {
            return helpers::IsSmallPageMemorySupported({10, 0, Build, Revision});
        };
        VERIFY_IS_FALSE(supported(WindowsBuildNumbers::Vibranium_22H2, 3392));
        VERIFY_IS_TRUE(supported(WindowsBuildNumbers::Vibranium_22H2, 3393));
        VERIFY_IS_FALSE(supported(WindowsBuildNumbers::Iron, 1969));
        VERIFY_IS_TRUE(supported(WindowsBuildNumbers::Iron, 1970));
        VERIFY_IS_FALSE(supported(WindowsBuildNumbers::Cobalt, 0));
        VERIFY_IS_TRUE(supported(WindowsBuildNumbers::Cobalt, 2360));
        VERIFY_IS_TRUE(supported(WindowsBuildNumbers::Germanium, 0));
    }

    TEST_METHOD(UsesOnlyRequestedHostingProcessSuffix)
    {
        auto request = CreateRequest();
        VERIFY_IS_FALSE(BuildConfiguration(request).Settings.VirtualMachine.ComputeTopology.Memory.HostingProcessNameSuffix.has_value());
        request.HostingProcessNameSuffix = VmRequestedValue<std::wstring>{L"CallerVm", VmSelectionPolicy::Preferred};
        const auto memory = BuildConfiguration(request).Settings.VirtualMachine.ComputeTopology.Memory;
        const bool supported = helpers::IsVmemmSuffixSupported();
        VERIFY_ARE_EQUAL(supported, memory.HostingProcessNameSuffix.has_value());
        if (supported)
        {
            VERIFY_ARE_EQUAL(std::wstring{L"CallerVm"}, memory.HostingProcessNameSuffix.value());
        }
        request.HostingProcessNameSuffix->Policy = VmSelectionPolicy::Required;
        VERIFY_ARE_EQUAL(supported ? S_OK : c_notSupported, ConfigurationResult(request));
        request.HostingProcessNameSuffix->Value.push_back(L'\0');
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.HostingProcessNameSuffix->Value.clear();
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.HostingProcessNameSuffix->Value = L"CallerVm";
        request.HostingProcessNameSuffix->Policy = static_cast<VmSelectionPolicy>(100);
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(SupportsBothSerialPortsWithoutChangingGuestArguments)
    {
        auto request = CreateRequest();
        request.Consoles = {
            {VmConsoleRole::KernelConsole, VmSerialConsole{0, L"\\\\.\\pipe\\caller-console"}},
            {VmConsoleRole::KernelDebugger, VmSerialConsole{1, L"\\\\.\\pipe\\caller-debugger"}}};
        const auto configuration = BuildConfiguration(request);
        VERIFY_ARE_EQUAL(size_t{2}, configuration.Settings.VirtualMachine.Devices.ComPorts.size());
        VERIFY_ARE_EQUAL(
            std::wstring{L"\\\\.\\pipe\\caller-debugger"}, configuration.Settings.VirtualMachine.Devices.ComPorts.at("1").NamedPipe);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, configuration.Description.Boot.KernelCommandLine);
        std::get<VmSerialConsole>(request.Consoles[1].Device).Port = 2;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(RejectsInvalidInputsBeforeCreatingSystem)
    {
        auto request = CreateRequest();
        const auto id = request.Identity.VmId;
        request.Identity.VmId = GUID_NULL;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Identity.VmId = id;
        request.Processor.Count = 0;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Processor.Count = 2;
        request.Memory.SizeBytes += 1;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Memory.SizeBytes -= 1;
        request.Boot.KernelPath = L"relative";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Boot.KernelPath = L"C:\\images\\kernel";
        request.Boot.KernelCommandLine.push_back(L'\0');
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Boot.KernelCommandLine.pop_back();
        request.Boot.Method = static_cast<VmBootMethod>(100);
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Boot.Method = VmBootMethod::Automatic;
        request.Identity.UserToken.reset();
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(AppliesOnlyRequestedMemoryFeatures)
    {
        auto request = CreateRequest();
        const auto disabled = BuildConfiguration(request).Settings.VirtualMachine.ComputeTopology.Memory;
        VERIFY_IS_FALSE(disabled.AllowOvercommit);
        VERIFY_IS_FALSE(disabled.EnableDeferredCommit);
        VERIFY_IS_FALSE(disabled.EnableColdDiscardHint);
        VERIFY_IS_FALSE(disabled.BackingPageSize.has_value());
        request.Memory.DeferredCommit = VmFeatureRequest::Required;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Memory.AllowOvercommit = VmFeatureRequest::Required;
        request.Memory.ColdDiscard = VmFeatureRequest::Preferred;
        const auto enabled = BuildConfiguration(request).Settings.VirtualMachine.ComputeTopology.Memory;
        VERIFY_IS_TRUE(enabled.AllowOvercommit);
        VERIFY_IS_TRUE(enabled.EnableDeferredCommit);
        VERIFY_IS_TRUE(enabled.EnableColdDiscardHint);
        request.Memory.ColdDiscard = static_cast<VmFeatureRequest>(100);
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(ResolvesRequestedProcessorFeatures)
    {
        auto request = CreateRequest();
        request.Processor.NestedVirtualization = VmFeatureRequest::Preferred;
        request.Processor.PerfmonPmu = VmFeatureRequest::Preferred;
        request.Processor.PerfmonLbr = VmFeatureRequest::Preferred;

        bool nested = false;
        if (helpers::IsWindows11OrAbove())
        {
            const auto& features = schema::GetProcessorFeatures();
            nested = std::find(features.begin(), features.end(), "NestedVirt") != features.end();
        }
        bool pmu = false;
        bool lbr = false;
#ifdef _AMD64_
        HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
        __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
        pmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
        lbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;
#endif
        const auto configuration = BuildConfiguration(request);
        const auto& processor = configuration.Settings.VirtualMachine.ComputeTopology.Processor;
        VERIFY_ARE_EQUAL(nested, processor.ExposeVirtualizationExtensions.value());
        VERIFY_ARE_EQUAL(pmu, processor.EnablePerfmonPmu.value());
        VERIFY_ARE_EQUAL(lbr, processor.EnablePerfmonLbr.value());
        VERIFY_ARE_EQUAL(nested, configuration.Description.Processor.NestedVirtualization);
        VERIFY_ARE_EQUAL(pmu, configuration.Description.Processor.PerfmonPmu);
        VERIFY_ARE_EQUAL(lbr, configuration.Description.Processor.PerfmonLbr);

        request.Processor.NestedVirtualization = VmFeatureRequest::Required;
        VERIFY_ARE_EQUAL(nested ? S_OK : c_notSupported, ConfigurationResult(request));
        request.Processor.NestedVirtualization = VmFeatureRequest::Preferred;
        request.Processor.PerfmonPmu = VmFeatureRequest::Required;
        VERIFY_ARE_EQUAL(pmu ? S_OK : c_notSupported, ConfigurationResult(request));
        request.Processor.PerfmonPmu = VmFeatureRequest::Preferred;
        request.Processor.PerfmonLbr = VmFeatureRequest::Required;
        VERIFY_ARE_EQUAL(lbr ? S_OK : c_notSupported, ConfigurationResult(request));
    }

    TEST_METHOD(ReservesExplicitDiskPlacementsFirst)
    {
        auto request = CreateRequest();
        request.BootDisks = {CreateDisk(L"automatic"), CreateDisk(L"explicit")};
        request.BootDisks[0].Disk.ReadOnly = false;
        request.BootDisks[1].Disk.Placement = VmScsiPlacement{{0, 0}};
        const auto configuration = BuildConfiguration(request);
        const auto& disks = configuration.Settings.VirtualMachine.Devices.Scsi.at("0").Attachments;
        VERIFY_ARE_EQUAL(UINT32{1}, configuration.Description.BootDisks.at(L"automatic").GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{0}, configuration.Description.BootDisks.at(L"explicit").GuestAddress.Lun);
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, configuration.Description.BootDisks.at(L"automatic").Id.Owner.VmId));
        VERIFY_IS_FALSE(disks.at("1").ReadOnly);
        VERIFY_IS_TRUE(disks.at("0").ReadOnly);
        VERIFY_ARE_EQUAL(schema::AttachmentType::VirtualDisk, disks.at("0").Type);
        for (const auto& [lun, disk] : disks)
        {
            const nlohmann::json expected = {
                {"Type", "VirtualDisk"},
                {"Path", "C:\\images\\disk.vhdx"},
                {"ReadOnly", lun == "0"},
                {"SupportCompressedVolumes", true},
                {"AlwaysAllowSparseFiles", true},
                {"SupportEncryptedFiles", true}};
            VERIFY_IS_TRUE(expected == nlohmann::json(disk));
        }
        request.BootDisks[0].Disk.Placement = VmScsiPlacement{{0, 0}};
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.BootDisks[0].Disk.Placement.reset();
        request.BootDisks[0].Key = request.BootDisks[1].Key;
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.BootDisks[0].Key = L"physical";
        request.BootDisks[0].Disk.Source = VmPhysicalDiskSource{L"\\\\.\\PhysicalDrive1"};
        const auto physical = BuildConfiguration(request).Settings.VirtualMachine.Devices.Scsi.at("0").Attachments.at("1");
        VERIFY_ARE_EQUAL(schema::AttachmentType::PassThru, physical.Type);
        VERIFY_ARE_EQUAL(std::wstring{L"\\\\.\\PhysicalDrive1"}, physical.Path);
        VERIFY_IS_FALSE(physical.SupportCompressedVolumes);
        VERIFY_IS_FALSE(physical.AlwaysAllowSparseFiles);
        VERIFY_IS_FALSE(physical.SupportEncryptedFiles);
        request.BootDisks[0].Disk.Placement = VmScsiPlacement{{0, 254}};
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), ConfigurationResult(request));
    }

    TEST_METHOD(UsesOnlyCallerProvidedCrashDestination)
    {
        auto request = CreateRequest();
        request.CrashCapture = VmCrashCaptureRequest{L"C:\\dumps\\caller.vmrs", VmSelectionPolicy::Preferred};
        const auto configuration = BuildConfiguration(request);
        if (wsl::windows::common::helpers::IsWindows11OrAbove())
        {
            VERIFY_ARE_EQUAL(
                request.CrashCapture->SavedStatePath.native(),
                configuration.Settings.VirtualMachine.DebugOptions.BugcheckSavedStateFileName.value());
        }
        else
        {
            VERIFY_IS_FALSE(configuration.Settings.VirtualMachine.DebugOptions.BugcheckSavedStateFileName.has_value());
            request.CrashCapture->Policy = VmSelectionPolicy::Required;
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), ConfigurationResult(request));
        }
        request.CrashCapture->SavedStatePath = L"relative.vmrs";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
    }

    TEST_METHOD(DoesNotInferConsoleConfigurationFromRoles)
    {
        auto request = CreateRequest();
        request.Consoles = {{VmConsoleRole::Telemetry, VmSerialConsole{0, L"\\\\.\\pipe\\caller-serial"}}};
        const auto configuration = BuildConfiguration(request);
        VERIFY_ARE_EQUAL(
            std::wstring{L"\\\\.\\pipe\\caller-serial"}, configuration.Settings.VirtualMachine.Devices.ComPorts.at("0").NamedPipe);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, configuration.Description.Boot.KernelCommandLine);
        request.Consoles.push_back(request.Consoles[0]);
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));
        request.Consoles.pop_back();
        std::get<VmSerialConsole>(request.Consoles[0].Device).NamedPipe = L"C:\\not-a-pipe";
        VERIFY_ARE_EQUAL(E_INVALIDARG, ConfigurationResult(request));

        if (wsl::windows::common::helpers::IsVirtioSerialConsoleSupported())
        {
            request.Consoles = {{VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"caller-port", L"\\\\.\\pipe\\caller-virtio", false}}};
            const auto virtio = BuildConfiguration(request).Settings.VirtualMachine.Devices.VirtioSerial;
            VERIFY_ARE_EQUAL(std::wstring{L"caller-port"}, virtio->Ports.at("0").Name);
            VERIFY_IS_FALSE(virtio->Ports.at("0").ConsoleSupport);
        }
    }
};

} // namespace HcsVirtualMachineBackendTests
