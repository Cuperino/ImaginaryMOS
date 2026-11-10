// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/profile0.h>

#include <charconv>
#include <utility>

#include <imos/codec/gen/views.h>
#include <imos/time_format.h>

namespace imos::profiles {

std::string_view deviceTypeName(DeviceType type) noexcept {
    return type == DeviceType::Ncs ? "NCS" : "MOS";
}

codec::Message buildListMachInfo(const MachineInfo& info, WallClockTime now) {
    codec::Message message = codec::makeMessage(codec::MessageType::ListMachInfo);
    codec::XmlNode* body = message.body();
    codec::view::ListMachInfo view{*body};
    view.setManufacturer(info.manufacturer);
    view.setModel(info.model);
    view.setHwRev(info.hwRev);
    view.setSwRev(info.swRev);
    view.setDom(info.dom);
    view.setSn(info.serialNumber);
    view.setId(info.id);
    view.setTime(formatSpecTime(now));
    if (info.opTime.has_value()) {
        view.setOpTime(formatSpecTime(*info.opTime));
    }
    view.setMosRev(info.mosRev);

    codec::XmlNode& profiles = view.ensureSupportedProfiles();
    profiles.setAttribute("deviceType", deviceTypeName(info.deviceType));
    for (int n = 0; n < kProfileCount; ++n) {
        codec::XmlNode& profile = profiles.appendElement("mosProfile");
        profile.setAttribute("number", std::to_string(n));
        profile.setText(info.profiles[static_cast<std::size_t>(n)] ? "YES" : "NO");
    }
    return message;
}

Result<PeerMachineInfo> parseListMachInfo(const codec::Message& message) {
    if (message.type() != codec::MessageType::ListMachInfo) {
        return makeError(ErrorCode::InvalidArgument, std::string{"expected listMachInfo, got "} +
                                                         std::string{message.info().wireName});
    }
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::ParseError, "listMachInfo has no body element");
    }

    PeerMachineInfo peer;
    const auto grab = [body](std::string_view name) -> std::string {
        const codec::XmlNode* child = body->firstChildElement(name);
        return child == nullptr ? std::string{} : child->text();
    };
    peer.info.manufacturer = grab("manufacturer");
    peer.info.model = grab("model");
    peer.info.hwRev = grab("hwRev");
    peer.info.swRev = grab("swRev");
    peer.info.dom = grab("DOM");
    peer.info.serialNumber = grab("SN");
    peer.info.id = grab("ID");
    peer.info.mosRev = grab("mosRev");
    peer.time = grab("time");
    peer.opTime = grab("opTime");

    if (const codec::XmlNode* profiles = body->firstChildElement("supportedProfiles")) {
        peer.hasSupportedProfiles = true;
        if (const std::optional<std::string_view> type = profiles->attribute("deviceType")) {
            if (*type == "NCS") {
                peer.info.deviceType = DeviceType::Ncs;
            }  // anything else (incl. dialect strays) keeps the MOS default
        }
        for (const codec::XmlNode& child : profiles->children) {
            if (!child.isElement("mosProfile")) {
                continue;
            }
            const std::optional<std::string_view> number = child.attribute("number");
            if (!number.has_value()) {
                continue;
            }
            int n = -1;
            const auto [ptr, ec] =
                std::from_chars(number->data(), number->data() + number->size(), n);
            if (ec != std::errc{} || ptr != number->data() + number->size() || n < 0 ||
                n >= kProfileCount) {
                continue;  // unknown profile numbers are ignored, per the unknown-tag spirit
            }
            peer.info.profiles[static_cast<std::size_t>(n)] = child.text() == "YES";
        }
    }
    return peer;
}

Profile0Responder::Profile0Responder(MachineInfo info, PeerSession* session, IClock* clock)
    : info_(std::move(info)), session_(session), clock_(clock) {}

bool Profile0Responder::handle(const SessionEvent& event) {
    switch (event.type) {
        case SessionEventType::MessageReceived:
            if (event.message.type() == codec::MessageType::ReqMachInfo) {
                codec::Message reply = buildListMachInfo(info_, clock_->wallClock());
                codec::echoMessageID(event.message, reply);
                // A terminal reply: it completes the peer's exchange and
                // must not wait behind a request of our own in flight.
                (void)session_->enqueueReply(event.channel, std::move(reply));
                return true;
            }
            if (event.message.type() == codec::MessageType::ListMachInfo) {
                // Unsolicited peer listMachInfo (not our reqMachInfo reply).
                if (auto result = parseListMachInfo(event.message); result.ok()) {
                    peerInfo_ = result.value();
                }
                return false;
            }
            return false;
        case SessionEventType::AckReceived:
            if (pendingReqMachInfo_ && event.seq == *pendingReqMachInfo_ &&
                event.channel == pendingReqMachInfoChannel_ &&
                event.message.type() == codec::MessageType::ListMachInfo) {
                pendingReqMachInfo_.reset();
                if (auto result = parseListMachInfo(event.message); result.ok()) {
                    peerInfo_ = result.value();
                }
                return true;
            }
            return false;
        default:
            return false;
    }
}

Result<Seq> Profile0Responder::requestMachineInfo(Channel channel) {
    // reqMachInfo is ChannelAffinity::AnyChannel, so the channel-inferring
    // enqueue overload rejects it; the channel must be named explicitly.
    Result<Seq> seq =
        session_->enqueue(channel, codec::makeMessage(codec::MessageType::ReqMachInfo));
    if (seq.ok()) {
        pendingReqMachInfo_ = seq.value();
        pendingReqMachInfoChannel_ = channel;
    }
    return seq;
}

const PeerMachineInfo* Profile0Responder::peerMachineInfo() const noexcept {
    return peerInfo_.has_value() ? &*peerInfo_ : nullptr;
}

}  // namespace imos::profiles
