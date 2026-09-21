// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HcsVirtualMachineBackend.h"
#include "GuestDeviceManager.h"
#include "disk.hpp"
#include "hvsocket.hpp"
#include "retryshared.h"
#include "wslsecurity.h"
#include <set>

namespace validation = wsl::windows::common::vm::validation;

namespace {

namespace schema = wsl::windows::common::hcs;
namespace helpers = wsl::windows::common::helpers;

constexpr UINT64 c_mib = 1024 * 1024;
constexpr UINT64 c_memoryGranularity = 2 * c_mib;
constexpr UINT32 c_maximumDisks = 254;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

bool ResolveFeature(VmFeatureRequest Request, bool Supported)
{
    switch (Request)
    {
    case VmFeatureRequest::Disabled:
        return false;
    case VmFeatureRequest::Preferred:
        return Supported;
    case VmFeatureRequest::Required:
        THROW_HR_IF(c_notSupported, !Supported);
        return true;
    }

    THROW_HR(E_INVALIDARG);
}

bool ResolveSelection(VmSelectionPolicy Policy, bool Supported)
{
    THROW_HR_IF(E_INVALIDARG, Policy != VmSelectionPolicy::Required && Policy != VmSelectionPolicy::Preferred);
    THROW_HR_IF(c_notSupported, Policy == VmSelectionPolicy::Required && !Supported);
    return Supported;
}

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
    description.Boot.Consoles = Request.Consoles;
    description.Boot.Method = Request.Boot.Method;
    if (description.Boot.Method == VmBootMethod::Automatic)
    {
        description.Boot.Method = wsl::shared::Arm64 ? VmBootMethod::Uefi : VmBootMethod::LinuxDirect;
    }

    auto& settings = configuration.Settings;
    settings.Owner = Request.Owner;
    settings.ShouldTerminateOnLastHandleClosed = true;
    const bool windows11 = helpers::IsWindows11OrAbove();
    settings.SchemaVersion = {2, 3};
    auto& vm = settings.VirtualMachine;
    vm.StopOnReset = true;
    vm.Chipset.UseUtc = true;
    if (!Request.EnablePlan9)
    {
        vm.Devices.Plan9.reset();
    }
    if (!Request.EnableBattery)
    {
        vm.Devices.Battery.reset();
    }
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

    bool nestedVirtualization = false;
    if (Request.Processor.NestedVirtualization != VmFeatureRequest::Disabled)
    {
        nestedVirtualization = schema::IsNestedVirtualizationSupported();
    }
    description.Processor.NestedVirtualization = ResolveFeature(Request.Processor.NestedVirtualization, nestedVirtualization);

    const auto perfmon = schema::GetPerfmonCapabilities();
    description.Processor.PerfmonPmu = ResolveFeature(Request.Processor.PerfmonPmu, perfmon.Pmu);
    description.Processor.PerfmonLbr = ResolveFeature(Request.Processor.PerfmonLbr, perfmon.Lbr);
    auto& processor = vm.ComputeTopology.Processor;
    processor.Count = description.Processor.Count;
    if (Request.Processor.NestedVirtualization != VmFeatureRequest::Disabled)
    {
        processor.ExposeVirtualizationExtensions = description.Processor.NestedVirtualization;
    }
    if (Request.Processor.PerfmonPmu != VmFeatureRequest::Disabled)
    {
        processor.EnablePerfmonPmu = description.Processor.PerfmonPmu;
    }
    if (Request.Processor.PerfmonLbr != VmFeatureRequest::Disabled)
    {
        processor.EnablePerfmonLbr = description.Processor.PerfmonLbr;
    }

    description.Memory.AllowOvercommit = ResolveFeature(Request.Memory.AllowOvercommit, true);
    description.Memory.DeferredCommit = ResolveFeature(Request.Memory.DeferredCommit, true);
    description.Memory.ColdDiscard = ResolveFeature(Request.Memory.ColdDiscard, true);
    THROW_HR_IF(E_INVALIDARG, description.Memory.DeferredCommit && !description.Memory.AllowOvercommit);
    auto& memory = vm.ComputeTopology.Memory;
    memory.SizeInMB = description.Memory.SizeBytes / c_mib;
    memory.AllowOvercommit = description.Memory.AllowOvercommit;
    memory.EnableDeferredCommit = description.Memory.DeferredCommit;
    memory.EnableColdDiscardHint = description.Memory.ColdDiscard;

    if (Request.Memory.SmallPages)
    {
        const auto& smallPages = *Request.Memory.SmallPages;
        if (ResolveSelection(smallPages.Policy, helpers::IsSmallPageMemorySupported(helpers::GetWindowsVersion())))
        {
            schema::ConfigureSmallPageMemory(memory, smallPages.FaultClusterSizeShift, smallPages.DirectMapFaultClusterSizeShift);
            description.Memory.SmallPages = true;
            description.Memory.FaultClusterSizeShift = memory.FaultClusterSizeShift;
            description.Memory.DirectMapFaultClusterSizeShift = memory.DirectMapFaultClusterSizeShift;
        }
    }

    if (Request.Memory.Mmio)
    {
        const auto& mmio = *Request.Memory.Mmio;
        THROW_HR_IF(E_INVALIDARG, mmio.HighWindowSizeBytes == 0 || mmio.HighWindowSizeBytes % c_mib != 0);
        memory.HighMmioGapInMB = mmio.HighWindowSizeBytes / c_mib;
        description.Memory.HighMmioSizeBytes = mmio.HighWindowSizeBytes;
        if (mmio.MaximumGuestAddressBits)
        {
            const auto bits = *mmio.MaximumGuestAddressBits;
            THROW_HR_IF(E_INVALIDARG, bits <= 32 || bits >= 64);
            const UINT64 addressLimit = UINT64{1} << bits;
            THROW_HR_IF(E_INVALIDARG, mmio.HighWindowSizeBytes > addressLimit - (UINT64{1} << 32));
            const auto base = addressLimit - mmio.HighWindowSizeBytes;
            memory.HighMmioBaseInMB = base / c_mib;
            description.Memory.HighMmioBaseBytes = base;
        }
    }

    if (Request.HostingProcessNameSuffix)
    {
        const auto& suffix = *Request.HostingProcessNameSuffix;
        THROW_HR_IF(E_INVALIDARG, suffix.Value.empty() || suffix.Value.find(L'\0') != std::wstring::npos);
        if (ResolveSelection(suffix.Policy, helpers::IsVmemmSuffixSupported()))
        {
            memory.HostingProcessNameSuffix = suffix.Value;
        }
    }

    std::set<std::wstring> consoleNames;
    for (const auto& console : Request.Consoles)
    {
        if (const auto* serial = std::get_if<VmSerialConsole>(&console.Device))
        {
            validation::ValidateConsolePath(serial->NamedPipe, L"HCS", E_INVALIDARG, true);
            THROW_HR_IF(E_INVALIDARG, serial->Port > 1);
            THROW_HR_IF(
                E_INVALIDARG,
                !vm.Devices.ComPorts.emplace(std::to_string(serial->Port), schema::ComPort{serial->NamedPipe.native()}).second);
        }
        else
        {
            const auto& virtio = std::get<VmVirtioConsole>(console.Device);
            THROW_HR_IF(c_notSupported, !helpers::IsVirtioSerialConsoleSupported());
            validation::ValidateConsolePath(virtio.NamedPipe, L"HCS", E_INVALIDARG, true);
            THROW_HR_IF(E_INVALIDARG, virtio.GuestName.find(L'\0') != std::wstring::npos);
            THROW_HR_IF(E_INVALIDARG, !consoleNames.emplace(virtio.GuestName).second);
            if (!vm.Devices.VirtioSerial)
            {
                vm.Devices.VirtioSerial.emplace();
            }
            THROW_HR_IF(
                E_INVALIDARG,
                !vm.Devices.VirtioSerial->Ports
                     .emplace(
                         std::to_string(virtio.Port), schema::VirtioSerialPort{virtio.GuestName, virtio.NamedPipe.native(), virtio.ConsoleSupport})
                     .second);
        }
    }

    THROW_HR_IF(c_notSupported, Request.BootDisks.size() > c_maximumDisks);
    std::bitset<c_maximumDisks> allocated;
    // Reserve explicit addresses before assigning any automatic placements.
    for (const auto& disk : Request.BootDisks)
    {
        THROW_HR_IF(E_INVALIDARG, disk.Key.empty() || disk.Key.find(L'\0') != std::wstring::npos);
        THROW_HR_IF(E_INVALIDARG, !description.BootDisks.emplace(disk.Key, VmDiskAttachment{}).second);
        if (disk.Disk.Placement)
        {
            const auto& address = disk.Disk.Placement->Address;
            THROW_HR_IF(c_notSupported, address.Controller != 0 || address.Lun >= c_maximumDisks);
            THROW_HR_IF(E_INVALIDARG, allocated.test(address.Lun));
            allocated.set(address.Lun);
        }
    }

    auto& scsi = vm.Devices.Scsi["0"];
    UINT64 nextId = 1;
    for (const auto& disk : Request.BootDisks)
    {
        UINT32 lun = 0;
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

        schema::Attachment attachment{};
        attachment.ReadOnly = disk.Disk.ReadOnly;
        if (const auto* source = std::get_if<VmVirtualDiskSource>(&disk.Disk.Source))
        {
            validation::ValidatePath(source->Path, L"HCS");
            THROW_HR_IF(E_INVALIDARG, source->Format != VmDiskFormat::Vhd && source->Format != VmDiskFormat::Vhdx);
            attachment = schema::CreateVhdAttachment(source->Path.c_str(), disk.Disk.ReadOnly);
        }
        else
        {
            const auto& physical = std::get<VmPhysicalDiskSource>(disk.Disk.Source);
            validation::ValidatePath(physical.DevicePath, L"HCS");
            attachment.Type = schema::AttachmentType::PassThru;
            attachment.Path = physical.DevicePath;
        }
        scsi.Attachments.emplace(std::to_string(lun), std::move(attachment));
        description.BootDisks.at(disk.Key) = {{description.Identity, nextId++}, {0, lun}, disk.Disk.ReadOnly};
    }

    if (Request.CrashCapture)
    {
        validation::ValidatePath(Request.CrashCapture->SavedStatePath, L"HCS");
        if (ResolveSelection(Request.CrashCapture->Policy, windows11))
        {
            vm.DebugOptions.BugcheckSavedStateFileName = Request.CrashCapture->SavedStatePath.native();
        }
    }

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(Request.Identity.UserToken.get());
    vm.Devices.HvSocket = schema::CreateHvSocketConfiguration(tokenUser->User.Sid);
    return configuration;
}

HcsVirtualMachineBackend::HcsVirtualMachineBackend() : m_state(std::make_unique<State>())
{
}

HcsVirtualMachineBackend::~HcsVirtualMachineBackend() noexcept = default;

HcsVirtualMachineBackend::State::~State() noexcept
{
    CloseFileSystemDevices();
}

void HcsVirtualMachineBackend::State::CloseFileSystemDevices() noexcept
{
    // Device hosts must be shut down while the compute system and callback context still exist.
    for (const auto& [id, device] : m_fileSystemDevices)
    {
        if (const auto* plan9 = std::get_if<Plan9Device>(&device.Resource))
        {
            LOG_IF_FAILED(plan9->Server->Teardown());
        }
    }
    m_guestDeviceManager.reset();
    m_fileSystemDevices.clear();
}

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
    m_state->m_machineId =
        wsl::shared::string::GuidToString<wchar_t>(Request.Identity.VmId, wsl::shared::string::GuidToStringFlags::Uppercase);
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
    m_state->CloseFileSystemDevices();
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
    const auto* virtualDisk = std::get_if<VmVirtualDiskSource>(&Request.Source);
    const bool isPhysical = virtualDisk == nullptr;
    const auto& path = isPhysical ? std::get<VmPhysicalDiskSource>(Request.Source).DevicePath : virtualDisk->Path.native();
    if (virtualDisk != nullptr)
    {
        validation::ValidatePath(virtualDisk->Path, L"HCS");
        THROW_HR_IF(E_INVALIDARG, virtualDisk->Format != VmDiskFormat::Vhd && virtualDisk->Format != VmDiskFormat::Vhdx);
    }
    else
    {
        validation::ValidatePath(std::filesystem::path{std::get<VmPhysicalDiskSource>(Request.Source).DevicePath}, L"HCS");
    }

    if (Request.Placement)
    {
        THROW_HR_IF(
            c_notSupported, Request.Placement->Address.Controller != 0 || Request.Placement->Address.Lun >= c_maximumDisks);
    }
    THROW_HR_IF(E_INVALIDARG, Request.OperationTimeout <= std::chrono::milliseconds::zero());

    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_system);
    for (auto disk = m_state->m_attachedDisks.begin(); disk != m_state->m_attachedDisks.end(); ++disk)
    {
        if (disk->second.IsPhysical != isPhysical || disk->second.Path != path)
        {
            continue;
        }

        if (isPhysical)
        {
            THROW_HR_WITH_USER_ERROR(WSL_E_DISK_ALREADY_ATTACHED, wsl::shared::Localization::MessageDiskAlreadyAttached(path.c_str()));
        }

        THROW_HR_IF(WSL_E_USER_VHD_ALREADY_ATTACHED, disk->second.IsUserDisk);
        if (wsl::windows::common::disk::IsBackingVolumeMounted(disk->second.BackingFile.get()))
        {
            return disk->second.Attachment;
        }

        schema::RemoveScsiDisk(m_state->m_system.get(), disk->second.Attachment.GuestAddress.Lun);
        if (disk->second.AccessGranted)
        {
            schema::RevokeVmAccess(m_state->m_machineId.c_str(), disk->second.Path.c_str());
        }
        m_state->m_attachedDisks.erase(disk);
        break;
    }

    const auto lunInUse = [&](std::uint32_t Lun) {
        for (const auto& entry : m_state->m_description.BootDisks)
        {
            if (entry.second.GuestAddress.Lun == Lun)
            {
                return true;
            }
        }

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
    State::AttachedDisk disk{attachment, path, isPhysical, Request.IsUserDisk, false, false, {}, Request.OperationTimeout};
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (disk.AccessGranted)
        {
            schema::RevokeVmAccess(m_state->m_machineId.c_str(), disk.Path.c_str());
        }
        if (disk.WasOnline)
        {
            const auto diskHandle = wsl::windows::common::disk::OpenDevice(
                disk.Path.c_str(), GENERIC_READ | GENERIC_WRITE, Request.OperationTimeout.count());
            wsl::windows::common::disk::SetOnline(diskHandle.get(), true, Request.OperationTimeout.count());
        }
    });

    try
    {
        if (isPhysical)
        {
            THROW_HR_IF(
                WSL_E_ELEVATION_NEEDED_TO_MOUNT_DISK, Request.UserToken && !wsl::windows::common::security::IsTokenElevated(Request.UserToken.get()));
            schema::GrantVmAccess(m_state->m_machineId.c_str(), path.c_str());
            disk.AccessGranted = true;
            {
                const auto diskHandle = wsl::windows::common::disk::OpenDevice(
                    path.c_str(), GENERIC_READ | GENERIC_WRITE, Request.OperationTimeout.count());
                if (wsl::windows::common::disk::IsDiskOnline(diskHandle.get()))
                {
                    wsl::windows::common::disk::SetOnline(diskHandle.get(), false, Request.OperationTimeout.count());
                    disk.WasOnline = true;
                }
            }
            wsl::shared::retry::RetryWithTimeout<void>(
                [&] { schema::AddPassThroughDisk(m_state->m_system.get(), path.c_str(), lun, Request.ReadOnly); },
                wsl::windows::common::disk::c_diskOperationRetry,
                Request.OperationTimeout,
                [] { return wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION); });
        }
        else
        {
            disk.BackingFile = wsl::windows::common::disk::OpenVhdBackingFile(path.c_str());
            const auto token = Request.UserToken ? Request.UserToken.get() : m_state->m_description.Identity.UserToken.get();
            const auto grantAccess = [&] {
                auto runAsUser = wil::impersonate_token(token);
                schema::GrantVmAccess(m_state->m_machineId.c_str(), path.c_str());
                disk.AccessGranted = true;
            };
            if (!Request.ReadOnly)
            {
                grantAccess();
            }
            auto result = wil::ResultFromException([&] { schema::AddVhd(m_state->m_system.get(), path.c_str(), lun, Request.ReadOnly); });
            if (result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && !disk.AccessGranted)
            {
                grantAccess();
                schema::AddVhd(m_state->m_system.get(), path.c_str(), lun, Request.ReadOnly);
            }
            else
            {
                THROW_IF_FAILED(result);
            }
        }
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();
        THROW_HR_WITH_USER_ERROR(
            result, wsl::shared::Localization::MessageFailedToAttachDisk(path.c_str(), wsl::windows::common::wslutil::GetSystemErrorString(result)));
    }

    const auto inserted = m_state->m_attachedDisks.emplace(attachment.Id.Value, std::move(disk)).second;
    WI_ASSERT(inserted);
    ++m_state->m_nextDiskId;
    cleanup.release();
    return attachment;
}

void HcsVirtualMachineBackend::DetachDisk(VmDiskId Disk)
{
    validation::ValidateResourceId(Disk, m_state->m_description.Identity);
    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_system);
    const auto disk = m_state->m_attachedDisks.find(Disk.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), disk == m_state->m_attachedDisks.end());
    {
        auto runAsSelf = wil::run_as_self();
        schema::RemoveScsiDisk(m_state->m_system.get(), disk->second.Attachment.GuestAddress.Lun);
        if (disk->second.AccessGranted)
        {
            schema::RevokeVmAccess(m_state->m_machineId.c_str(), disk->second.Path.c_str());
        }
    }
    if (disk->second.WasOnline)
    {
        wsl::windows::common::disk::RestorePassthroughDiskState(disk->second.Path.c_str(), disk->second.OperationTimeout.count());
    }
    m_state->m_attachedDisks.erase(disk);
}

VmFileSystemDevice HcsVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request)
{
    const auto* virtioFs = std::get_if<VmVirtioFsDevice>(&Request.Transport);
    const auto* plan9Socket = std::get_if<VmPlan9SocketDevice>(&Request.Transport);
    const auto* plan9Virtio = std::get_if<VmPlan9VirtioDevice>(&Request.Transport);
    if (std::holds_alternative<VmVirtioFsDevice>(Request.Transport))
    {
        validation::ValidateName(virtioFs->Tag, L"HCS virtio-fs tag");
        THROW_HR_IF(E_INVALIDARG, virtioFs->Layout != VmVirtioFsLayout::SingleShare && virtioFs->Layout != VmVirtioFsLayout::Aggregate);
    }
    else if (std::holds_alternative<VmPlan9SocketDevice>(Request.Transport))
    {
        THROW_HR_IF(E_INVALIDARG, plan9Socket->Port.Value == 0);
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, plan9Virtio == nullptr);
        validation::ValidateName(plan9Virtio->Tag, L"HCS Plan9 virtio tag");
    }

    auto lock = m_state->m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_state->m_system || m_state->m_terminatingEvent.is_signaled());
    THROW_HR_IF(E_BOUNDS, m_state->m_nextDeviceId == UINT64_MAX);
    for (const auto& [id, existing] : m_state->m_fileSystemDevices)
    {
        bool duplicate = false;
        if (virtioFs != nullptr)
        {
            const auto* transport = std::get_if<VmVirtioFsDevice>(&existing.Request.Transport);
            duplicate = transport != nullptr && transport->Tag == virtioFs->Tag;
        }
        else if (plan9Socket != nullptr)
        {
            const auto* transport = std::get_if<VmPlan9SocketDevice>(&existing.Request.Transport);
            duplicate = transport != nullptr && transport->Port.Value == plan9Socket->Port.Value;
        }
        else
        {
            const auto* transport = std::get_if<VmPlan9VirtioDevice>(&existing.Request.Transport);
            duplicate = transport != nullptr && transport->Tag == plan9Virtio->Tag;
        }
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), duplicate);
    }

    VmFileSystemDevice device{{m_state->m_description.Identity, m_state->m_nextDeviceId}, VmFileSystemDeviceState::Prepared};
    const auto [entry, inserted] = m_state->m_fileSystemDevices.emplace(device.Id.Value, State::FileSystemDevice{device, Request, {}});
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([&] { m_state->m_fileSystemDevices.erase(entry); });

    // Single-share virtio-fs needs the host path and options supplied by AddFileSystemShare.
    if (virtioFs == nullptr || virtioFs->Layout == VmVirtioFsLayout::Aggregate)
    {
        const auto runtimeId = schema::GetRuntimeId(m_state->m_system.get());
        const bool hadDeviceManager = m_state->m_guestDeviceManager != nullptr;
        auto rollbackManager = wil::scope_exit([&] {
            if (!hadDeviceManager)
            {
                m_state->m_guestDeviceManager.reset();
            }
        });
        if (plan9Socket == nullptr && !m_state->m_guestDeviceManager)
        {
            m_state->m_guestDeviceManager = std::make_unique<GuestDeviceManager>(m_state->m_machineId, runtimeId);
        }

        const auto userToken = m_state->m_description.Identity.UserToken.get();
        if (virtioFs != nullptr)
        {
            entry->second.Resource = m_state->m_guestDeviceManager->AddVirtiofsDevice(
                virtioFs->Tag.c_str(), L"", L"", userToken, {.Kind = VirtiofsShareKind_Aggregate});
        }
        else
        {
            wil::com_ptr<IPlan9FileSystem> server;
            auto teardownOnFailure = wil::scope_exit([&] {
                if (server)
                {
                    LOG_IF_FAILED(server->Teardown());
                }
            });
            bool registered = false;
            auto unregisterOnFailure = wil::scope_exit([&] {
                if (registered)
                {
                    m_state->m_guestDeviceManager->RemoveRemoteFileSystem(__uuidof(p9fs::Plan9FileSystem), plan9Virtio->Tag);
                }
            });
            {
                auto revert = wil::impersonate_token(userToken);
                server = wsl::windows::common::wslutil::CreateComServerAsUser<p9fs::Plan9FileSystem, IPlan9FileSystem>(userToken);
                if (plan9Socket != nullptr)
                {
                    THROW_IF_FAILED(server->Init(&runtimeId, plan9Socket->Port.Value));
                    THROW_IF_FAILED(server->Resume());
                    entry->second.Resource = State::Plan9Device{server, {}};
                }
                else
                {
                    m_state->m_guestDeviceManager->AddRemoteFileSystem(__uuidof(p9fs::Plan9FileSystem), plan9Virtio->Tag.c_str(), server);
                    registered = true;
                }
            }
            if (plan9Virtio != nullptr)
            {
                // The initial device needs host privileges; subsequent mounts use NotifyAllDevicesInUse.
                const auto instanceId =
                    m_state->m_guestDeviceManager->AddNewDevice(VIRTIO_PLAN9_DEVICE_ID, server, plan9Virtio->Tag.c_str());
                entry->second.Resource = State::Plan9Device{server, instanceId};
            }
            unregisterOnFailure.release();
            teardownOnFailure.release();
        }
        rollbackManager.release();
        device.State = VmFileSystemDeviceState::Serving;
        entry->second.Device.State = device.State;
    }

    ++m_state->m_nextDeviceId;
    rollback.release();
    return device;
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
