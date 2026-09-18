// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "IVirtualMachineBackend.h"
#include "hvsocket.hpp"
#include "socket.hpp"

VmGuestListener IVirtualMachineBackend::RegisterGuestListener(const VmInstanceId& Identity, GuestServicePort Port)
{
    auto lock = m_guestListenersLock.lock_exclusive();
    THROW_HR_IF(E_BOUNDS, m_nextListenerId == UINT64_MAX);
    for (const auto& entry : m_guestListeners)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), entry.second->Listener.Port.Value == Port.Value);
    }

    const VmGuestListener listener{{Identity, m_nextListenerId}, Port};
    auto state = ConfigureGuestListener(listener);
    THROW_HR_IF(E_UNEXPECTED, !state);
    THROW_HR_IF(E_UNEXPECTED, state->Listener.Id.Value != listener.Id.Value || state->Listener.Port.Value != listener.Port.Value);

    const auto inserted = m_guestListeners.emplace(listener.Id.Value, std::move(state)).second;
    WI_ASSERT(inserted);
    ++m_nextListenerId;
    return listener;
}

wil::unique_socket IVirtualMachineBackend::AcceptGuestListenerConnection(VmListenerId Listener, const VmInstanceId& Identity) const
{
    THROW_HR_IF(E_INVALIDARG, Listener.Value == 0 || !IsEqualGUID(Listener.Owner.VmId, Identity.VmId));
    std::shared_ptr<VmGuestListenerState> listener;
    {
        auto lock = m_guestListenersLock.lock_shared();
        const auto entry = m_guestListeners.find(Listener.Value);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), entry == m_guestListeners.end());
        listener = entry->second;
    }

    auto socket = wsl::windows::common::socket::CancellableAccept(listener->Socket.get(), INFINITE, listener->CancellationEvent.get());
    THROW_HR_IF(E_ABORT, !socket);
    return std::move(*socket);
}

std::shared_ptr<VmGuestListenerState> IVirtualMachineBackend::RemoveGuestListener(VmListenerId Listener, const VmInstanceId& Identity)
{
    THROW_HR_IF(E_INVALIDARG, Listener.Value == 0 || !IsEqualGUID(Listener.Owner.VmId, Identity.VmId));
    auto lock = m_guestListenersLock.lock_exclusive();
    const auto entry = m_guestListeners.find(Listener.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), entry == m_guestListeners.end());
    THROW_IF_WIN32_BOOL_FALSE(SetEvent(entry->second->CancellationEvent.get()));
    auto result = std::move(entry->second);
    m_guestListeners.erase(entry);
    return result;
}

void IVirtualMachineBackend::CloseGuestListeners() noexcept
{
    auto lock = m_guestListenersLock.lock_exclusive();
    for (const auto& entry : m_guestListeners)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetEvent(entry.second->CancellationEvent.get()));
    }

    m_guestListeners.clear();
}

std::shared_ptr<VmGuestListenerState> IVirtualMachineBackend::ConfigureGuestListener(const VmGuestListener& Listener)
{
    auto state = std::make_shared<VmGuestListenerState>();
    state->Listener = Listener;
    state->Socket = wsl::windows::common::hvsocket::Listen(Listener.Id.Owner.VmId, Listener.Port.Value);
    return state;
}
