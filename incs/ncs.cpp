// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <incs/ncs.h>

#include <utility>

namespace incs {

namespace {

imos::profiles::MachineInfo asNcs(imos::profiles::MachineInfo info, bool claimsProfile6) {
    info.deviceType = imos::profiles::DeviceType::Ncs;
    if (claimsProfile6) {
        info.profiles[6] = true;
    }
    return info;
}

std::optional<imos::Error> armProfile6(imos::profiles::Profile6Classifier& classifier,
                                       const NcsConfig& config) {
    imos::Result<void> valid = Ncs::validateConfig(config);
    if (!valid.ok()) {
        return std::move(valid).error();
    }
    // Classification is relative to the peer device: an authored reference
    // that is Foreign to it is what would demand an out-of-band transfer.
    // An identity-only config (requireFqidSelf alone) arms nothing — its
    // peer need not be an FQID device.
    if (config.profile6.flagForeignReferences || config.profile6.rejectForeignReferences) {
        (void)classifier.configure(config.profile6, config.session.mosID);  // validated above
    }
    return std::nullopt;
}

}  // namespace

imos::Result<void> Ncs::validateConfig(const NcsConfig& config) {
    if (!config.profile6.enabled()) {
        return {};
    }
    // Identity: the spec's FQID convention covers ncsIDs too.
    if (imos::Result<void> self =
            imos::profiles::validateProfile6Identity(config.profile6, config.session.ncsID);
        !self.ok()) {
        return self;
    }
    if (config.profile6.flagForeignReferences || config.profile6.rejectForeignReferences) {
        return imos::profiles::validateProfile6Identity(config.profile6, config.session.mosID);
    }
    return {};
}

Ncs::Ncs(NcsConfig config, imos::PeerSession::Dependencies deps)
    : profile6Error_(armProfile6(profile6_, config)),
      session_(std::move(config.session), deps),
      profile0_(asNcs(std::move(config.machineInfo),
                      !profile6Error_.has_value() && config.profile6.enabled()),
                &session_, deps.clock),
      directory_(&session_, deps.clock),
      author_(&session_),
      profile2_(&session_, deps.clock),
      profile4_(&session_),
      profile5_(&session_),
      profile7_(&author_, &defaultLeasePolicy_, &profile0_, &session_, deps.clock) {
    profile2_.setSource(&author_);           // roReq is answered from the authoring store
    profile4_.setStore(&author_.desired());  // roReqAll enumerates the same store
    author_.setProfile6(&profile6_);         // authoring-time foreign-reference rejection
    if (profile6Error_.has_value()) {
        // Defense in depth for hosts that skipped validateConfig(): run
        // without Profile 6 and surface the bring-up diagnostic once.
        imos::SessionEvent event;
        event.type = imos::SessionEventType::ProtocolError;
        event.diagnostic = profile6Error_->message;
        session_.postEvent(std::move(event));
    }
}

imos::Locality Ncs::classifyMosID(std::string_view mosID) const {
    return profile6_.classify(mosID);
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

imos::Result<imos::Seq> Ncs::requestMachineInfo(imos::Channel channel) {
    imos::Result<imos::Seq> seq = profile0_.requestMachineInfo(channel);
    dispatch();
    return seq;
}

const imos::profiles::PeerMachineInfo* Ncs::peerMachineInfo() const {
    return profile0_.peerMachineInfo();
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

std::optional<imos::profiles::DirectoryEvent> Ncs::nextDirectoryEvent() {
    return directory_.nextEvent();
}

std::optional<imos::SessionEvent> Ncs::nextEvent() {
    if (appEvents_.empty()) {
        return std::nullopt;
    }
    imos::SessionEvent event = std::move(appEvents_.front());
    appEvents_.pop_front();
    return event;
}

imos::Result<imos::Seq> Ncs::sendStory(std::string_view roID, std::string_view storyID) {
    imos::Result<imos::Seq> seq = author_.sendStory(roID, storyID);
    dispatch();
    return seq;
}

imos::Result<imos::Seq> Ncs::sendCtrl(imos::profiles::CtrlScope scope, std::string_view roID,
                                      std::string_view storyID, std::string_view itemID,
                                      imos::profiles::CtrlCommand command) {
    imos::Result<imos::Seq> seq = profile5_.sendCtrl(scope, roID, storyID, itemID, command);
    dispatch();
    return seq;
}

imos::Result<imos::Seq> Ncs::sendItemCue(const imos::profiles::ItemCue& cue) {
    imos::Result<imos::Seq> seq = profile5_.sendItemCue(cue);
    dispatch();
    return seq;
}

std::optional<imos::profiles::CtrlResult> Ncs::nextCtrlResult() {
    return profile5_.nextResult();
}

std::optional<imos::profiles::ItemCue> Ncs::nextItemCue() {
    return profile5_.nextItemCue();
}

void Ncs::setLeasePolicy(ILeasePolicy* policy) noexcept {
    if (policy != nullptr) {
        profile7_.setLeasePolicy(policy);
    }
}

void Ncs::setStoryActionCapabilityPolicy(Profile7NcsResponder::CapabilityPolicy policy) noexcept {
    profile7_.setCapabilityPolicy(policy);
}

std::optional<StoryActionAudit> Ncs::nextStoryActionAudit() {
    return profile7_.nextAudit();
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
            profile2_.handle(*event) || profile4_.handle(*event) || profile5_.handle(*event) ||
            profile7_.handle(*event)) {
            continue;
        }
        appEvents_.push_back(std::move(*event));
    }
}

}  // namespace incs
