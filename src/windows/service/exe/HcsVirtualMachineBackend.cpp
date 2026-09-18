// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HcsVirtualMachineBackend.h"
#include "hvsocket.hpp"

namespace validation = wsl::windows::common::vm::validation;

namespace {

namespace schema = wsl::windows::common::hcs;

constexpr UINT64 c_mib = 1024 * 1024;
constexpr UINT64 c_memoryGranularity = 2 * c_mib;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

std::wstring GetVmbFsPath(const std::filesystem::path& Path, const std::filesystem::path& Root)
{
    const auto path = Path.lexically_normal();
    const auto root = Root.lexically_normal();
    auto position = path.begin();
    for (const auto& component : root)
    {
        if (component.empty())
        {
            continue;
        }

        THROW_HR_IF(E_INVALIDARG, position == path.end() || _wcsicmp(component.c_str(), position->c_str()) != 0);
        ++position;
    }

    std::filesystem::path relative;
    for (; position != path.end(); ++position)
    {
        relative /= *position;
    }
    THROW_HR_IF(E_INVALIDARG, relative.empty() || relative.filename().empty());
    return L"\\" + relative.native();
}

} // namespace

wsl::windows::common::vm::hcs::VmConfiguration wsl::windows::common::vm::hcs::BuildConfiguration(const VmCreateRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, IsEqualGUID(Request.Identity.VmId, GUID_NULL) || !Request.Identity.UserToken);
    THROW_HR_IF(E_INVALIDARG, Request.Owner.empty() || Request.Owner.find(L'\0') != std::wstring::npos);
    THROW_HR_IF(E_INVALIDARG, Request.Processor.Count == 0 || Request.Memory.SizeBytes == 0);
    THROW_HR_IF(E_INVALIDARG, Request.Memory.SizeBytes % c_memoryGranularity != 0);
    validation::ValidatePath(Request.Boot.KernelPath, L"HCS");
    if (!Request.Boot.InitrdPath.empty())
    {
        validation::ValidatePath(Request.Boot.InitrdPath, L"HCS");
    }
    THROW_HR_IF(E_INVALIDARG, Request.Boot.KernelCommandLine.find(L'\0') != std::wstring::npos);

    VmConfiguration configuration{};
    auto& description = configuration.Description;
    description.Identity = Request.Identity;
    description.Backend = BackendKind::Hcs;
    description.Processor.Count = Request.Processor.Count;
    description.Memory.SizeBytes = Request.Memory.SizeBytes;
    description.Boot.KernelCommandLine = Request.Boot.KernelCommandLine;
    description.Boot.Method = Request.Boot.Method;
    if (description.Boot.Method == VmBootMethod::Automatic)
    {
        description.Boot.Method = wsl::shared::Arm64 ? VmBootMethod::Uefi : VmBootMethod::LinuxDirect;
    }

    auto& settings = configuration.Settings;
    settings.Owner = Request.Owner;
    settings.ShouldTerminateOnLastHandleClosed = true;
    settings.SchemaVersion = {2, 3};
    auto& vm = settings.VirtualMachine;
    vm.StopOnReset = true;
    vm.Chipset.UseUtc = true;
    switch (description.Boot.Method)
    {
    case VmBootMethod::LinuxDirect:
        THROW_HR_IF(c_notSupported, wsl::shared::Arm64);
        THROW_HR_IF(E_INVALIDARG, Request.Boot.UefiRootPath.has_value());
        vm.Chipset.LinuxKernelDirect =
            schema::LinuxKernelDirect{Request.Boot.KernelPath.native(), Request.Boot.InitrdPath.native(), Request.Boot.KernelCommandLine};
        break;
    case VmBootMethod::Uefi:
    {
        const auto root = Request.Boot.UefiRootPath.value_or(Request.Boot.KernelPath.parent_path());
        validation::ValidatePath(root, L"HCS");
        // Firmware consumes the caller's initrd= argument, not InitrdPath.
        if (!Request.Boot.InitrdPath.empty())
        {
            GetVmbFsPath(Request.Boot.InitrdPath, root);
        }
        vm.Chipset.Uefi = schema::Uefi{schema::UefiBootEntry{
            schema::UefiBootDevice::VmbFs,
            root.lexically_normal().native(),
            GetVmbFsPath(Request.Boot.KernelPath, root),
            Request.Boot.KernelCommandLine}};
        break;
    }
    default:
        THROW_HR(E_INVALIDARG);
    }

    vm.ComputeTopology.Processor.Count = description.Processor.Count;
    vm.ComputeTopology.Memory.SizeInMB = description.Memory.SizeBytes / c_mib;

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(Request.Identity.UserToken.get());
    vm.Devices.HvSocket = schema::CreateHvSocketConfiguration(tokenUser->User.Sid);
    return configuration;
}

HcsVirtualMachineBackend::HcsVirtualMachineBackend() : m_state(std::make_unique<State>())
{
}

HcsVirtualMachineBackend::~HcsVirtualMachineBackend() noexcept = default;

std::unique_ptr<HcsVirtualMachineBackend> HcsVirtualMachineBackend::Create(const VmCreateRequest& Request)
{
    auto backend = std::unique_ptr<HcsVirtualMachineBackend>{new HcsVirtualMachineBackend{}};
    backend->Initialize(Request);
    return backend;
}

void HcsVirtualMachineBackend::Initialize(const VmCreateRequest& Request)
{
    auto configuration = wsl::windows::common::vm::hcs::BuildConfiguration(Request);
    const auto id = wsl::shared::string::GuidToString<wchar_t>(Request.Identity.VmId, wsl::shared::string::GuidToStringFlags::None);
    m_state->m_configuration = wsl::shared::ToJsonW(configuration.Settings);
    m_state->m_description = std::move(configuration.Description);
    auto lock = m_state->m_lock.lock_exclusive();
    m_state->m_system = schema::CreateComputeSystem(id.c_str(), m_state->m_configuration.c_str());
    schema::RegisterCallback(m_state->m_system.get(), OnSystemEvent, m_state.get());
}

void CALLBACK HcsVirtualMachineBackend::OnSystemEvent(HCS_EVENT* Event, void* Context) noexcept
{
    if (Event->Type == HcsEventSystemExited || Event->Type == HcsEventServiceDisconnect)
    {
        const auto state = static_cast<State*>(Context);
        LOG_IF_WIN32_BOOL_FALSE(SetEvent(state->m_terminatingEvent.get()));
    }
}

VmPlatformCapabilities HcsVirtualMachineBackend::QueryCapabilities()
{
    THROW_HR(E_NOTIMPL);
}

VmPlatformCapabilities HcsVirtualMachineBackend::GetCapabilities() const
{
    THROW_HR(E_NOTIMPL);
}

wil::unique_handle HcsVirtualMachineBackend::GetTerminationEvent() const
{
    wil::unique_handle event;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), m_state->m_terminatingEvent.get(), GetCurrentProcess(), event.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));
    return event;
}

void HcsVirtualMachineBackend::Start()
{
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_system);
    schema::StartComputeSystem(m_state->m_system.get(), m_state->m_configuration.c_str());
}

void HcsVirtualMachineBackend::Terminate()
{
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_system);
    schema::TerminateComputeSystem(m_state->m_system.get());
    m_state->m_system.reset();
    CloseGuestListeners();
    // A system terminated before Start may not send an exit notification.
    m_state->m_terminatingEvent.SetEvent();
}

void HcsVirtualMachineBackend::CancelPendingOperations() noexcept
{
    CloseGuestListeners();
}

VmGuestListener HcsVirtualMachineBackend::CreateGuestListener(GuestServicePort Port)
{
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_system);
    return RegisterGuestListener(m_state->m_description.Identity, Port);
}

wil::unique_socket HcsVirtualMachineBackend::AcceptGuestConnection(VmListenerId Listener)
{
    return AcceptGuestListenerConnection(Listener, m_state->m_description.Identity);
}

wil::unique_socket HcsVirtualMachineBackend::ConnectGuest(GuestServicePort Port)
{
    auto lock = m_state->m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_system);
    return wsl::windows::common::hvsocket::Connect(m_state->m_description.Identity.VmId, Port.Value);
}

void HcsVirtualMachineBackend::CloseGuestListener(VmListenerId Listener)
{
    RemoveGuestListener(Listener, m_state->m_description.Identity);
}

VmDiskAttachment HcsVirtualMachineBackend::AttachDisk(const VmDiskRequest& Request)
{
    THROW_HR(E_NOTIMPL);
}

void HcsVirtualMachineBackend::DetachDisk(VmDiskId Disk)
{
    THROW_HR(E_NOTIMPL);
}

VmFileSystemDevice HcsVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request)
{
    THROW_HR(E_NOTIMPL);
}

VmFileSystemShare HcsVirtualMachineBackend::AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request)
{
    THROW_HR(E_NOTIMPL);
}

void HcsVirtualMachineBackend::RemoveFileSystemShare(VmShareId Share)
{
    THROW_HR(E_NOTIMPL);
}

VmNetworkAttachment HcsVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest& Request)
{
    THROW_HR(E_NOTIMPL);
}

VmPortBinding HcsVirtualMachineBackend::BindPort(VmDeviceId Device, const VmPortBindingRequest& Request)
{
    THROW_HR(E_NOTIMPL);
}

void HcsVirtualMachineBackend::UnbindPort(VmPortBindingId Binding)
{
    THROW_HR(E_NOTIMPL);
}
