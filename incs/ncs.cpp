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
      directory_(&session_, deps.clock),
      author_(&session_),
      profile2_(&session_, deps.clock),
      profile4_(&session_) {
    profile2_.setSource(&author_);           // roReq is answered from the authoring store
    profile4_.setStore(&author_.desired());  // roReqAll enumerates the same store
}

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

imos::Result<imos::Seq> Ncs::sendStory(std::string_view roID, std::string_view storyID) {
    imos::Result<imos::Seq> seq = author_.sendStory(roID, storyID);
    dispatch();
    return seq;
}

imos::Result<imos::Seq> Ncs::sendElementStat(const imos::profiles::ElementStatusReport& report) {
    imos::Result<imos::Seq> seq = profile2_.sendElementStat(report);
    dispatch();
    return seq;
}

std::optional<imos::profiles::ElementStatusReport> Ncs::nextElementStatus() {
    return profile2_.nextStatus();
}

void Ncs::dispatch() {
    while (std::optional<imos::SessionEvent> event = session_.nextEvent()) {
        if (profile0_.handle(*event) || directory_.handle(*event) || author_.handle(*event) ||
            profile2_.handle(*event) || profile4_.handle(*event)) {
            continue;
        }
        appEvents_.push_back(std::move(*event));
    }
}

}  // namespace incs
