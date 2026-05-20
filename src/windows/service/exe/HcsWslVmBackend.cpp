// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HcsWslVmBackend.h"
#include "NatNetworking.h"
#include "BridgedNetworking.h"
#include "MirroredNetworking.h"
#include "VirtioNetworking.h"

using namespace wsl::windows::common;

void HcsWslVmBackend::Create(_In_ const CreateParams& Params, _In_ PCWSTR Config)
{
    m_machineId = Params.MachineId;
    m_config = Config;

    // Create the compute system and retrieve the runtime ID.
    m_system = wsl::windows::common::hcs::CreateComputeSystem(m_machineId.c_str(), Config);
    m_runtimeId = wsl::windows::common::hcs::GetRuntimeId(m_system.get());
    WI_ASSERT(IsEqualGUID(Params.VmId, m_runtimeId));

    // Initialize the guest device manager.
    m_guestDeviceManager = std::make_shared<GuestDeviceManager>(m_machineId, m_runtimeId);
}

void HcsWslVmBackend::Start()
{
    // Start the compute system. If startup fails, release the system handle so
    // Terminate() does not attempt to terminate a VM that never ran.
    try
    {
        wsl::windows::common::hcs::StartComputeSystem(m_system.get(), m_config.c_str());
    }
    catch (...)
    {
        m_system.reset();
        throw;
    }
}

void HcsWslVmBackend::Terminate()
{
    if (m_system)
    {
        wsl::windows::common::hcs::TerminateComputeSystem(m_system.get());
    }
}

void HcsWslVmBackend::RegisterExitCallback(_In_ ExitCallback Callback)
{
    m_exitCallback = std::move(Callback);
    wsl::windows::common::hcs::RegisterCallback(m_system.get(), s_OnExit, this);
}

GUID HcsWslVmBackend::GetRuntimeId() const
{
    return m_runtimeId;
}

wil::unique_socket HcsWslVmBackend::ListenForGuestConnection(_In_ ULONG Port)
{
    return wsl::windows::common::hvsocket::Listen(m_runtimeId, Port);
}

std::optional<wil::unique_socket> HcsWslVmBackend::AcceptGuestConnection(
    _In_ SOCKET ListenSocket, _In_ DWORD TimeoutMs, _In_ HANDLE CancellationEvent)
{
    return wsl::windows::common::hvsocket::CancellableAccept(ListenSocket, TimeoutMs, CancellationEvent);
}

void HcsWslVmBackend::AttachVhd(_In_ PCWSTR DiskPath, _In_ ULONG Lun, _In_ bool ReadOnly)
{
    wsl::windows::common::hcs::AddVhd(m_system.get(), DiskPath, Lun, ReadOnly);
}

void HcsWslVmBackend::AttachPassThroughDisk(_In_ PCWSTR DiskPath, _In_ ULONG Lun)
{
    wsl::windows::common::hcs::AddPassThroughDisk(m_system.get(), DiskPath, Lun);
}

void HcsWslVmBackend::DetachDisk(_In_ ULONG Lun)
{
    wsl::windows::common::hcs::RemoveScsiDisk(m_system.get(), Lun);
}

void HcsWslVmBackend::GrantVmAccess(_In_ PCWSTR FilePath)
{
    wsl::windows::common::hcs::GrantVmAccess(m_machineId.c_str(), FilePath);
}

void HcsWslVmBackend::RevokeVmAccess(_In_ PCWSTR FilePath)
{
    wsl::windows::common::hcs::RevokeVmAccess(m_machineId.c_str(), FilePath);
}

void HcsWslVmBackend::AddPlan9Share(
    _In_ PCWSTR Name,
    _In_ PCWSTR AccessName,
    _In_ PCWSTR Path,
    _In_ ULONG Port,
    _In_ wsl::windows::common::hcs::Plan9ShareFlags Flags,
    _In_opt_ HANDLE UserToken)
{
    wsl::windows::common::hcs::AddPlan9Share(m_system.get(), Name, AccessName, Path, Port, Flags, UserToken);
}

bool HcsWslVmBackend::AddGpu()
{
    hcs::ModifySettingRequest<hcs::GpuConfiguration> gpuRequest{};
    gpuRequest.ResourcePath = L"VirtualMachine/ComputeTopology/Gpu";
    gpuRequest.RequestType = hcs::ModifyRequestType::Update;
    gpuRequest.Settings.AssignmentMode = hcs::GpuAssignmentMode::Mirror;
    gpuRequest.Settings.AllowVendorExtension = true;
    if (wsl::windows::common::hcs::IsDisableVgpuSettingsSupported())
    {
        gpuRequest.Settings.DisableGdiAcceleration = true;
        gpuRequest.Settings.DisablePresentation = true;
    }

    wsl::windows::common::hcs::ModifyComputeSystem(m_system.get(), wsl::shared::ToJsonW(gpuRequest).c_str());
    return true;
}

std::shared_ptr<GuestDeviceManager> HcsWslVmBackend::GetGuestDeviceManager()
{
    return m_guestDeviceManager;
}

std::unique_ptr<wsl::core::INetworkingEngine> HcsWslVmBackend::CreateNetworkingEngine(
    _In_ wsl::core::NetworkingMode Mode,
    _In_ wsl::core::GnsChannel&& GnsChannel,
    _Inout_ wsl::core::Config& Config,
    _In_ REFGUID RuntimeId,
    _In_ wil::unique_socket&& DnsTunnelingSocket,
    _In_ const std::shared_ptr<GuestDeviceManager>& DeviceManager,
    _In_ const wil::shared_handle& UserToken,
    _Inout_opt_ wsl::windows::common::hcs::unique_hcn_network* NatNetwork)
{
    std::unique_ptr<wsl::core::INetworkingEngine> engine;

    if (Mode == wsl::core::NetworkingMode::Mirrored)
    {
        engine = std::make_unique<wsl::core::MirroredNetworking>(
            m_system.get(), std::move(GnsChannel), Config, RuntimeId, std::move(DnsTunnelingSocket));
    }
    else if (Mode == wsl::core::NetworkingMode::Nat)
    {
        WI_ASSERT(NatNetwork && *NatNetwork);
        engine = std::make_unique<wsl::core::NatNetworking>(
            m_system.get(), std::move(*NatNetwork), std::move(GnsChannel), Config, std::move(DnsTunnelingSocket));
    }
    else if (Mode == wsl::core::NetworkingMode::VirtioProxy)
    {
        wsl::core::VirtioNetworkingFlags flags = wsl::core::VirtioNetworkingFlags::Ipv6;
        WI_SetFlagIf(flags, wsl::core::VirtioNetworkingFlags::LocalhostRelay, Config.EnableLocalhostRelay);
        WI_SetFlagIf(flags, wsl::core::VirtioNetworkingFlags::DnsTunneling, Config.EnableDnsTunneling);
        DnsTunnelingSocket.reset();
        engine = std::make_unique<wsl::core::VirtioNetworking>(
            std::move(GnsChannel), flags, LX_INIT_RESOLVCONF_FULL_HEADER, DeviceManager, UserToken);
    }
    else if (Mode == wsl::core::NetworkingMode::Bridged)
    {
        engine = std::make_unique<wsl::core::BridgedNetworking>(m_system.get(), Config);
    }

    return engine;
}

void CALLBACK HcsWslVmBackend::s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context)
{
    auto* self = static_cast<HcsWslVmBackend*>(Context);
    if (self && self->m_exitCallback)
    {
        std::wstring exitDetails;
        if (Event->EventData)
        {
            exitDetails = Event->EventData;
        }

        self->m_exitCallback(std::move(exitDetails));
    }
}
