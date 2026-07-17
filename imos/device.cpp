// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/device.h>

#include <algorithm>
#include <utility>

namespace imos {

namespace {

profiles::MachineInfo asDevice(profiles::MachineInfo info) {
    info.deviceType = profiles::DeviceType::Mos;
    return info;
}

}  // namespace

Device::Device(DeviceConfig config, PeerSession::Dependencies deps)
    : session_(std::move(config.session), deps),
      profile0_(asDevice(std::move(config.machineInfo)), &session_, deps.clock),
      profile1_(nullptr, &session_, deps.clock),
      profile2_(&session_, deps.clock),
      profile4_(&session_, &profile2_),
      profile5_(&session_, &profile2_),
      profile7_(&session_),
      hydrateOnConnect_(config.hydrateRunningOrdersOnConnect) {}

void Device::attach(profiles::IObjectSource& source) {
    profile1_.setSource(&source);
    profile2_.setObjectSource(&source);
}

void Device::attach(profiles::IRunningOrderSink& sink) {
    profile2_.setSink(&sink);
}

void Device::attach(profiles::IStorySink& sink) {
    profile4_.setSink(&sink);
}

void Device::attach(profiles::IControlSink& sink) {
    profile5_.setSink(&sink);
}

void Device::setUnknownObjectPolicy(profiles::Profile2Responder::UnknownObjectPolicy policy) {
    profile2_.setUnknownObjectPolicy(policy);
}

profiles::RunningOrderModel& Device::runningOrderModel() noexcept {
    return profile2_.model();
}

const profiles::RunningOrderModel& Device::runningOrderModel() const noexcept {
    return profile2_.model();
}

void Device::onChannelConnected(Channel channel) {
    session_.onChannelConnected(channel);
    if (channel == Channel::Ro && hydrateOnConnect_) {
        (void)profile4_.requestAllRunningOrders(true);
    }
    dispatch();
}

void Device::onChannelDisconnected(Channel channel) {
    session_.onChannelDisconnected(channel);
}

void Device::onFrameReceived(Channel channel, std::span<const std::uint8_t> frame) {
    session_.onFrameReceived(channel, frame);
    dispatch();
}

void Device::poll() {
    session_.poll();
    profile1_.poll();
    dispatch();
}

std::optional<MonotonicTime> Device::nextDeadline() const {
    const std::optional<MonotonicTime> session = session_.nextDeadline();
    const std::optional<MonotonicTime> paced = profile1_.nextDeadline();
    if (!session.has_value()) {
        return paced;
    }
    if (!paced.has_value()) {
        return session;
    }
    return std::min(*session, *paced);
}

Result<Seq> Device::announceObject(codec::Message mosObj) {
    return profile1_.announceObject(std::move(mosObj));
}

Result<Seq> Device::requestMachineInfo(Channel channel) {
    return profile0_.requestMachineInfo(channel);
}

const profiles::PeerMachineInfo* Device::peerMachineInfo() const {
    return profile0_.peerMachineInfo();
}

Result<Seq> Device::reportElementStatus(const profiles::ElementStatusReport& report) {
    return profile2_.sendElementStat(report);
}

Result<Seq> Device::requestAllRunningOrders(bool hydrate) {
    return profile4_.requestAllRunningOrders(hydrate);
}

std::vector<std::string> Device::runningOrderHeaderIDs() const {
    return profile4_.headerIDs();
}

const codec::XmlNode* Device::runningOrderHeader(std::string_view roID) const {
    return profile4_.header(roID);
}

const codec::Message* Device::story(std::string_view roID, std::string_view storyID) const {
    return profile4_.story(roID, storyID);
}

std::vector<std::string> Device::storyIDs(std::string_view roID) const {
    return profile4_.storyIDs(roID);
}

Result<Seq> Device::sendItemCue(const profiles::ItemCue& cue, std::string_view addresseeNcsID) {
    return profile5_.sendItemCue(cue, addresseeNcsID);
}

std::optional<profiles::CtrlRequest> Device::nextCtrlRequest() {
    return profile5_.nextCtrl();
}

std::optional<profiles::ItemCue> Device::nextItemCue() {
    return profile5_.nextItemCue();
}

Result<Seq> Device::requestStoryAction(const profiles::StoryActionRequest& request) {
    return profile7_.requestStoryAction(request);
}

std::optional<profiles::StoryActionResult> Device::nextStoryActionResult() {
    return profile7_.nextResult();
}

std::optional<profiles::ElementStatusReport> Device::nextElementStatus() {
    return profile2_.nextStatus();
}

std::optional<SessionEvent> Device::nextEvent() {
    if (appEvents_.empty()) {
        return std::nullopt;
    }
    SessionEvent event = std::move(appEvents_.front());
    appEvents_.pop_front();
    return event;
}

void Device::dispatch() {
    while (std::optional<SessionEvent> event = session_.nextEvent()) {
        if (profile0_.handle(*event) || profile1_.handle(*event) || profile2_.handle(*event) ||
            profile4_.handle(*event) || profile5_.handle(*event) || profile7_.handle(*event)) {
            continue;
        }
        appEvents_.push_back(std::move(*event));
    }
}

}  // namespace imos
