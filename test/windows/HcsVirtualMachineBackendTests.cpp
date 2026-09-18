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
        VERIFY_IS_TRUE(vm.Devices.ComPorts.empty());
        VERIFY_IS_FALSE(vm.Devices.VirtioSerial.has_value());
        VERIFY_IS_FALSE(vm.DebugOptions.BugcheckSavedStateFileName.has_value());

        const nlohmann::json json = settings;
        const auto& processor = json.at("VirtualMachine").at("ComputeTopology").at("Processor");
        VERIFY_IS_FALSE(processor.contains("ExposeVirtualizationExtensions"));
        VERIFY_IS_FALSE(processor.contains("EnablePerfmonPmu"));
        VERIFY_IS_FALSE(processor.contains("EnablePerfmonLbr"));
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

};

} // namespace HcsVirtualMachineBackendTests
