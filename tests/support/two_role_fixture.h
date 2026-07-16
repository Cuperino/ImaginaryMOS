// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// TwoRoleFixture — a real imos::Device wired to a real incs::Ncs over a
// loopback ITransport pair and in-memory stores, sharing one fake clock. This
// is the conformance rig the profile suites run against: enqueue on one role,
// pump(), and observe the mirror on the other. pump() moves one hop per
// direction so tests can reason a round-trip at a time; settle() drives the
// link to quiescence for the common "just let it finish" case.

#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include <imos/device.h>
#include <imos/session/in_memory_store.h>
#include <incs/ncs.h>

#include "fakes.h"

namespace imos::test {

struct TwoRoleFixture {
    static constexpr const char* kMosID = "device.example.mos";
    static constexpr const char* kNcsID = "ncs.example.com";

    FakeClock clock;
    FakeRandom random;
    LoopbackEndpoint deviceTransport;
    LoopbackEndpoint ncsTransport;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;
    std::unique_ptr<imos::Device> device;
    std::unique_ptr<incs::Ncs> ncs;

    TwoRoleFixture() {
        device = std::make_unique<imos::Device>(
            imos::DeviceConfig{sessionConfig(), machineInfo(imos::profiles::DeviceType::Mos)},
            imos::PeerSession::Dependencies{&clock, &deviceTransport, &deviceStore, nullptr,
                                            &random, nullptr});
        ncs = std::make_unique<incs::Ncs>(
            incs::NcsConfig{sessionConfig(), machineInfo(imos::profiles::DeviceType::Ncs)},
            imos::PeerSession::Dependencies{&clock, &ncsTransport, &ncsStore, nullptr, &random,
                                            nullptr});
    }

    static imos::SessionConfig sessionConfig() {
        imos::SessionConfig config;
        config.mosID = kMosID;
        config.ncsID = kNcsID;
        config.retryTimeoutMillis = 10'000;
        config.maxRetries = 3;
        config.keepAliveIntervalMillis = 0;  // keep the wire quiet under test
        return config;
    }

    static imos::profiles::MachineInfo machineInfo(imos::profiles::DeviceType type) {
        imos::profiles::MachineInfo info;
        info.manufacturer = "Imaginary";
        info.model = type == imos::profiles::DeviceType::Mos ? "VTR-7" : "NCS-1";
        info.swRev = "1.0.0";
        info.id = type == imos::profiles::DeviceType::Mos ? kMosID : kNcsID;
        info.deviceType = type;
        info.profiles = {true, true, true, false, false, false, false, false};
        return info;
    }

    void connectAll() {
        for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
            deviceTransport.connected[i] = true;
            ncsTransport.connected[i] = true;
            device->onChannelConnected(static_cast<imos::Channel>(i));
            ncs->onChannelConnected(static_cast<imos::Channel>(i));
        }
    }

    /// Deliver every currently-queued frame, one hop per direction. Frames
    /// produced during delivery land in the live queues and wait for the next
    /// pump. Returns whether anything was delivered.
    bool pump() {
        std::array<std::deque<std::vector<std::uint8_t>>, imos::kChannelCount> deviceOut;
        std::array<std::deque<std::vector<std::uint8_t>>, imos::kChannelCount> ncsOut;
        for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
            deviceOut[i].swap(deviceTransport.outbound[i]);
            ncsOut[i].swap(ncsTransport.outbound[i]);
        }
        bool delivered = false;
        for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
            const auto channel = static_cast<imos::Channel>(i);
            for (const auto& frame : deviceOut[i]) {
                delivered = true;
                ncs->onFrameReceived(channel, frame);
            }
            for (const auto& frame : ncsOut[i]) {
                delivered = true;
                device->onFrameReceived(channel, frame);
            }
        }
        return delivered;
    }

    /// Poll both roles and pump until the link goes quiet (bounded).
    void settle(int maxRounds = 64) {
        for (int round = 0; round < maxRounds; ++round) {
            device->poll();
            ncs->poll();
            if (!pump()) {
                return;
            }
        }
    }
};

}  // namespace imos::test
