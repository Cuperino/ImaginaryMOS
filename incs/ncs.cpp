// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <incs/ncs.h>

#include <utility>

namespace incs {

namespace {

imos::profiles::MachineInfo asNcs(imos::profiles::MachineInfo info) {
    info.deviceType = imos::profiles::DeviceType::Ncs;
    return info;
}

}  // namespace

Ncs::Ncs(NcsConfig config, imos::PeerSession::Dependencies deps)
    : session_(std::move(config.session), deps),
      profile0_(asNcs(std::move(config.machineInfo)), &session_, deps.clock),
      directory_(&session_, deps.clock) {}

void Ncs::onChannelConnected(imos::Channel channel) {
    session_.onChannelConnected(channel);
    dispatch();
}

void Ncs::onChannelDisconnected(imos::Channel channel) {
    session_.onChannelDisconnected(channel);
}

void Ncs::onFrameReceived(imos::Channel channel, std::span<const std::uint8_t> frame) {
    session_.onFrameReceived(channel, frame);
    dispatch();
}

void Ncs::poll() {
    session_.poll();
    dispatch();
}

std::optional<imos::MonotonicTime> Ncs::nextDeadline() const {
    return session_.nextDeadline();
}

imos::Result<imos::Seq> Ncs::requestObject(std::string_view objID) {
    imos::Result<imos::Seq> seq = directory_.requestObject(objID);
    dispatch();
    return seq;
}

imos::Result<imos::Seq> Ncs::requestAll(int pauseSeconds) {
    imos::Result<imos::Seq> seq = directory_.requestAll(pauseSeconds);
    dispatch();
    return seq;
}

const imos::codec::Message* Ncs::object(std::string_view objID) const {
    return directory_.object(objID);
}

std::vector<std::string> Ncs::objectIDs() const {
    return directory_.objectIDs();
}

std::optional<imos::profiles::DirectoryEvent> Ncs::nextEvent() {
    return directory_.nextEvent();
}

void Ncs::dispatch() {
    while (std::optional<imos::SessionEvent> event = session_.nextEvent()) {
        if (profile0_.handle(*event) || directory_.handle(*event)) {
            continue;
        }
        appEvents_.push_back(std::move(*event));
    }
}

}  // namespace incs
