// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "HcsVirtualMachineBackend.h"

using wsl::windows::common::vm::hcs::BuildConfiguration;
namespace schema = wsl::windows::common::hcs;

namespace {

constexpr UINT64 c_mib = 1024 * 1024;

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

    TEST_METHOD(PreservesCallerInputsWithoutGuestPolicy)
    {
        const auto request = CreateRequest();
        const auto configuration = BuildConfiguration(request);
        const auto& description = configuration.Description;
        const auto& settings = configuration.Settings;
        const auto& vm = settings.VirtualMachine;
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, description.Identity.VmId));
        VERIFY_ARE_EQUAL(request.Owner, settings.Owner);
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
