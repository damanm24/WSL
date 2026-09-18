// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmVirtualMachineBackend.h"
#include <afunix.h>
#include <bitset>
#include "socket.hpp"
#include "SubProcess.h"
#include "wslopenvmm.h"

namespace validation = wsl::windows::common::vm::validation;

namespace {

constexpr UINT64 c_memoryGranularity = 2ULL * 1024 * 1024;
constexpr UINT64 c_maximumMemory = 4ULL * 1024 * 1024 * 1024;
constexpr UINT32 c_maximumDisks = 254;
constexpr UINT32 c_rpcTimeoutMs = 30000;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

void DestroyConfig(WslOpenVmmConfig* Config) noexcept
{
    WslOpenVmmDestroyConfig(&Config);
}

using UniqueConfig = wil::unique_any<WslOpenVmmConfig*, decltype(&DestroyConfig), DestroyConfig>;

wil::unique_hfile OpenBackingFile(const std::filesystem::path& Path, bool ReadOnly)
{
    wil::unique_hfile file{CreateFileW(
        Path.c_str(), GENERIC_READ | (ReadOnly ? 0 : GENERIC_WRITE), FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!file);
    file.reset();

    // Pin the file without conflicting with the VMM's disk sharing mode.
    file.reset(CreateFileW(Path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(!file);
    return file;
}

void DeleteOwnedFile(const std::filesystem::path& Path) noexcept
{
    if (!Path.empty() && !DeleteFileW(Path.c_str()))
    {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        {
            LOG_WIN32(error);
        }
    }
}

std::filesystem::path GetVsockListenerPath(const std::filesystem::path& VsockPath, GuestServicePort Port)
{
    return std::format(L"{}_{:08x}-facb-11e6-bd58-64006a7986d3", VsockPath.native(), Port.Value);
}

std::wstring GetConsommeCidr(const VmUserModeNatNetwork& Configuration)
{
    std::uint32_t prefixLength = 0;
    bool foundZero = false;
    VmIpv4Address network;
    for (size_t octet = 0; octet < Configuration.Netmask.Bytes.size(); ++octet)
    {
        for (std::uint8_t bit = 0x80; bit != 0; bit >>= 1)
        {
            if ((Configuration.Netmask.Bytes[octet] & bit) != 0)
            {
                THROW_HR_IF_MSG(E_INVALIDARG, foundZero, "OpenVMM requires a contiguous IPv4 netmask");
                ++prefixLength;
            }
            else
            {
                foundZero = true;
            }
        }

        network.Bytes[octet] = Configuration.ClientIpv4.Bytes[octet] & Configuration.Netmask.Bytes[octet];
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            network.Bytes[octet] != (Configuration.GatewayIpv4.Bytes[octet] & Configuration.Netmask.Bytes[octet]),
            "OpenVMM requires the client and gateway IPv4 addresses to be in the same subnet");
    }

    return std::format(L"{}.{}.{}.{}/{}", network.Bytes[0], network.Bytes[1], network.Bytes[2], network.Bytes[3], prefixLength);
}

std::wstring FormatIpAddress(const VmIpAddress& Address)
{
    if (const auto* ipv4 = std::get_if<VmIpv4Address>(&Address))
    {
        return std::format(L"{}.{}.{}.{}", ipv4->Bytes[0], ipv4->Bytes[1], ipv4->Bytes[2], ipv4->Bytes[3]);
    }

    const auto& ipv6 = std::get<VmIpv6Address>(Address);
    IN6_ADDR address{};
    std::copy(ipv6.Bytes.begin(), ipv6.Bytes.end(), address.u.Byte);
    std::wstring result(INET6_ADDRSTRLEN, L'\0');
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), InetNtopW(AF_INET6, &address, result.data(), result.size()) == nullptr);
    result.resize(std::wcslen(result.c_str()));
    if (ipv6.ScopeId != 0)
    {
        result += std::format(L"%{}", ipv6.ScopeId);
    }

    return result;
}

} // namespace

OpenVmmVirtualMachineBackend::State::GuestListener::~GuestListener() noexcept
{
    Socket.reset();
    DeleteOwnedFile(Path);
}

VmDescription wsl::windows::common::vm::openvmm::ValidateCreateRequest(const VmCreateRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, IsEqualGUID(Request.Identity.VmId, GUID_NULL));
    THROW_HR_IF(E_INVALIDARG, Request.Processor.Count == 0 || Request.Memory.SizeBytes == 0);
    THROW_HR_IF_MSG(c_notSupported, wsl::shared::Arm64, "OpenVMM direct boot is currently supported only on x64");
    validation::ValidatePath(Request.Boot.KernelPath, L"OpenVMM");
    validation::ValidatePath(Request.Boot.InitrdPath, L"OpenVMM");
    THROW_HR_IF(E_INVALIDARG, Request.Boot.KernelCommandLine.find(L'\0') != std::wstring::npos);
    THROW_HR_IF(
        E_INVALIDARG,
        Request.Boot.Method != VmBootMethod::Automatic && Request.Boot.Method != VmBootMethod::LinuxDirect &&
            Request.Boot.Method != VmBootMethod::Uefi);
    THROW_HR_IF(c_notSupported, Request.Boot.Method == VmBootMethod::Uefi);
    THROW_HR_IF(c_notSupported, Request.Boot.UefiRootPath.has_value());
    THROW_HR_IF(c_notSupported, Request.Memory.Mmio.has_value());

    VmDescription description;
    description.Identity = Request.Identity;
    description.Backend = BackendKind::OpenVmm;
    description.Processor.Count = Request.Processor.Count;
    description.Memory.SizeBytes = Request.Memory.SizeBytes;
    THROW_HR_IF(E_INVALIDARG, Request.Memory.SizeBytes % c_memoryGranularity != 0);

    THROW_HR_IF_MSG(
        c_notSupported,
        description.Memory.SizeBytes < c_memoryGranularity || Request.Memory.SizeBytes > c_maximumMemory,
        "OpenVMM currently supports memory sizes from 2 MiB to 4 GiB; memory is not silently capped");
    validation::ValidateFeature(Request.Processor.NestedVirtualization, L"nested virtualization");
    validation::ValidateFeature(Request.Processor.PerfmonPmu, L"PMU");
    validation::ValidateFeature(Request.Processor.PerfmonLbr, L"LBR");
    validation::ValidateFeature(Request.Memory.AllowOvercommit, L"memory overcommit");
    validation::ValidateFeature(Request.Memory.DeferredCommit, L"deferred memory commit");
    validation::ValidateFeature(Request.Memory.ColdDiscard, L"cold discard");

    if (Request.Memory.SmallPages)
    {
        validation::ValidateUnsupportedSelection(Request.Memory.SmallPages->Policy);
    }
    if (Request.CrashCapture)
    {
        validation::ValidateUnsupportedSelection(Request.CrashCapture->Policy);
    }

    description.Boot.Method = VmBootMethod::LinuxDirect;
    description.Boot.KernelCommandLine = Request.Boot.KernelCommandLine;

    bool serialConfigured = false;
    bool virtioConfigured = false;
    for (const auto& console : Request.Consoles)
    {
        if (const auto* serial = std::get_if<VmSerialConsole>(&console.Device))
        {
            THROW_HR_IF(c_notSupported, serial->Port != 0);
            THROW_HR_IF(E_INVALIDARG, serialConfigured);
            validation::ValidateConsolePath(serial->NamedPipe, L"OpenVMM", c_notSupported, false);
            serialConfigured = true;
        }
        else
        {
            const auto& virtio = std::get<VmVirtioConsole>(console.Device);
            THROW_HR_IF(c_notSupported, virtio.Port != 0 || !virtio.GuestName.empty());
            THROW_HR_IF(E_INVALIDARG, virtioConfigured);
            validation::ValidateConsolePath(virtio.NamedPipe, L"OpenVMM", c_notSupported, false);
            virtioConfigured = true;
        }
        description.Boot.Consoles.push_back(console);
    }

    THROW_HR_IF(c_notSupported, Request.BootDisks.size() > c_maximumDisks);
    std::bitset<c_maximumDisks> allocated;
    for (const auto& disk : Request.BootDisks)
    {
        THROW_HR_IF(E_INVALIDARG, disk.Key.empty() || description.BootDisks.contains(disk.Key));
        description.BootDisks.emplace(disk.Key, VmDiskAttachment{});
        validation::ValidateDiskRequest(disk.Disk, c_maximumDisks);
        if (disk.Disk.Placement)
        {
            const auto& placement = *disk.Disk.Placement;
            THROW_HR_IF(E_INVALIDARG, allocated.test(placement.Address.Lun));
            allocated.set(placement.Address.Lun);
        }
    }

    std::uint64_t nextId = 1;
    for (const auto& disk : Request.BootDisks)
    {
        std::uint32_t lun = 0;
        if (disk.Disk.Placement)
        {
            lun = disk.Disk.Placement->Address.Lun;
        }
        else
        {
            while (lun < c_maximumDisks && allocated.test(lun))
            {
                ++lun;
            }
            THROW_HR_IF(E_BOUNDS, lun == c_maximumDisks);
            allocated.set(lun);
        }
        description.BootDisks.at(disk.Key) = {{description.Identity, nextId++}, {0, lun}, disk.Disk.ReadOnly};
    }

    return description;
}

void OpenVmmVirtualMachineBackend::DestroyVm(WslOpenVmmVm* Vm) noexcept
{
    WslOpenVmmDestroyVm(&Vm);
}

OpenVmmVirtualMachineBackend::OpenVmmVirtualMachineBackend() : m_state(std::make_unique<State>())
{
}

OpenVmmVirtualMachineBackend::~OpenVmmVirtualMachineBackend() noexcept
{
    {
        auto lock = m_state->m_lock.lock_exclusive();
        CloseGuestListeners();
    }
    if (m_state->m_processWait)
    {
        SetThreadpoolWait(m_state->m_processWait.get(), nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(m_state->m_processWait.get(), TRUE);
        m_state->m_processWait.reset();
    }
    if (m_state->m_vm && WaitForSingleObject(m_state->m_process.get(), 0) == WAIT_TIMEOUT)
    {
        LOG_IF_FAILED(WslOpenVmmVmTeardown(m_state->m_vm.get()));
        LOG_IF_FAILED(WslOpenVmmVmQuit(m_state->m_vm.get()));
    }
    m_state->m_vm.reset();
    m_state->m_job.reset();
    if (m_state->m_process)
    {
        // Confirm exit before releasing backing files or deleting socket paths.
        LOG_LAST_ERROR_IF(WaitForSingleObject(m_state->m_process.get(), INFINITE) == WAIT_FAILED);
    }
    m_state->m_backingFiles.clear();
    if (m_state->m_directoryCreated)
    {
        DeleteOwnedFile(m_state->m_rpcSocketPath);
        DeleteOwnedFile(m_state->m_vsockPath);
        DeleteOwnedFile(m_state->m_socketDirectory / L"openvmm.log");
        LOG_IF_WIN32_BOOL_FALSE(RemoveDirectoryW(m_state->m_socketDirectory.c_str()));
    }
}

std::unique_ptr<OpenVmmVirtualMachineBackend> OpenVmmVirtualMachineBackend::Create(const VmCreateRequest& Request)
{
    auto description = wsl::windows::common::vm::openvmm::ValidateCreateRequest(Request);
    auto backend = std::unique_ptr<OpenVmmVirtualMachineBackend>{new OpenVmmVirtualMachineBackend{}};
    backend->m_state->m_description = std::move(description);
    backend->Initialize(Request);
    return backend;
}

void OpenVmmVirtualMachineBackend::Initialize(const VmCreateRequest& Request)
{
    using namespace wsl::windows::common;
    const auto executable = wslutil::GetBasePath() / L"openvmm.exe";
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        !filesystem::FileExists(executable.c_str()),
        "openvmm.exe not found at: %ls",
        executable.c_str());

    auto id = wsl::shared::string::GuidToString<wchar_t>(Request.Identity.VmId, wsl::shared::string::GuidToStringFlags::None);
    std::erase(id, L'-');
    // An exclusive directory creation prevents shortened path IDs from aliasing another VM.
    m_state->m_socketDirectory = filesystem::GetTempFolderPath(GetCurrentProcessToken()) / (L"ov-" + id.substr(0, 16));
    m_state->m_rpcSocketPath = m_state->m_socketDirectory / L"r";
    m_state->m_vsockPath = m_state->m_socketDirectory / L"v";
    const auto longestPath =
        wsl::shared::string::WideToMultiByte(m_state->m_vsockPath.native() + L"_ffffffff-facb-11e6-bd58-64006a7986d3");
    SOCKADDR_UN address{};
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        longestPath.size() >= sizeof(address.sun_path),
        "OpenVMM guest socket path exceeds the AF_UNIX limit: %hs",
        longestPath.c_str());
    THROW_HR_IF(E_INVALIDARG, m_state->m_rpcSocketPath.native().find_first_of(L",\"\r\n") != std::wstring::npos);

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken());
    const auto sid = wslutil::SidToString(tokenUser->User.Sid);
    const auto sddl = std::format(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;{})", sid.get());
    wil::unique_hlocal_security_descriptor security;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.get(), FALSE};
    {
        m_state->m_backingFiles.push_back(OpenBackingFile(Request.Boot.KernelPath, true));
        m_state->m_backingFiles.push_back(OpenBackingFile(Request.Boot.InitrdPath, true));
        auto lock = m_state->m_lock.lock_exclusive();
        for (const auto& disk : Request.BootDisks)
        {
            const auto& attachment = m_state->m_description.BootDisks.at(disk.Key);
            auto backingFile = OpenBackingFile(std::get<VmVirtualDiskSource>(disk.Disk.Source).Path, disk.Disk.ReadOnly);
            m_state->m_attachedDisks.emplace(attachment.Id.Value, State::AttachedDisk{attachment, std::move(backingFile)});
        }
        m_state->m_nextDiskId = Request.BootDisks.size() + 1;
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(m_state->m_socketDirectory.c_str(), &attributes));
        m_state->m_directoryCreated = true;
    }

    UniqueConfig config;
    THROW_IF_FAILED(WslOpenVmmCreateConfig(config.put()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelPath(config.get(), Request.Boot.KernelPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetInitrdPath(config.get(), Request.Boot.InitrdPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelCmdLine(config.get(), m_state->m_description.Boot.KernelCommandLine.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetMemoryMb(config.get(), m_state->m_description.Memory.SizeBytes / (1024 * 1024)));
    THROW_IF_FAILED(WslOpenVmmConfigSetProcessorCount(config.get(), Request.Processor.Count));
    THROW_IF_FAILED(WslOpenVmmConfigSetHvSocketPath(config.get(), m_state->m_vsockPath.c_str()));
    for (const auto& disk : Request.BootDisks)
    {
        const auto& attachment = m_state->m_description.BootDisks.at(disk.Key);
        THROW_IF_FAILED(WslOpenVmmConfigAddBootDisk(
            config.get(),
            attachment.GuestAddress.Controller,
            attachment.GuestAddress.Lun,
            std::get<VmVirtualDiskSource>(disk.Disk.Source).Path.c_str(),
            disk.Disk.ReadOnly));
    }
    for (const auto& console : Request.Consoles)
    {
        if (const auto* serial = std::get_if<VmSerialConsole>(&console.Device))
        {
            THROW_IF_FAILED(WslOpenVmmConfigAddSerialPort(config.get(), serial->Port, serial->NamedPipe.c_str()));
        }
        else
        {
            THROW_IF_FAILED(WslOpenVmmConfigSetVirtioConsolePath(config.get(), std::get<VmVirtioConsole>(console.Device).NamedPipe.c_str()));
        }
    }

    m_state->m_job = helpers::CreateKillOnCloseJob();
    const auto commandLine =
        std::format(L"\"{}\" --rpc \"path={},transport=grpc\"", executable.native(), m_state->m_rpcSocketPath.native());
    SubProcess process{executable.c_str(), commandLine.c_str()};
    process.SetFlags(CREATE_NO_WINDOW);
    process.SetJobObject(m_state->m_job.get());
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    wil::unique_hfile logFile;
    wil::unique_hfile input;
    {
        logFile.reset(CreateFileW(
            (m_state->m_socketDirectory / L"openvmm.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!logFile);
        input.reset(CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!input);
    }
    // OpenVMM closes stdout at startup; stderr must have a distinct handle value.
    wil::unique_hfile errorLogFile;
    THROW_IF_WIN32_BOOL_FALSE(
        DuplicateHandle(GetCurrentProcess(), logFile.get(), GetCurrentProcess(), errorLogFile.put(), 0, TRUE, DUPLICATE_SAME_ACCESS));
    process.SetStdHandles(input.get(), logFile.get(), errorLogFile.get());
    m_state->m_process = process.Start();
    m_state->m_processWait.reset(CreateThreadpoolWait(OnProcessExit, this, nullptr));
    THROW_LAST_ERROR_IF(!m_state->m_processWait);
    SetThreadpoolWait(m_state->m_processWait.get(), m_state->m_process.get(), nullptr);
    THROW_IF_FAILED_MSG(
        WslOpenVmmCreateVm(config.addressof(), m_state->m_rpcSocketPath.c_str(), c_rpcTimeoutMs, m_state->m_vm.put()),
        "Failed to create OpenVMM VM");
    const auto result = WaitForSingleObject(m_state->m_process.get(), 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED), result == WAIT_OBJECT_0);
}

void CALLBACK OpenVmmVirtualMachineBackend::OnProcessExit(PTP_CALLBACK_INSTANCE, void* Context, PTP_WAIT, TP_WAIT_RESULT) noexcept
{
    auto& backend = *static_cast<OpenVmmVirtualMachineBackend*>(Context);
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(backend.m_state->m_exitEvent.get()));
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(backend.m_state->m_operationCancellationEvent.get()));
    auto lock = backend.m_state->m_lock.lock_exclusive();
    backend.CloseGuestListeners();
}

VmPlatformCapabilities OpenVmmVirtualMachineBackend::QueryCapabilities()
{
    VmPlatformCapabilities capabilities;
    capabilities.Backend = BackendKind::OpenVmm;
    // Report known OpenVMM support independently of which backend methods are wired through the C ABI.
    for (const auto operation :
         {VmOperation::Create,
          VmOperation::Start,
          VmOperation::Terminate,
          VmOperation::CreateGuestListener,
          VmOperation::AcceptGuestConnection,
          VmOperation::ConnectGuest,
          VmOperation::CloseGuestListener,
          VmOperation::AttachDisk,
          VmOperation::DetachDisk,
          VmOperation::CreateFileSystemDevice,
          VmOperation::AddFileSystemShare,
          VmOperation::RemoveFileSystemShare,
          VmOperation::RemoveDevice,
          VmOperation::AddNetworkAdapter,
          VmOperation::UpdateNetworkAdapter,
          VmOperation::BindPort,
          VmOperation::UnbindPort})
    {
        capabilities.Operations.set(static_cast<size_t>(operation));
    }
    for (const auto feature :
         {VmFeature::LinuxDirectBoot,
          VmFeature::LinuxFirmwareBoot,
          VmFeature::MemoryOvercommit,
          VmFeature::SerialConsole,
          VmFeature::VirtioConsole,
          VmFeature::Vhd,
          VmFeature::Vhdx,
          VmFeature::VirtioFsFileBacked,
          VmFeature::SavedStateOnCrash,
          VmFeature::UserModeNatNetwork,
          VmFeature::TcpPortBinding,
          VmFeature::UdpPortBinding,
          VmFeature::Ipv6PortBinding,
          VmFeature::ScopedIpv6PortBinding})
    {
        capabilities.Features.set(static_cast<size_t>(feature));
    }
    return capabilities;
}

VmPlatformCapabilities OpenVmmVirtualMachineBackend::GetCapabilities() const
{
    return QueryCapabilities();
}

wil::unique_handle OpenVmmVirtualMachineBackend::GetTerminationEvent() const
{
    wil::unique_handle event;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), m_state->m_exitEvent.get(), GetCurrentProcess(), event.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));
    return event;
}

void OpenVmmVirtualMachineBackend::Start()
{
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    THROW_IF_FAILED(WslOpenVmmVmResume(m_state->m_vm.get()));
}

void OpenVmmVirtualMachineBackend::Terminate()
{
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    THROW_IF_FAILED(WslOpenVmmVmTeardown(m_state->m_vm.get()));
    const auto quitResult = WslOpenVmmVmQuit(m_state->m_vm.get());
    const auto waitResult = WaitForSingleObject(m_state->m_process.get(), c_rpcTimeoutMs);
    THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
    if (waitResult != WAIT_OBJECT_0)
    {
        THROW_IF_FAILED(quitResult);
        THROW_HR(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    }

    m_state->m_vm.reset();
    m_state->m_attachedDisks.clear();
    m_state->m_fileSystemShares.clear();
    m_state->m_fileSystemDevices.clear();
    m_state->m_portBindings.clear();
    m_state->m_networkAdapters.clear();
    CloseGuestListeners();
}

void OpenVmmVirtualMachineBackend::CancelPendingOperations() noexcept
{
    if (m_state->m_vm)
    {
        LOG_IF_FAILED(WslOpenVmmVmCancelRequests(m_state->m_vm.get()));
    }
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(m_state->m_operationCancellationEvent.get()));
    auto lock = m_state->m_lock.lock_exclusive();
    CloseGuestListeners();
}

std::shared_ptr<VmGuestListenerState> OpenVmmVirtualMachineBackend::ConfigureGuestListener(const VmGuestListener& Listener)
{
    auto listener = std::make_shared<State::GuestListener>();
    listener->Listener = Listener;
    listener->Path = GetVsockListenerPath(m_state->m_vsockPath, Listener.Port);
    DeleteOwnedFile(listener->Path);

    listener->Socket.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !listener->Socket);

    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    const auto narrowPath = wsl::shared::string::WideToMultiByte(listener->Path.native());
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(address.sun_path), "vsock bridge path too long: %hs", narrowPath.c_str());
    std::copy(narrowPath.cbegin(), narrowPath.cend(), address.sun_path);
    address.sun_path[narrowPath.size()] = '\0';

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        bind(listener->Socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), listen(listener->Socket.get(), SOMAXCONN) == SOCKET_ERROR);
    return listener;
}

VmGuestListener OpenVmmVirtualMachineBackend::CreateGuestListener(GuestServicePort Port)
{
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    return RegisterGuestListener(m_state->m_description.Identity, Port);
}

wil::unique_socket OpenVmmVirtualMachineBackend::AcceptGuestConnection(VmListenerId Listener)
{
    return AcceptGuestListenerConnection(Listener, m_state->m_description.Identity);
}

wil::unique_socket OpenVmmVirtualMachineBackend::ConnectGuest(GuestServicePort Port)
{
    {
        auto lock = m_state->m_lock.lock_shared();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    }

    wil::unique_socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);

    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    const auto narrowPath = wsl::shared::string::WideToMultiByte(m_state->m_vsockPath.native());
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(address.sun_path), "vsock bridge path too long: %hs", narrowPath.c_str());
    std::copy(narrowPath.cbegin(), narrowPath.cend(), address.sun_path);
    address.sun_path[narrowPath.size()] = '\0';

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()), connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

    const auto request = std::format("CONNECT {}\n", Port.Value);
    wsl::windows::common::socket::Send(
        socket.get(),
        gsl::make_span(reinterpret_cast<const gsl::byte*>(request.data()), request.size()),
        m_state->m_operationCancellationEvent.get());

    std::array<char, 64> response{};
    size_t responseLength = 0;
    for (; responseLength < response.size() - 1; ++responseLength)
    {
        const auto bytesRead = wsl::windows::common::socket::Receive(
            socket.get(),
            gsl::make_span(reinterpret_cast<gsl::byte*>(&response[responseLength]), 1),
            m_state->m_operationCancellationEvent.get(),
            MSG_WAITALL,
            c_rpcTimeoutMs);
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), bytesRead == 0, "vsock bridge closed during CONNECT handshake");
        if (response[responseLength] == '\n')
        {
            ++responseLength;
            break;
        }
    }

    THROW_HR_IF_MSG(
        E_FAIL, responseLength == response.size() - 1 && response[responseLength - 1] != '\n', "vsock bridge response too long");
    const std::string_view responseView{response.data(), responseLength};
    THROW_HR_IF_MSG(E_FAIL, !responseView.starts_with("OK "), "vsock bridge CONNECT failed: %hs", response.data());
    return socket;
}

void OpenVmmVirtualMachineBackend::CloseGuestListener(VmListenerId Listener)
{
    RemoveGuestListener(Listener, m_state->m_description.Identity);
}

VmDiskAttachment OpenVmmVirtualMachineBackend::AttachDisk(const VmDiskRequest& Request)
{
    const auto& source = validation::ValidateDiskRequest(Request, c_maximumDisks);
    auto backingFile = OpenBackingFile(source.Path, Request.ReadOnly);
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);

    const auto lunInUse = [&](std::uint32_t Lun) {
        for (const auto& entry : m_state->m_attachedDisks)
        {
            if (entry.second.Attachment.GuestAddress.Lun == Lun)
            {
                return true;
            }
        }
        return false;
    };

    std::uint32_t lun = 0;
    if (Request.Placement)
    {
        lun = Request.Placement->Address.Lun;
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), lunInUse(lun));
    }
    else
    {
        while (lun < c_maximumDisks && lunInUse(lun))
        {
            ++lun;
        }
        THROW_HR_IF(WSL_E_TOO_MANY_DISKS_ATTACHED, lun == c_maximumDisks);
    }

    THROW_HR_IF(E_BOUNDS, m_state->m_nextDiskId == UINT64_MAX);
    const VmDiskAttachment attachment{{m_state->m_description.Identity, m_state->m_nextDiskId}, {0, lun}, Request.ReadOnly};
    const auto [disk, inserted] =
        m_state->m_attachedDisks.emplace(attachment.Id.Value, State::AttachedDisk{attachment, std::move(backingFile)});
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([&] { m_state->m_attachedDisks.erase(disk); });
    THROW_IF_FAILED(WslOpenVmmVmAttachScsiDisk(
        m_state->m_vm.get(), attachment.GuestAddress.Controller, attachment.GuestAddress.Lun, source.Path.c_str(), Request.ReadOnly));
    ++m_state->m_nextDiskId;
    rollback.release();
    return attachment;
}

void OpenVmmVirtualMachineBackend::DetachDisk(VmDiskId Disk)
{
    THROW_HR_IF(E_INVALIDARG, Disk.Value == 0 || !IsEqualGUID(Disk.Owner.VmId, m_state->m_description.Identity.VmId));
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    const auto disk = m_state->m_attachedDisks.find(Disk.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), disk == m_state->m_attachedDisks.end());
    THROW_IF_FAILED(WslOpenVmmVmDetachScsiDisk(
        m_state->m_vm.get(), disk->second.Attachment.GuestAddress.Controller, disk->second.Attachment.GuestAddress.Lun));
    m_state->m_attachedDisks.erase(disk);
}

VmFileSystemDevice OpenVmmVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request)
{
    validation::ValidateName(Request.Transport.Tag, L"OpenVMM virtio-fs tag");
    THROW_HR_IF_MSG(
        c_notSupported,
        Request.Transport.Layout != VmVirtioFsLayout::SingleShare,
        "OpenVMM currently supports only single-share virtio-fs devices");

    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    THROW_HR_IF(E_BOUNDS, m_state->m_nextDeviceId == UINT64_MAX);
    for (const auto& entry : m_state->m_fileSystemDevices)
    {
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), wsl::shared::string::IsEqual(entry.second.Transport.Tag, Request.Transport.Tag, false));
    }

    VmFileSystemDevice device{{m_state->m_description.Identity, m_state->m_nextDeviceId}, VmFileSystemDeviceState::Prepared};
    const auto inserted =
        m_state->m_fileSystemDevices.emplace(device.Id.Value, State::FileSystemDevice{device, Request.Transport, {}}).second;
    WI_ASSERT(inserted);
    ++m_state->m_nextDeviceId;
    return device;
}

VmFileSystemShare OpenVmmVirtualMachineBackend::AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request)
{
    validation::ValidateResourceId(Device, m_state->m_description.Identity);
    validation::ValidatePath(Request.HostPath, L"OpenVMM");
    THROW_HR_IF_MSG(
        E_INVALIDARG, !Request.Name.empty(), "A single-share OpenVMM virtio-fs device does not accept a child share name");
    THROW_HR_IF_MSG(c_notSupported, !Request.Options.MountOptions.empty(), "OpenVMM does not support virtio-fs mount options");
    const auto hostPath = wsl::windows::common::filesystem::GetCanonicalPath(Request.HostPath);
    const auto attributes = GetFileAttributesW(hostPath.c_str());
    THROW_LAST_ERROR_IF(attributes == INVALID_FILE_ATTRIBUTES);
    THROW_HR_IF_MSG(
        E_INVALIDARG, WI_IsFlagClear(attributes, FILE_ATTRIBUTE_DIRECTORY), "The virtio-fs host path must be a directory");

    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    const auto device = m_state->m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_state->m_fileSystemDevices.end());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), device->second.Share.has_value());
    THROW_HR_IF(E_BOUNDS, m_state->m_nextShareId == UINT64_MAX);

    VmFileSystemShare share{
        {m_state->m_description.Identity, m_state->m_nextShareId}, Device, {device->second.Transport.Tag, {}}, hostPath, Request.ReadOnly};
    const auto [entry, inserted] = m_state->m_fileSystemShares.emplace(share.Id.Value, State::FileSystemShare{share});
    WI_ASSERT(inserted);
    device->second.Share = share.Id.Value;
    device->second.Device.State = VmFileSystemDeviceState::Serving;
    auto rollback = wil::scope_exit([&] {
        device->second.Share.reset();
        device->second.Device.State = VmFileSystemDeviceState::Prepared;
        m_state->m_fileSystemShares.erase(entry);
    });
    THROW_IF_FAILED(WslOpenVmmVmAddShare(m_state->m_vm.get(), share.GuestAddress.Tag.c_str(), hostPath.c_str(), Request.ReadOnly));
    ++m_state->m_nextShareId;
    rollback.release();
    return share;
}

void OpenVmmVirtualMachineBackend::RemoveFileSystemShare(VmShareId Share)
{
    validation::ValidateResourceId(Share, m_state->m_description.Identity);
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    const auto share = m_state->m_fileSystemShares.find(Share.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), share == m_state->m_fileSystemShares.end());
    const auto device = m_state->m_fileSystemDevices.find(share->second.Share.Device.Value);
    THROW_HR_IF(E_UNEXPECTED, device == m_state->m_fileSystemDevices.end() || device->second.Share != Share.Value);

    THROW_IF_FAILED(WslOpenVmmVmRemoveShare(m_state->m_vm.get(), share->second.Share.GuestAddress.Tag.c_str()));
    device->second.Share.reset();
    device->second.Device.State = VmFileSystemDeviceState::Prepared;
    m_state->m_fileSystemShares.erase(share);
}

VmNetworkAttachment OpenVmmVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest& Request)
{
    validation::ValidateName(Request.Tag, L"OpenVMM network adapter tag");
    const auto cidr = GetConsommeCidr(Request.Configuration);
    const auto macAddress = wsl::shared::string::FormatMacAddress(Request.Configuration.ClientMac.Bytes, L'-');
    GUID nicId{};
    THROW_IF_FAILED(CoCreateGuid(&nicId));
    const auto nicIdString = wsl::shared::string::GuidToString<wchar_t>(nicId, wsl::shared::string::GuidToStringFlags::None);

    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    THROW_HR_IF(E_BOUNDS, m_state->m_nextDeviceId == UINT64_MAX);
    for (const auto& entry : m_state->m_networkAdapters)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), wsl::shared::string::IsEqual(entry.second.Attachment.Tag, Request.Tag, false));
    }

    VmNetworkAttachment attachment{{m_state->m_description.Identity, m_state->m_nextDeviceId}, Request.Tag, nicId, Request.Configuration};
    const auto [entry, inserted] = m_state->m_networkAdapters.emplace(attachment.Id.Value, State::NetworkAdapter{attachment, nicIdString});
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([&] { m_state->m_networkAdapters.erase(entry); });
    THROW_IF_FAILED(WslOpenVmmVmAddConsommeNic(m_state->m_vm.get(), nicIdString.c_str(), macAddress.c_str(), cidr.c_str()));
    ++m_state->m_nextDeviceId;
    rollback.release();
    return attachment;
}

VmPortBinding OpenVmmVirtualMachineBackend::BindPort(VmDeviceId Device, const VmPortBindingRequest& Request)
{
    validation::ValidateResourceId(Device, m_state->m_description.Identity);
    THROW_HR_IF_MSG(
        c_notSupported, Request.Listen.Port == 0, "OpenVMM cannot report the allocated port for a dynamic host port binding");
    THROW_HR_IF(E_INVALIDARG, Request.GuestPort == 0);
    const auto hostAddress = FormatIpAddress(Request.Listen.Address);
    bool tcp = false;
    switch (Request.Protocol)
    {
    case VmTransportProtocol::Tcp:
        tcp = true;
        break;
    case VmTransportProtocol::Udp:
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }

    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    const auto adapter = m_state->m_networkAdapters.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), adapter == m_state->m_networkAdapters.end());
    THROW_HR_IF(E_BOUNDS, m_state->m_nextPortBindingId == UINT64_MAX);
    for (const auto& entry : m_state->m_portBindings)
    {
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            entry.second.Binding.Protocol == Request.Protocol && entry.second.Binding.EffectiveListen.Port == Request.Listen.Port &&
                wsl::shared::string::IsEqual(entry.second.HostAddress, hostAddress, false));
    }

    VmPortBinding binding{
        {m_state->m_description.Identity, m_state->m_nextPortBindingId}, Device, Request.Protocol, Request.Listen, Request.GuestPort};
    const auto [entry, inserted] =
        m_state->m_portBindings.emplace(binding.Id.Value, State::PortBinding{binding, adapter->second.NicId, hostAddress});
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([&] { m_state->m_portBindings.erase(entry); });
    THROW_IF_FAILED(WslOpenVmmVmBindPort(
        m_state->m_vm.get(), adapter->second.NicId.c_str(), Request.Listen.Port, Request.GuestPort, tcp, hostAddress.c_str()));
    ++m_state->m_nextPortBindingId;
    rollback.release();
    return binding;
}

void OpenVmmVirtualMachineBackend::UnbindPort(VmPortBindingId Binding)
{
    validation::ValidateResourceId(Binding, m_state->m_description.Identity);
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_vm);
    const auto binding = m_state->m_portBindings.find(Binding.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), binding == m_state->m_portBindings.end());

    THROW_IF_FAILED(WslOpenVmmVmUnbindPort(
        m_state->m_vm.get(),
        binding->second.NicId.c_str(),
        binding->second.Binding.EffectiveListen.Port,
        binding->second.Binding.GuestPort,
        binding->second.Binding.Protocol == VmTransportProtocol::Tcp,
        binding->second.HostAddress.c_str()));
    m_state->m_portBindings.erase(binding);
}
