// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IWslVmBackend.h"

/// <summary>
/// HCS (Hyper-V Compute Service) implementation of the WSL VM backend.
/// This wraps all HCS API calls for VM lifecycle management, disk operations,
/// file sharing, and guest communication via HVSocket.
/// </summary>
class HcsWslVmBackend : public IWslVmBackend
{
    NON_COPYABLE(HcsWslVmBackend);
    NON_MOVABLE(HcsWslVmBackend);

public:
    HcsWslVmBackend() = default;
    ~HcsWslVmBackend() override = default;

    // VM Lifecycle
    void CreateAndStart(_In_ const CreateParams& Params, _In_ PCWSTR Config) override;
    void Terminate() override;
    void RegisterExitCallback(_In_ ExitCallback Callback) override;
    GUID GetRuntimeId() const override;

    // Guest Communication
    wil::unique_socket ListenForGuestConnection(_In_ ULONG Port) override;
    std::optional<wil::unique_socket> AcceptGuestConnection(
        _In_ SOCKET ListenSocket, _In_ DWORD TimeoutMs, _In_ HANDLE CancellationEvent) override;

    // Disk Management
    void AttachVhd(_In_ PCWSTR DiskPath, _In_ ULONG Lun, _In_ bool ReadOnly) override;
    void AttachPassThroughDisk(_In_ PCWSTR DiskPath, _In_ ULONG Lun) override;
    void DetachDisk(_In_ ULONG Lun) override;
    void GrantVmAccess(_In_ PCWSTR FilePath) override;
    void RevokeVmAccess(_In_ PCWSTR FilePath) override;

    // File Sharing
    void AddPlan9Share(
        _In_ PCWSTR Name,
        _In_ PCWSTR AccessName,
        _In_ PCWSTR Path,
        _In_ ULONG Port,
        _In_ wsl::windows::common::hcs::Plan9ShareFlags Flags,
        _In_opt_ HANDLE UserToken = nullptr) override;

    // VM Modification
    bool AddGpu() override;

    // Device Management
    std::shared_ptr<GuestDeviceManager> GetGuestDeviceManager() override;

    // Networking
    std::unique_ptr<wsl::core::INetworkingEngine> CreateNetworkingEngine(
        _In_ wsl::core::NetworkingMode Mode,
        _In_ wsl::core::GnsChannel&& GnsChannel,
        _Inout_ wsl::core::Config& Config,
        _In_ REFGUID RuntimeId,
        _In_ wil::unique_socket&& DnsTunnelingSocket,
        _In_ const std::shared_ptr<GuestDeviceManager>& DeviceManager,
        _In_ const wil::shared_handle& UserToken,
        _Inout_opt_ wsl::windows::common::hcs::unique_hcn_network* NatNetwork = nullptr) override;

    /// <summary>
    /// Gets the underlying HCS system handle. Used during the transition period
    /// where some code still needs direct HCS access.
    /// </summary>
    HCS_SYSTEM GetSystem() const { return m_system.get(); }

private:
    static void CALLBACK s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context);

    wsl::windows::common::hcs::unique_hcs_system m_system;
    GUID m_runtimeId{};
    std::wstring m_machineId;
    ExitCallback m_exitCallback;
    std::shared_ptr<GuestDeviceManager> m_guestDeviceManager;
};
