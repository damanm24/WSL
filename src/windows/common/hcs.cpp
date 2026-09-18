/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    hcs.cpp

Abstract:

    This file contains helper function definitions for interacting with the
    host compute service.

--*/

#include "precomp.h"
#include "hcs.hpp"
#include <ComputeCore.h>

#pragma hdrstop

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

constexpr auto c_processorCapabilities = "ProcessorCapabilities";
constexpr LPCWSTR c_processorCapabilitiesQuery = L"{ \"PropertyQueries\": {\"ProcessorCapabilities\" : {}}}";
constexpr LPCWSTR c_scsiResourcePath = L"VirtualMachine/Devices/Scsi/0/Attachments/";

bool wsl::windows::common::hcs::IsNestedVirtualizationSupported()
{
    if (!helpers::IsWindows11OrAbove())
    {
        return false;
    }

    const auto& features = GetProcessorFeatures();
    return std::find(features.begin(), features.end(), "NestedVirt") != features.end();
}

wsl::windows::common::hcs::PerfmonCapabilities wsl::windows::common::hcs::GetPerfmonCapabilities()
{
    PerfmonCapabilities capabilities{};
#ifdef _AMD64_
    HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
    __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
    capabilities.Pmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
    capabilities.Lbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;
#endif
    return capabilities;
}

void wsl::windows::common::hcs::ConfigureSmallPageMemory(Memory& Settings, uint32_t FaultClusterSizeShift, uint32_t DirectMapFaultClusterSizeShift)
{
    Settings.BackingPageSize = MemoryBackingPageSize::Small;
    Settings.FaultClusterSizeShift = FaultClusterSizeShift;
    Settings.DirectMapFaultClusterSizeShift = DirectMapFaultClusterSizeShift;
}

wsl::windows::common::hcs::Attachment wsl::windows::common::hcs::CreateVhdAttachment(_In_ PCWSTR Path, bool ReadOnly)
{
    Attachment attachment{};
    attachment.Type = AttachmentType::VirtualDisk;
    attachment.Path = Path;
    attachment.ReadOnly = ReadOnly;
    attachment.SupportCompressedVolumes = true;
    attachment.AlwaysAllowSparseFiles = true;
    attachment.SupportEncryptedFiles = true;
    return attachment;
}

wsl::windows::common::hcs::HvSocket wsl::windows::common::hcs::CreateHvSocketConfiguration(_In_ PSID UserSid)
{
    wil::unique_hlocal_string userSidString;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(UserSid, &userSidString));
    const auto securityDescriptor = std::format(L"D:P(A;;FA;;;SY)(A;;FA;;;{})", userSidString.get());
    HvSocket settings{};
    settings.HvSocketConfig.DefaultBindSecurityDescriptor = securityDescriptor;
    settings.HvSocketConfig.DefaultConnectSecurityDescriptor = securityDescriptor;
    return settings;
}

void wsl::windows::common::hcs::AddPlan9Share(
    _In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Name, _In_ PCWSTR AccessName, _In_ PCWSTR Path, _In_ UINT32 Port, _In_ Plan9ShareFlags Flags, _In_opt_ HANDLE UserToken)
{
    ModifySettingRequest<Plan9Share> request{};
    request.RequestType = ModifyRequestType::Add;
    request.ResourcePath = L"VirtualMachine/Devices/Plan9/Shares";
    request.Settings.Name = Name;
    request.Settings.AccessName = AccessName;
    request.Settings.Path = Path;
    request.Settings.Port = Port;
    WI_SetFlagIf(Flags, Plan9ShareFlags::UseShareRootIdentity, ARGUMENT_PRESENT(UserToken));
    request.Settings.Flags = Flags;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str(), UserToken);
}

void wsl::windows::common::hcs::RemovePlan9Share(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR AccessName, _In_ UINT32 Port)
{
    ModifySettingRequest<Plan9Share> request{};
    request.RequestType = ModifyRequestType::Remove;
    request.ResourcePath = L"VirtualMachine/Devices/Plan9/Shares";
    request.Settings.AccessName = AccessName;
    request.Settings.Port = Port;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

void wsl::windows::common::hcs::AddVhd(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR VhdPath, _In_ ULONG Lun, _In_ bool ReadOnly)
{
    ModifySettingRequest<Attachment> request{};
    request.RequestType = ModifyRequestType::Add;
    request.ResourcePath = c_scsiResourcePath + std::to_wstring(Lun);
    request.Settings = CreateVhdAttachment(VhdPath, ReadOnly);

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

void wsl::windows::common::hcs::AddPassThroughDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Disk, _In_ ULONG Lun)
{
    AddPassThroughDisk(ComputeSystem, Disk, Lun, false);
}

void wsl::windows::common::hcs::AddPassThroughDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Disk, _In_ ULONG Lun, _In_ bool ReadOnly)
{
    ModifySettingRequest<Attachment> request{};
    request.RequestType = ModifyRequestType::Add;
    request.Settings.Path = Disk;
    request.Settings.ReadOnly = ReadOnly;
    request.ResourcePath = c_scsiResourcePath + std::to_wstring(Lun);
    request.Settings.Type = AttachmentType::PassThru;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

wsl::windows::common::hcs::unique_hcs_operation wsl::windows::common::hcs::CreateOperation()
{
    unique_hcs_operation operation(::HcsCreateOperation(nullptr, nullptr));
    THROW_LAST_ERROR_IF_MSG(!operation, "HcsCreateOperation");

    return operation;
}

wsl::windows::common::hcs::unique_hcs_system wsl::windows::common::hcs::CreateComputeSystem(_In_ PCWSTR Id, _In_ PCWSTR Configuration)
{
    WSL_LOG_DEBUG("HcsCreateComputeSystem", TraceLoggingValue(Id, "id"), TraceLoggingValue(Configuration, "configuration"));

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    unique_hcs_system system{};
    THROW_IF_FAILED(::HcsCreateComputeSystem(Id, Configuration, operation.get(), nullptr, &system));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    if (FAILED(result))
    {
        // N.B. Logging is split into two calls because the configuration and error strings can be quite long.
        LOG_HR_MSG(result, "HcsCreateComputeSystem(%ls, %ls)", Id, Configuration);
        THROW_HR_MSG(result, "HcsCreateComputeSystem failed (error string: %ls)", resultDocument.get());
    }

    return system;
}

const std::vector<std::string>& wsl::windows::common::hcs::GetProcessorFeatures()
{
    static std::vector<std::string> g_processorFeatures;
    static std::once_flag flag;
    std::call_once(flag, []() {
        ExecutionContext context(Context::HCS);

        wil::unique_cotaskmem_string result;
        THROW_IF_FAILED(::HcsGetServiceProperties(c_processorCapabilitiesQuery, &result));

        const auto properties =
            wsl::shared::FromJson<ServicePropertiesResponse<PropertyResponse<ProcessorCapabilitiesInfo>>>(result.get());

        const auto& response = properties.PropertyResponses.at(c_processorCapabilities);
        if (response.Error)
        {
            THROW_HR_MSG(static_cast<HRESULT>(response.Error->Error), "%hs", response.Error->ErrorMessage.c_str());
        }

        g_processorFeatures = response.Response.ProcessorFeatures;
    });

    return g_processorFeatures;
}

wsl::shared::hns::HNSEndpoint wsl::windows::common::hcs::GetEndpointProperties(HCN_ENDPOINT Endpoint)
{
    WSL_LOG_DEBUG("HcsGetEndpointProperties");

    ExecutionContext context(Context::HNS);

    wil::unique_cotaskmem_string propertiesString;
    wil::unique_cotaskmem_string error;
    const auto result = HcnQueryEndpointProperties(Endpoint, nullptr, &propertiesString, &error);
    THROW_IF_FAILED_MSG(result, "HcnQueryEndpointProperties %ls", error.get());

    return wsl::shared::FromJson<wsl::shared::hns::HNSEndpoint>(propertiesString.get());
}

GUID wsl::windows::common::hcs::GetRuntimeId(_In_ HCS_SYSTEM ComputeSystem)
{
    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsGetComputeSystemProperties(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(result, "HcsGetComputeSystemProperties failed (error string: %ls)", resultDocument.get());

    const auto properties = wsl::shared::FromJson<Properties>(resultDocument.get());
    THROW_HR_IF(HCS_E_SYSTEM_NOT_FOUND, (properties.SystemType != SystemType::VirtualMachine));

    return properties.RuntimeId;
}

std::pair<uint32_t, uint32_t> wsl::windows::common::hcs::GetSchemaVersion()
{
    static std::pair<uint32_t, uint32_t> g_schemaVersion{};
    static std::once_flag flag;
    std::call_once(flag, []() {
        ExecutionContext context(Context::HCS);

        PropertyQuery query;
        query.PropertyTypes.emplace_back(PropertyType::Basic);
        wil::unique_cotaskmem_string result;
        THROW_IF_FAILED(::HcsGetServiceProperties(wsl::shared::ToJsonW(query).c_str(), &result));

        const auto properties = wsl::shared::FromJson<ServiceProperties<BasicInformation>>(result.get());
        THROW_HR_IF_MSG(E_UNEXPECTED, properties.Properties.empty(), "%ls", result.get());

        uint32_t majorVersion = 0;
        uint32_t minorVersion = 0;
        for (const auto& version : properties.Properties[0].SupportedSchemaVersions)
        {
            if (version.Major >= majorVersion)
            {
                if ((version.Major > majorVersion) || (version.Minor > minorVersion))
                {
                    majorVersion = version.Major;
                    minorVersion = version.Minor;
                }
            }
        }

        g_schemaVersion = {majorVersion, minorVersion};
    });

    return g_schemaVersion;
}

void wsl::windows::common::hcs::GrantVmAccess(_In_ PCWSTR VmId, _In_ PCWSTR FilePath)
{
    WSL_LOG_DEBUG("HcsGrantVmAccess", TraceLoggingValue(VmId, "vmId"), TraceLoggingValue(FilePath, "filePath"));

    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED_MSG(::HcsGrantVmAccess(VmId, FilePath), "HcsGrantVmAccess(%ls, %ls)", VmId, FilePath);
}

void wsl::windows::common::hcs::ModifyComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Configuration, _In_opt_ HANDLE Identity)
{
    WSL_LOG_DEBUG("HcsModifyComputeSystem", TraceLoggingValue(Configuration, "configuration"));

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED_MSG(
        ::HcsModifyComputeSystem(ComputeSystem, operation.get(), Configuration, Identity), "HcsModifyComputeSystem (%ls)", Configuration);

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    if (FAILED(result))
    {
        // N.B. Logging is split into two calls because the configuration and error strings can be quite long.
        LOG_HR_MSG(result, "HcsModifyComputeSystem(%ls)", Configuration);
        THROW_HR_MSG(result, "HcsModifyComputeSystem failed (error string: %ls)", resultDocument.get());
    }
}

wsl::windows::common::hcs::unique_hcs_system wsl::windows::common::hcs::OpenComputeSystem(_In_ PCWSTR Id, _In_ DWORD RequestedAccess)
{
    WSL_LOG_DEBUG("HcsOpenComputeSystem", TraceLoggingValue(Id, "id"), TraceLoggingValue(RequestedAccess, "requestedAccess"));

    ExecutionContext context(Context::HCS);

    unique_hcs_system system;
    THROW_IF_FAILED_MSG(::HcsOpenComputeSystem(Id, RequestedAccess, &system), "HcsOpenComputeSystem(%ls)", Id);

    return system;
}

void wsl::windows::common::hcs::RegisterCallback(_In_ HCS_SYSTEM ComputeSystem, _In_ HCS_EVENT_CALLBACK Callback, _In_ void* Context)
{
    WSL_LOG_DEBUG("HcsSetComputeSystemCallback");

    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED(::HcsSetComputeSystemCallback(ComputeSystem, HcsEventOptionNone, Context, Callback));
}

void wsl::windows::common::hcs::RemoveScsiDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ ULONG Lun)
{
    ModifySettingRequest<void> request{};
    request.RequestType = ModifyRequestType::Remove;
    request.ResourcePath = c_scsiResourcePath + std::to_wstring(Lun);
    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

void wsl::windows::common::hcs::RevokeVmAccess(_In_ PCWSTR VmId, _In_ PCWSTR FilePath)
{
    WSL_LOG_DEBUG("HcsRevokeVmAccess", TraceLoggingValue(VmId, "vmId"), TraceLoggingValue(FilePath, "filePath"));

    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED_MSG(::HcsRevokeVmAccess(VmId, FilePath), "HcsRevokeVmAccess(%ls, %ls)", VmId, FilePath);
}

void wsl::windows::common::hcs::StartComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ LPCWSTR Configuration)
{
    WSL_LOG_DEBUG("HcsStartComputeSystem", TraceLoggingValue(Configuration, "configuration"));

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsStartComputeSystem(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    if (FAILED(result))
    {
        // N.B. Logging is split into two calls because the configuration and error strings can be quite long.
        LOG_HR_MSG(result, "HcsStartComputeSystem(%ls)", Configuration);
        THROW_HR_MSG(result, "HcsStartComputeSystem failed (error string: %ls)", resultDocument.get());
    }
}

void wsl::windows::common::hcs::TerminateComputeSystem(_In_ HCS_SYSTEM ComputeSystem)
{
    WSL_LOG_DEBUG("HcsTerminateComputeSystem");

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsTerminateComputeSystem(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(result, "HcsTerminateComputeSystem failed (error string: %ls)", resultDocument.get());
}

wsl::windows::common::hcs::unique_hcn_service_callback wsl::windows::common::hcs::RegisterServiceCallback(
    _In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context)
{
    WSL_LOG_DEBUG("HcsRegisterServiceCallback");

    ExecutionContext context(Context::HNS);

    unique_hcn_service_callback callbackHandle;
    THROW_IF_FAILED(::HcnRegisterServiceCallback(Callback, Context, &callbackHandle));

    return callbackHandle;
}

wsl::windows::common::hcs::unique_hcn_guest_network_service_callback wsl::windows::common::hcs::RegisterGuestNetworkServiceCallback(
    _In_ const unique_hcn_guest_network_service& GuestNetworkService, _In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context)
{
    WSL_LOG_DEBUG("HcsRegisterGuestNetworkServiceCallback");

    ExecutionContext context(Context::HNS);

    unique_hcn_guest_network_service_callback callbackHandle;
    THROW_IF_FAILED(::HcnRegisterGuestNetworkServiceCallback(GuestNetworkService.get(), Callback, Context, &callbackHandle));

    return callbackHandle;
}

bool wsl::windows::common::hcs::IsDisableVgpuSettingsSupported()
{
    static constexpr std::pair<uint32_t, uint32_t> c_schemaVersionNickel{2, 7};

    // See if the Windows version has the required platform change.
    return ((GetSchemaVersion() >= c_schemaVersionNickel) && (wsl::windows::common::helpers::GetWindowsVersion().BuildNumber >= 22545));
}