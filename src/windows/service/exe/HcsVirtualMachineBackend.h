// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IVirtualMachineBackend.h"
#include "hcs.hpp"

class GuestDeviceManager;
struct IPlan9FileSystem;

namespace wsl::windows::common::vm::hcs {

struct VmConfiguration
{
    VmDescription Description;
    wsl::windows::common::hcs::ComputeSystem Settings;
};

VmConfiguration BuildConfiguration(const VmCreateRequest& Request);

} // namespace wsl::windows::common::vm::hcs

class HcsVirtualMachineBackend : public IVirtualMachineBackend
{
public:
    ~HcsVirtualMachineBackend() noexcept override;

    static std::unique_ptr<HcsVirtualMachineBackend> Create(const VmCreateRequest& Request);

    static VmPlatformCapabilities QueryCapabilities();

    VmPlatformCapabilities GetCapabilities() const override;
    wil::unique_handle GetTerminationEvent() const override;

    void Start() override;
    void Terminate() override;
    void CancelPendingOperations() noexcept override;

    VmGuestListener CreateGuestListener(GuestServicePort Port) override;
    wil::unique_socket AcceptGuestConnection(VmListenerId Listener) override;
    wil::unique_socket ConnectGuest(GuestServicePort Port) override;
    void CloseGuestListener(VmListenerId Listener) override;

    VmDiskAttachment AttachDisk(const VmDiskRequest& Request) override;
    void DetachDisk(VmDiskId Disk) override;

    VmFileSystemDevice CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request) override;
    VmFileSystemShare AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request) override;
    void RemoveFileSystemShare(VmShareId Share) override;

    VmNetworkAttachment AddNetworkAdapter(const VmNetworkAdapterRequest& Request) override;
    VmPortBinding BindPort(VmDeviceId Device, const VmPortBindingRequest& Request) override;
    void UnbindPort(VmPortBindingId Binding) override;

private:
    HcsVirtualMachineBackend();
    void Initialize(const VmCreateRequest& Request);
    static void CALLBACK OnSystemEvent(HCS_EVENT* Event, void* Context) noexcept;
    NON_COPYABLE(HcsVirtualMachineBackend);
    NON_MOVABLE(HcsVirtualMachineBackend);

    struct State
    {
        ~State() noexcept;
        void CloseFileSystemDevices() noexcept;

        struct Plan9Device
        {
            wil::com_ptr<IPlan9FileSystem> Server;
            std::optional<GUID> InstanceId;
        };

        struct FileSystemDevice
        {
            VmFileSystemDevice Device;
            VmFileSystemDeviceRequest Request;
            // Prepared virtio-fs, physical virtio-fs device, or owned Plan9 server.
            std::variant<std::monostate, GUID, Plan9Device> Resource;
        };

        struct AttachedDisk
        {
            VmDiskAttachment Attachment;
            std::wstring Path;
            bool IsPhysical = false;
            bool IsUserDisk = false;
            bool AccessGranted = false;
            bool WasOnline = false;
            wil::unique_hfile BackingFile;
            std::chrono::milliseconds OperationTimeout;
        };

        wil::srwlock m_lock;
        VmDescription m_description;
        std::wstring m_configuration;
        std::wstring m_machineId;
        wil::unique_event m_terminatingEvent{wil::EventOptions::ManualReset};
        _Guarded_by_(m_lock) std::map<std::uint64_t, AttachedDisk> m_attachedDisks;
        _Guarded_by_(m_lock) std::uint64_t m_nextDiskId = 1;
        // Closing the system drains callbacks before their event and context are destroyed.
        _Guarded_by_(m_lock) wsl::windows::common::hcs::unique_hcs_system m_system;
        _Guarded_by_(m_lock) std::unique_ptr<GuestDeviceManager> m_guestDeviceManager;
        _Guarded_by_(m_lock) std::map<std::uint64_t, FileSystemDevice> m_fileSystemDevices;
        _Guarded_by_(m_lock) std::uint64_t m_nextDeviceId = 1;
    };

    std::unique_ptr<State> m_state;
};