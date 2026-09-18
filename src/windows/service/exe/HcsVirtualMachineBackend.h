// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IVirtualMachineBackend.h"
#include "hcs.hpp"

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
        wil::srwlock m_lock;
        VmDescription m_description;
        std::wstring m_configuration;
        wil::unique_event m_terminatingEvent{wil::EventOptions::ManualReset};
        // Closing the system drains callbacks before their event and context are destroyed.
        _Guarded_by_(m_lock) wsl::windows::common::hcs::unique_hcs_system m_system;
    };

    std::unique_ptr<State> m_state;
};