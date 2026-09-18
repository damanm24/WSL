// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HcsVirtualMachineBackend.h"
#include <set>

namespace {

namespace schema = wsl::windows::common::hcs;
namespace helpers = wsl::windows::common::helpers;

constexpr UINT64 c_mib = 1024 * 1024;
constexpr UINT64 c_memoryGranularity = 2 * c_mib;
constexpr UINT32 c_maximumDisks = 254;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

void ValidatePath(const std::filesystem::path& Path)
{
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        Path.empty() || !Path.is_absolute() || Path.native().find(L'\0') != std::wstring::npos,
        "HCS requires an absolute, nonempty host path");
}

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

void ValidateConsolePath(const std::filesystem::path& Path)
{
    ValidatePath(Path);
    THROW_HR_IF(E_INVALIDARG, !Path.native().starts_with(L"\\\\.\\pipe\\") || Path.filename().empty());
}

} // namespace

wsl::windows::common::vm::hcs::VmConfiguration wsl::windows::common::vm::hcs::BuildConfiguration(const VmCreateRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, IsEqualGUID(Request.Identity.VmId, GUID_NULL) || !Request.Identity.UserToken);
    THROW_HR_IF(E_INVALIDARG, Request.Owner.empty() || Request.Owner.find(L'\0') != std::wstring::npos);
    THROW_HR_IF(E_INVALIDARG, Request.Processor.Count == 0 || Request.Memory.SizeBytes == 0);
    THROW_HR_IF(E_INVALIDARG, Request.Memory.SizeBytes % c_memoryGranularity != 0);
    ValidatePath(Request.Boot.KernelPath);
    if (!Request.Boot.InitrdPath.empty())
    {
        ValidatePath(Request.Boot.InitrdPath);
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
    settings.SchemaVersion = {2, windows11 ? 7u : 3u};
    auto& vm = settings.VirtualMachine;
    vm.StopOnReset = true;
    vm.Chipset.UseUtc = true;
    vm.Devices.Plan9.reset();
    vm.Devices.Battery.reset();
    switch (description.Boot.Method)
    {
    case VmBootMethod::LinuxDirect:
        THROW_HR_IF(c_notSupported, wsl::shared::Arm64);
        vm.Chipset.LinuxKernelDirect =
            schema::LinuxKernelDirect{Request.Boot.KernelPath.native(), Request.Boot.InitrdPath.native(), Request.Boot.KernelCommandLine};
        break;
    case VmBootMethod::Uefi:
        // Firmware resolves initrd paths relative to this directory; it does not consume InitRdPath.
        THROW_HR_IF(
            E_INVALIDARG,
            !Request.Boot.InitrdPath.empty() && _wcsicmp(
                                                    Request.Boot.InitrdPath.parent_path().lexically_normal().c_str(),
                                                    Request.Boot.KernelPath.parent_path().lexically_normal().c_str()) != 0);
        vm.Chipset.Uefi = schema::Uefi{schema::UefiBootEntry{
            schema::UefiBootDevice::VmbFs,
            Request.Boot.KernelPath.parent_path().native(),
            L"\\" + Request.Boot.KernelPath.filename().native(),
            Request.Boot.KernelCommandLine}};
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }

    bool nestedVirtualization = false;
    if (Request.Processor.NestedVirtualization != VmFeatureRequest::Disabled && windows11)
    {
        const auto& features = schema::GetProcessorFeatures();
        nestedVirtualization = std::find(features.begin(), features.end(), "NestedVirt") != features.end();
    }
    description.Processor.NestedVirtualization = ResolveFeature(Request.Processor.NestedVirtualization, nestedVirtualization);

    bool pmu = false;
    bool lbr = false;
#ifdef _AMD64_
    HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
    __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
    pmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
    lbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;
#endif
    description.Processor.PerfmonPmu = ResolveFeature(Request.Processor.PerfmonPmu, pmu);
    description.Processor.PerfmonLbr = ResolveFeature(Request.Processor.PerfmonLbr, lbr);
    auto& processor = vm.ComputeTopology.Processor;
    processor.Count = description.Processor.Count;
    processor.ExposeVirtualizationExtensions = description.Processor.NestedVirtualization;
    processor.EnablePerfmonPmu = description.Processor.PerfmonPmu;
    processor.EnablePerfmonLbr = description.Processor.PerfmonLbr;

    description.Memory.AllowOvercommit = ResolveFeature(Request.Memory.AllowOvercommit, true);
    description.Memory.DeferredCommit = ResolveFeature(Request.Memory.DeferredCommit, true);
    description.Memory.ColdDiscard = ResolveFeature(Request.Memory.ColdDiscard, true);
    THROW_HR_IF(E_INVALIDARG, description.Memory.DeferredCommit && !description.Memory.AllowOvercommit);
    auto& memory = vm.ComputeTopology.Memory;
    memory.SizeInMB = description.Memory.SizeBytes / c_mib;
    memory.AllowOvercommit = description.Memory.AllowOvercommit;
    memory.EnableDeferredCommit = description.Memory.DeferredCommit;
    memory.EnableColdDiscardHint = description.Memory.ColdDiscard;

    std::set<std::wstring> consoleNames;
    for (const auto& console : Request.Consoles)
    {
        if (const auto* serial = std::get_if<VmSerialConsole>(&console.Device))
        {
            ValidateConsolePath(serial->NamedPipe);
            THROW_HR_IF(E_INVALIDARG, serial->Port > 1 || (wsl::shared::Arm64 && serial->Port != 0));
            THROW_HR_IF(
                E_INVALIDARG,
                !vm.Devices.ComPorts.emplace(std::to_string(serial->Port), schema::ComPort{serial->NamedPipe.native()}).second);
        }
        else
        {
            const auto& virtio = std::get<VmVirtioConsole>(console.Device);
            THROW_HR_IF(c_notSupported, !helpers::IsVirtioSerialConsoleSupported());
            ValidateConsolePath(virtio.NamedPipe);
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
            ValidatePath(source->Path);
            THROW_HR_IF(E_INVALIDARG, source->Format != VmDiskFormat::Vhd && source->Format != VmDiskFormat::Vhdx);
            attachment.Type = schema::AttachmentType::VirtualDisk;
            attachment.Path = source->Path.native();
            attachment.SupportCompressedVolumes = true;
            attachment.AlwaysAllowSparseFiles = true;
            attachment.SupportEncryptedFiles = true;
        }
        else
        {
            const auto& physical = std::get<VmPhysicalDiskSource>(disk.Disk.Source);
            ValidatePath(physical.DevicePath);
            attachment.Type = schema::AttachmentType::PassThru;
            attachment.Path = physical.DevicePath;
        }
        scsi.Attachments.emplace(std::to_string(lun), std::move(attachment));
        description.BootDisks.at(disk.Key) = {{description.Identity, nextId++}, {0, lun}, disk.Disk.ReadOnly};
    }

    if (Request.CrashCapture)
    {
        THROW_HR_IF(E_INVALIDARG, Request.CrashCapture->Policy != VmSelectionPolicy::Required && Request.CrashCapture->Policy != VmSelectionPolicy::Preferred);
        ValidatePath(Request.CrashCapture->SavedStatePath);
        THROW_HR_IF(c_notSupported, !windows11 && Request.CrashCapture->Policy == VmSelectionPolicy::Required);
        if (windows11)
        {
            vm.DebugOptions.BugcheckSavedStateFileName = Request.CrashCapture->SavedStatePath.native();
        }
    }

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(Request.Identity.UserToken.get());
    wil::unique_hlocal_string userSid;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(tokenUser->User.Sid, &userSid));
    const auto securityDescriptor = std::format(L"D:P(A;;FA;;;SY)(A;;FA;;;{})", userSid.get());
    vm.Devices.HvSocket.HvSocketConfig.DefaultBindSecurityDescriptor = securityDescriptor;
    vm.Devices.HvSocket.HvSocketConfig.DefaultConnectSecurityDescriptor = securityDescriptor;
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
    const auto json = wsl::shared::ToJsonW(configuration.Settings);
    m_state->m_description = std::move(configuration.Description);
    m_state->m_system = schema::CreateComputeSystem(id.c_str(), json.c_str());
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
    THROW_HR(E_NOTIMPL);
}

void HcsVirtualMachineBackend::Start()
{
    THROW_HR(E_NOTIMPL);
}

void HcsVirtualMachineBackend::Terminate()
{
    THROW_HR(E_NOTIMPL);
}

void HcsVirtualMachineBackend::CancelPendingOperations() noexcept
{
    LOG_HR(E_NOTIMPL);
}

VmGuestListener HcsVirtualMachineBackend::CreateGuestListener(GuestServicePort Port)
{
    THROW_HR(E_NOTIMPL);
}

wil::unique_socket HcsVirtualMachineBackend::AcceptGuestConnection(VmListenerId Listener)
{
    THROW_HR(E_NOTIMPL);
}

wil::unique_socket HcsVirtualMachineBackend::ConnectGuest(GuestServicePort Port)
{
    THROW_HR(E_NOTIMPL);
}

void HcsVirtualMachineBackend::CloseGuestListener(VmListenerId Listener)
{
    THROW_HR(E_NOTIMPL);
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
