// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "hcs.hpp"
#include "WslCoreConfig.h"
#include "INetworkingEngine.h"
#include "GnsChannel.h"
#include "GuestDeviceManager.h"
#include "helpers.hpp"

/// <summary>
/// Abstract interface for WSL VM backend implementations.
/// This decouples WslCoreVm from the specific VM technology (HCS or OpenVMM)
/// used to manage the underlying virtual machine.
/// </summary>
class IWslVmBackend
{
public:
    virtual ~IWslVmBackend() = default;

    /// <summary>
    /// Callback type for VM exit notification.
    /// The parameter is the exit details string (may be empty).
    /// </summary>
    using ExitCallback = std::function<void(std::wstring)>;

    /// <summary>
    /// Configuration passed to the backend during creation.
    /// </summary>
    struct CreateParams
    {
        const GUID& VmId;
        std::wstring MachineId;
        wil::shared_handle UserToken;
        wsl::windows::common::helpers::WindowsVersion WindowsVersion;
    };

    // ========================================================================
    // VM Lifecycle
    // ========================================================================

    /// <summary>
    /// Creates and starts the virtual machine.
    /// The Config parameter is backend-specific (JSON for HCS, ttrpc config for OpenVMM).
    /// After this call returns successfully, the VM is running and ready to accept connections.
    /// </summary>
    virtual void CreateAndStart(_In_ const CreateParams& Params, _In_ PCWSTR Config) = 0;

    /// <summary>
    /// Terminates the virtual machine forcefully.
    /// </summary>
    virtual void Terminate() = 0;

    /// <summary>
    /// Registers a callback invoked when the VM exits (gracefully or unexpectedly).
    /// Must be called after CreateAndStart.
    /// </summary>
    virtual void RegisterExitCallback(_In_ ExitCallback Callback) = 0;

    /// <summary>
    /// Gets the runtime ID of the VM (assigned by the backend).
    /// </summary>
    virtual GUID GetRuntimeId() const = 0;

    // ========================================================================
    // Guest Communication
    // ========================================================================

    /// <summary>
    /// Creates a socket listening for connections from the guest on the specified port.
    /// For HCS, this is an HVSocket listener. For OpenVMM, this may use Unix domain sockets with a relay.
    /// </summary>
    virtual wil::unique_socket ListenForGuestConnection(_In_ ULONG Port) = 0;

    /// <summary>
    /// Accepts a connection on a listening socket with support for cancellation.
    /// Returns nullopt if the cancellation event is signaled before a connection arrives.
    /// </summary>
    virtual std::optional<wil::unique_socket> AcceptGuestConnection(
        _In_ SOCKET ListenSocket, _In_ DWORD TimeoutMs, _In_ HANDLE CancellationEvent) = 0;

    // ========================================================================
    // Disk Management
    // ========================================================================

    /// <summary>
    /// Attaches a VHD/VHDX disk to the VM at the specified SCSI LUN.
    /// </summary>
    virtual void AttachVhd(_In_ PCWSTR DiskPath, _In_ ULONG Lun, _In_ bool ReadOnly) = 0;

    /// <summary>
    /// Attaches a physical (passthrough) disk to the VM at the specified SCSI LUN.
    /// </summary>
    virtual void AttachPassThroughDisk(_In_ PCWSTR DiskPath, _In_ ULONG Lun) = 0;

    /// <summary>
    /// Detaches a SCSI disk from the VM by LUN.
    /// </summary>
    virtual void DetachDisk(_In_ ULONG Lun) = 0;

    /// <summary>
    /// Grants the VM worker process access to a file (e.g., VHD).
    /// For HCS this calls HcsGrantVmAccess. Other backends may be no-ops.
    /// </summary>
    virtual void GrantVmAccess(_In_ PCWSTR FilePath) = 0;

    /// <summary>
    /// Revokes previously granted VM access to a file.
    /// </summary>
    virtual void RevokeVmAccess(_In_ PCWSTR FilePath) = 0;

    // ========================================================================
    // File Sharing
    // ========================================================================

    /// <summary>
    /// Adds a Plan9 file share to the VM.
    /// </summary>
    virtual void AddPlan9Share(
        _In_ PCWSTR Name,
        _In_ PCWSTR AccessName,
        _In_ PCWSTR Path,
        _In_ ULONG Port,
        _In_ wsl::windows::common::hcs::Plan9ShareFlags Flags,
        _In_opt_ HANDLE UserToken = nullptr) = 0;

    // ========================================================================
    // VM Modification
    // ========================================================================

    /// <summary>
    /// Adds GPU mirror passthrough to the VM.
    /// Backends that do not support GPU should return false.
    /// </summary>
    virtual bool AddGpu() = 0;

    // ========================================================================
    // Device Management
    // ========================================================================

    /// <summary>
    /// Gets the guest device manager for this VM.
    /// The device manager handles hot-add/remove of virtio devices.
    /// </summary>
    virtual std::shared_ptr<GuestDeviceManager> GetGuestDeviceManager() = 0;

    // ========================================================================
    // Networking
    // ========================================================================

    /// <summary>
    /// Creates the networking engine appropriate for this backend and networking mode.
    /// The backend is responsible for choosing the right networking implementation.
    /// Returns nullptr if networking mode is None.
    /// </summary>
    /// <param name="NatNetwork">Pre-created NAT network (may be null if not using NAT mode).</param>
    virtual std::unique_ptr<wsl::core::INetworkingEngine> CreateNetworkingEngine(
        _In_ wsl::core::NetworkingMode Mode,
        _In_ wsl::core::GnsChannel&& GnsChannel,
        _Inout_ wsl::core::Config& Config,
        _In_ REFGUID RuntimeId,
        _In_ wil::unique_socket&& DnsTunnelingSocket,
        _In_ const std::shared_ptr<GuestDeviceManager>& DeviceManager,
        _In_ const wil::shared_handle& UserToken,
        _Inout_opt_ wsl::windows::common::hcs::unique_hcn_network* NatNetwork = nullptr) = 0;
};
