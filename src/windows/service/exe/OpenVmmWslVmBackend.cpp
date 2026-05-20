// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmWslVmBackend.h"

OpenVmmWslVmBackend::~OpenVmmWslVmBackend()
{
    Terminate();

    if (m_processWatchThread.joinable())
    {
        m_vmExitEvent.SetEvent();
        m_processWatchThread.join();
    }
}

void OpenVmmWslVmBackend::Create(_In_ const CreateParams& Params, _In_ PCWSTR /*Config*/)
{
    m_machineId = Params.MachineId;
    m_runtimeId = Params.VmId;
    // Note: GuestDeviceManager is HCS-specific (it calls HcsOpenComputeSystem).
    // OpenVMM device management goes through ttrpc, so no GuestDeviceManager here.
}

void OpenVmmWslVmBackend::Start()
{
    // TODO: Parse the config or build OpenVMM-specific config from CreateParams.
    // TODO: Launch openvmm.exe, connect via ttrpc, and create+resume the VM.
    LaunchOpenVmm();
}

void OpenVmmWslVmBackend::Terminate()
{
    if (m_ttrpcClient)
    {
        // TODO: Send VM teardown via ttrpc.
        m_ttrpcClient.reset();
    }

    if (m_processHandle)
    {
        TerminateProcess(m_processHandle.get(), 1);
        m_processHandle.reset();
    }
}

void OpenVmmWslVmBackend::RegisterExitCallback(_In_ ExitCallback Callback)
{
    m_exitCallback = std::move(Callback);
}

GUID OpenVmmWslVmBackend::GetRuntimeId() const
{
    return m_runtimeId;
}

wil::unique_socket OpenVmmWslVmBackend::ListenForGuestConnection(_In_ ULONG /*Port*/)
{
    // TODO: For OpenVMM, guest communication uses Unix domain sockets via the vsock bridge
    // rather than HvSockets. Create the appropriate listener here.
    THROW_HR(E_NOTIMPL);
}

std::optional<wil::unique_socket> OpenVmmWslVmBackend::AcceptGuestConnection(
    _In_ SOCKET /*ListenSocket*/, _In_ DWORD /*TimeoutMs*/, _In_ HANDLE /*CancellationEvent*/)
{
    // TODO: Accept on the Unix domain socket listener with cancellation support.
    THROW_HR(E_NOTIMPL);
}

void OpenVmmWslVmBackend::AttachVhd(_In_ PCWSTR /*DiskPath*/, _In_ ULONG /*Lun*/, _In_ bool /*ReadOnly*/)
{
    // TODO: Use ttrpc to hot-add a VHD to the VM.
    THROW_HR(E_NOTIMPL);
}

void OpenVmmWslVmBackend::AttachPassThroughDisk(_In_ PCWSTR /*DiskPath*/, _In_ ULONG /*Lun*/)
{
    // TODO: Use ttrpc to hot-add a pass-through disk.
    THROW_HR(E_NOTIMPL);
}

void OpenVmmWslVmBackend::DetachDisk(_In_ ULONG /*Lun*/)
{
    // TODO: Use ttrpc to detach a disk from the VM.
    THROW_HR(E_NOTIMPL);
}

void OpenVmmWslVmBackend::GrantVmAccess(_In_ PCWSTR /*FilePath*/)
{
    // OpenVMM runs as a user-mode process and accesses files directly.
    // No VM access grant is needed (unlike HCS which runs the VM in kernel space).
}

void OpenVmmWslVmBackend::RevokeVmAccess(_In_ PCWSTR /*FilePath*/)
{
    // No-op for OpenVMM (see GrantVmAccess).
}

void OpenVmmWslVmBackend::AddPlan9Share(
    _In_ PCWSTR /*Name*/,
    _In_ PCWSTR /*AccessName*/,
    _In_ PCWSTR /*Path*/,
    _In_ ULONG /*Port*/,
    _In_ wsl::windows::common::hcs::Plan9ShareFlags /*Flags*/,
    _In_opt_ HANDLE /*UserToken*/)
{
    // TODO: Configure Plan9 share via ttrpc or virtio-fs.
    THROW_HR(E_NOTIMPL);
}

bool OpenVmmWslVmBackend::AddGpu()
{
    // TODO: GPU passthrough configuration for OpenVMM.
    // For now, GPU is not supported with the OpenVMM backend.
    return false;
}

std::shared_ptr<GuestDeviceManager> OpenVmmWslVmBackend::GetGuestDeviceManager()
{
    // OpenVMM device management goes through ttrpc, not GuestDeviceManager (which is HCS-specific).
    return nullptr;
}

std::unique_ptr<wsl::core::INetworkingEngine> OpenVmmWslVmBackend::CreateNetworkingEngine(
    _In_ wsl::core::NetworkingMode /*Mode*/,
    _In_ wsl::core::GnsChannel&& /*GnsChannel*/,
    _Inout_ wsl::core::Config& /*Config*/,
    _In_ REFGUID /*RuntimeId*/,
    _In_ wil::unique_socket&& /*DnsTunnelingSocket*/,
    _In_ const std::shared_ptr<GuestDeviceManager>& /*DeviceManager*/,
    _In_ const wil::shared_handle& /*UserToken*/,
    _Inout_opt_ wsl::windows::common::hcs::unique_hcn_network* /*NatNetwork*/)
{
    // TODO: Create the appropriate networking engine for OpenVMM.
    // Likely VirtioProxy-based networking similar to WSLC's ConsommeNetworking.
    THROW_HR(E_NOTIMPL);
}

void OpenVmmWslVmBackend::LaunchOpenVmm()
{
    // TODO: Locate openvmm.exe, build command line, spawn process,
    // connect ttrpc client, and issue CreateVm + ResumeVm RPCs.
    // See OpenVmmVirtualMachine::LaunchOpenVmm() for reference implementation.
    THROW_HR(E_NOTIMPL);
}

void OpenVmmWslVmBackend::WatchProcessExit()
{
    HANDLE handles[] = {m_processHandle.get(), m_vmExitEvent.get()};
    DWORD result = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE);

    if (result == WAIT_OBJECT_0 && m_exitCallback)
    {
        // openvmm.exe process exited.
        m_exitCallback(L"");
    }
}
