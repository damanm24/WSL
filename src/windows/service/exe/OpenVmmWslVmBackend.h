// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IWslVmBackend.h"
#include "TtrpcClient.h"
#include <filesystem>

/// <summary>
/// IWslVmBackend implementation that uses OpenVMM (openvmm.exe) as the VMM backend.
/// Spawns openvmm.exe in ttrpc orchestration mode and manages the VM lifecycle via ttrpc RPCs.
/// </summary>
class OpenVmmWslVmBackend : public IWslVmBackend
{
public:
    OpenVmmWslVmBackend() = default;
    ~OpenVmmWslVmBackend() override;

    NON_COPYABLE(OpenVmmWslVmBackend);
    NON_MOVABLE(OpenVmmWslVmBackend);

    // Lifecycle
    void Create(_In_ const CreateParams& Params, _In_ PCWSTR Config) override;
    void Start() override;
    void Terminate() override;
    void RegisterExitCallback(_In_ ExitCallback Callback) override;
    GUID GetRuntimeId() const override;

    // Guest Communication
    wil::unique_socket ListenForGuestConnection(_In_ ULONG Port) override;
    std::optional<wil::unique_socket> AcceptGuestConnection(
        _In_ SOCKET ListenSocket, _In_ DWORD TimeoutMs, _In_ HANDLE CancellationEvent) override;

    // Disks
    void AttachVhd(_In_ PCWSTR DiskPath, _In_ ULONG Lun, _In_ bool ReadOnly) override;
    void AttachPassThroughDisk(_In_ PCWSTR DiskPath, _In_ ULONG Lun) override;
    void DetachDisk(_In_ ULONG Lun) override;
    void GrantVmAccess(_In_ PCWSTR FilePath) override;
    void RevokeVmAccess(_In_ PCWSTR FilePath) override;

    // Shares
    void AddPlan9Share(
        _In_ PCWSTR Name,
        _In_ PCWSTR AccessName,
        _In_ PCWSTR Path,
        _In_ ULONG Port,
        _In_ wsl::windows::common::hcs::Plan9ShareFlags Flags,
        _In_opt_ HANDLE UserToken) override;

    // GPU
    bool AddGpu() override;

    // Devices
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
        _Inout_opt_ wsl::windows::common::hcs::unique_hcn_network* NatNetwork) override;

private:
    // Launch the openvmm.exe process and configure the VM via ttrpc.
    void LaunchOpenVmm();

    // Monitor the openvmm process and invoke the exit callback on termination.
    void WatchProcessExit();

    std::wstring m_machineId;
    GUID m_runtimeId{};
    ExitCallback m_exitCallback;

    // OpenVMM process management.
    wil::unique_handle m_processHandle;
    wil::unique_handle m_jobObject;
    std::thread m_processWatchThread;

    // ttrpc client for VM lifecycle management.
    std::filesystem::path m_ttrpcSocketPath;
    std::unique_ptr<TtrpcClient> m_ttrpcClient;

    // Vsock bridge path for guest communication (replaces HvSocket).
    std::filesystem::path m_vsockBridgePath;

    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};
};
