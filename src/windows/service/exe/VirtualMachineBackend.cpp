// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "IVirtualMachineBackend.h"
#include "OpenVmmVirtualMachineBackend.h"

namespace {

constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

} // namespace

void wsl::windows::common::vm::validation::ValidateFeature(VmFeatureRequest Request, PCWSTR Setting)
{
    switch (Request)
    {
    case VmFeatureRequest::Disabled:
    case VmFeatureRequest::Preferred:
        return;
    case VmFeatureRequest::Required:
        THROW_HR_MSG(c_notSupported, "OpenVMM does not support the required %ls setting", Setting);
    }

    THROW_HR(E_INVALIDARG);
}

void wsl::windows::common::vm::validation::ValidateUnsupportedSelection(VmSelectionPolicy Policy)
{
    THROW_HR_IF(E_INVALIDARG, Policy != VmSelectionPolicy::Required && Policy != VmSelectionPolicy::Preferred);
    THROW_HR_IF(c_notSupported, Policy == VmSelectionPolicy::Required);
}

void wsl::windows::common::vm::validation::ValidatePath(const std::filesystem::path& Path, PCWSTR Backend)
{
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        Path.empty() || !Path.is_absolute() || Path.native().find(L'\0') != std::wstring::npos,
        "%ls requires an absolute, nonempty host path",
        Backend);
}

const VmVirtualDiskSource& wsl::windows::common::vm::validation::ValidateDiskRequest(const VmDiskRequest& Request, UINT32 MaximumDisks)
{
    const auto* source = std::get_if<VmVirtualDiskSource>(&Request.Source);
    THROW_HR_IF(c_notSupported, source == nullptr);
    ValidatePath(source->Path, L"OpenVMM");
    switch (source->Format)
    {
    case VmDiskFormat::Vhd:
        THROW_HR_IF(E_INVALIDARG, _wcsicmp(source->Path.extension().c_str(), L".vhd") != 0);
        break;
    case VmDiskFormat::Vhdx:
        THROW_HR_IF(E_INVALIDARG, _wcsicmp(source->Path.extension().c_str(), L".vhdx") != 0);
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }

    if (Request.Placement)
    {
        THROW_HR_IF(c_notSupported, Request.Placement->Address.Controller != 0 || Request.Placement->Address.Lun >= MaximumDisks);
    }

    return *source;
}

void wsl::windows::common::vm::validation::ValidateConsolePath(
    const std::filesystem::path& Path, PCWSTR Backend, HRESULT Error, bool RequireName)
{
    ValidatePath(Path, Backend);
    THROW_HR_IF_MSG(
        Error,
        !Path.native().starts_with(L"\\\\.\\pipe\\") || (RequireName && Path.filename().empty()),
        "%ls consoles require a caller-provided named pipe",
        Backend);
}

void wsl::windows::common::vm::validation::ValidateName(std::wstring_view Name, PCWSTR Description)
{
    THROW_HR_IF_MSG(
        E_INVALIDARG, Name.empty() || Name.find(L'\0') != std::wstring_view::npos, "%ls must be nonempty and cannot contain NUL", Description);
}

void wsl::windows::common::vm::validation::ValidateResourceId(UINT64 Value, const GUID& VmId, const VmInstanceId& Owner)
{
    THROW_HR_IF(E_INVALIDARG, Value == 0 || !IsEqualGUID(VmId, Owner.VmId));
}

std::unique_ptr<IVirtualMachineBackend> CreateVirtualMachineBackend(BackendKind Kind, const VmCreateRequest& Request)
{
    switch (Kind)
    {
    case BackendKind::OpenVmm:
        return OpenVmmVirtualMachineBackend::Create(Request);

    case BackendKind::Hcs:
        THROW_HR(E_NOTIMPL);
    }

    THROW_HR(E_INVALIDARG);
}

VmPlatformCapabilities QueryVirtualMachineBackendCapabilities(BackendKind Kind)
{
    switch (Kind)
    {
    case BackendKind::OpenVmm:
        return OpenVmmVirtualMachineBackend::QueryCapabilities();

    case BackendKind::Hcs:
        THROW_HR(E_NOTIMPL);
    }

    THROW_HR(E_INVALIDARG);
}