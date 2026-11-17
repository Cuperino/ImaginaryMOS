// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/transports/ws3.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>

#include <imos/codec/message.h>
#include <imos/transports/ws3_transform.h>
#include <imos/ws40/basic_auth.h>

namespace imos::transports::ws3 {

namespace {

constexpr int kEphemeralBindAttempts = 8;

struct QueuedEvent {
    enum class Kind : std::uint8_t { Frame, Diagnostic, Up, Down };
    Kind kind = Kind::Frame;
    Channel channel = Channel::Mom;
    std::string frame;  ///< UTF-8 classic document
    std::string diagnostic;
};

struct OutboundCall {
    Channel channel = Channel::Mom;
    SoapRequest request;
};

/// An inbound HTTP exchange waiting for the session's reply.
struct ParkedExchange {
    std::string operation;
    std::promise<std::string> response;
};

/// The channel a classic message belongs on (for feeding inbound calls to
/// the session); Profile 0's any-channel messages default to mom.
Channel channelForMessage(const codec::Message& message) {
    return channelFor(message.info().channel).value_or(Channel::Mom);
}

}  // namespace

struct Ws3Endpoint::Impl {
    EndpointConfig config;
    ILogger* logger = nullptr;
    Ws3ErrorHandler errorHandler;
    ActivityHandler activity;  ///< set before start(); read on worker threads

    std::unique_ptr<ix::HttpServer> server;
    int boundPort = 0;
    std::atomic<bool> started{false};

    std::mutex eventMutex;
    std::deque<QueuedEvent> events;
    bool connectedAnnounced = false;
    /// Whether the peer's service answers (EndpointConfig::reachProbeMillis).
    std::atomic<bool> peerUp{true};

    // Outbound worker: one thread performing the blocking HTTP calls.
    std::thread worker;
    std::mutex workMutex;
    std::condition_variable workCv;
    std::deque<OutboundCall> work;
    bool workerQuit = false;

    // Parked inbound exchanges, keyed by the request's messageID.
    std::mutex parkedMutex;
    std::map<std::string, std::shared_ptr<ParkedExchange>> parked;

    void log(LogLevel level, const std::string& text) {
        if (logger != nullptr) {
            logger->log(level, text);
        }
    }

    void postEvent(QueuedEvent event) {
        {
            const std::scoped_lock lock{eventMutex};
            events.push_back(std::move(event));
        }
        if (activity) {
            activity();
        }
    }

    void postDiagnostic(Channel channel, std::string text) {
        postEvent({QueuedEvent::Kind::Diagnostic, channel, {}, std::move(text)});
    }

    void markUp() {
        if (!peerUp.exchange(true)) {
            postEvent({QueuedEvent::Kind::Up, Channel::Mom, {}, {}});
            workCv.notify_one();  // the calls held while it was away
        }
    }

    void markDown() {
        if (peerUp.exchange(false)) {
            postEvent({QueuedEvent::Kind::Down, Channel::Mom, {}, {}});
        }
    }

    // ---- server side ----------------------------------------------------

    ix::HttpResponsePtr handleRequest(const ix::HttpRequestPtr& request) {
        const auto fault = [](int status, std::string_view code, std::string_view text) {
            ix::WebSocketHttpHeaders headers;
            headers["Content-Type"] = "text/xml; charset=utf-8";
            return std::make_shared<ix::HttpResponse>(status, "Error", ix::HttpErrorCode::Ok,
                                                      headers, buildSoapFault(code, text));
        };
        if (request->method != "POST") {
            return fault(405, "Client", "only POST is supported");
        }
        markUp();  // the peer is there: it called
        Result<InboundCall> call = parseSoapRequest(request->body);
        if (!call.ok()) {
            postDiagnostic(Channel::Mom, "inbound SOAP rejected: " + call.error().message);
            return fault(400, "Client", call.error().message);
        }

        const Channel channel = channelForMessage(call.value().message);
        auto exchange = std::make_shared<ParkedExchange>();
        exchange->operation = call.value().operation;
        std::future<std::string> pending = exchange->response.get_future();
        const std::string key = call.value().messageId;
        {
            const std::scoped_lock lock{parkedMutex};
            parked[key] = exchange;
        }
        postEvent(
            {QueuedEvent::Kind::Frame, channel, codec::serializeMessage(call.value().message), {}});

        const auto deadline = std::chrono::milliseconds{config.inboundReplyTimeoutMillis};
        ix::WebSocketHttpHeaders headers;
        headers["Content-Type"] = "text/xml; charset=utf-8";
        if (pending.wait_for(deadline) == std::future_status::ready) {
            return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok, headers,
                                                      pending.get());
        }
        {
            const std::scoped_lock lock{parkedMutex};
            parked.erase(key);
        }
        postDiagnostic(channel, "parked <" + exchange->operation +
                                    "> exchange timed out waiting for the session's reply");
        return fault(500, "Server", "no reply from the MOS session within the timeout");
    }

    // ---- outbound worker -------------------------------------------------

    void workerLoop() {
        ix::HttpClient client;
        const auto probeEvery = std::chrono::milliseconds{std::max(config.reachProbeMillis, 10)};
        while (true) {
            OutboundCall call;
            {
                std::unique_lock lock{workMutex};
                if (peerUp && config.idleProbeMillis > 0) {
                    const bool woken =
                        workCv.wait_for(lock, std::chrono::milliseconds{config.idleProbeMillis},
                                        [&] { return workerQuit || !work.empty() || !peerUp; });
                    if (!woken) {  // idle: is it still there?
                        lock.unlock();
                        if (!probe(client)) {
                            markDown();
                        }
                        continue;
                    }
                } else if (peerUp) {
                    workCv.wait(lock, [&] { return workerQuit || !work.empty() || !peerUp; });
                } else {
                    workCv.wait_for(lock, probeEvery, [&] { return workerQuit || peerUp.load(); });
                }
                if (workerQuit && (work.empty() || !peerUp)) {
                    return;
                }
                if (!peerUp) {
                    lock.unlock();
                    probe(client);
                    continue;
                }
                if (work.empty()) {
                    continue;
                }
                call = std::move(work.front());
                work.pop_front();
            }
            performCall(client, call);
        }
    }

    /// Anything the peer's service answers - even an error - says it is there.
    bool probe(ix::HttpClient& client) {
        auto args = std::make_shared<ix::HttpRequestArgs>();
        args->connectTimeout = 1;
        args->transferTimeout = 2;
        args->followRedirects = false;
        std::string url;
        {
            const std::scoped_lock lock{workMutex};
            url = config.peerServiceUrl;
        }
        const ix::HttpResponsePtr response = client.get(url, args);
        if (response != nullptr && response->statusCode > 0) {
            markUp();
            return true;
        }
        return false;
    }

    void performCall(ix::HttpClient& client, const OutboundCall& call) {
        auto args = std::make_shared<ix::HttpRequestArgs>();
        args->extraHeaders["Content-Type"] = "text/xml; charset=utf-8";
        args->extraHeaders["SOAPAction"] = "\"" + call.request.soapAction + "\"";
        if (config.auth.has_value()) {
            Result<std::string> header = imos::ws40::composeBasicAuthorization(*config.auth);
            if (header.ok()) {
                args->extraHeaders["Authorization"] = header.value();
            }
        }
        args->connectTimeout = config.requestTimeoutSecs;
        args->transferTimeout = config.requestTimeoutSecs;
        args->followRedirects = false;
        args->compress = config.acceptGzipResponses;
        args->compressRequest = false;

        std::string url;
        {
            const std::scoped_lock lock{workMutex};
            url = config.peerServiceUrl;
        }
        const ix::HttpResponsePtr response = client.post(url, call.request.envelope, args);
        if (response == nullptr || response->statusCode <= 0) {
            markDown();  // no answer at all: refused, reset or timed out
        } else {
            markUp();
        }
        if (response == nullptr || response->statusCode != 200) {
            const std::string detail =
                response == nullptr
                    ? "no response"
                    : "HTTP " + std::to_string(response->statusCode) +
                          (response->errorMsg.empty() ? "" : " (" + response->errorMsg + ")");
            postDiagnostic(call.channel, "<" + call.request.operation + "> call failed: " + detail +
                                             "; the session's retry timer re-sends");
            return;
        }
        Result<std::vector<codec::Message>> replies =
            parseSoapResponse(response->body, call.request.operation, config.mosID, config.ncsID,
                              call.request.messageId);
        if (!replies.ok()) {
            postDiagnostic(call.channel, "<" + call.request.operation +
                                             "> response rejected: " + replies.error().message);
            return;
        }
        for (const codec::Message& reply : replies.value()) {
            postEvent({QueuedEvent::Kind::Frame, call.channel, codec::serializeMessage(reply), {}});
        }
    }
};

Ws3Endpoint::Ws3Endpoint(EndpointConfig config, ILogger* logger) : impl_(std::make_unique<Impl>()) {
    ix::initNetSystem();
    impl_->config = std::move(config);
    impl_->logger = logger;
}

Ws3Endpoint::~Ws3Endpoint() {
    stop();
}

Result<void> Ws3Endpoint::start() {
    if (impl_->started) {
        return makeError(ErrorCode::WrongState, "endpoint already started");
    }
    // config.bindPort == 0 → probe an ephemeral port and retry the bind
    // (same probe race as the ws40 server documents).
    const int requested = impl_->boundPort != 0 ? impl_->boundPort : impl_->config.bindPort;
    const bool ephemeral = requested == 0;
    const int attempts = ephemeral ? kEphemeralBindAttempts : 1;
    std::string lastError;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const int port = ephemeral ? ix::getFreePort() : requested;
        auto server = std::make_unique<ix::HttpServer>(port, impl_->config.bindHost);
        server->setOnConnectionCallback(
            [impl = impl_.get()](ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState>) {
                return impl->handleRequest(request);
            });
        const std::pair<bool, std::string> listening = server->listen();
        if (!listening.first) {
            lastError = listening.second;
            continue;
        }
        server->start();
        impl_->server = std::move(server);
        impl_->boundPort = port;
        break;
    }
    if (impl_->server == nullptr) {
        return makeError(ErrorCode::IoError, "ws3 service failed to bind: " + lastError);
    }
    impl_->workerQuit = false;
    impl_->worker = std::thread{[impl = impl_.get()] { impl->workerLoop(); }};
    impl_->started = true;
    impl_->connectedAnnounced = false;
    return {};
}

void Ws3Endpoint::stop() {
    if (!impl_->started) {
        return;
    }
    {
        const std::scoped_lock lock{impl_->workMutex};
        impl_->workerQuit = true;
    }
    impl_->workCv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    // Release any parked exchange with a fault so server threads unblock
    // before the server is torn down.
    {
        const std::scoped_lock lock{impl_->parkedMutex};
        for (auto& [key, exchange] : impl_->parked) {
            exchange->response.set_value(
                buildSoapFault("Server", "endpoint stopping before a reply was produced"));
        }
        impl_->parked.clear();
    }
    if (impl_->server != nullptr) {
        impl_->server->stop();
        impl_->server.reset();
    }
    impl_->started = false;
}

namespace {

Result<codec::Message> parseClassic(std::span<const std::uint8_t> frame) {
    const std::string utf8{reinterpret_cast<const char*>(frame.data()), frame.size()};
    Result<codec::Message> parsed = codec::parseMessage(utf8);
    if (!parsed.ok()) {
        return makeError(ErrorCode::InvalidArgument,
                         "ws3 send expects a classic UTF-8 <mos> document (session must run "
                         "WireEncoding::Utf8): " +
                             parsed.error().message);
    }
    return parsed;
}

}  // namespace

Result<void> Ws3Endpoint::sendReply(Channel channel, std::span<const std::uint8_t> frame) {
    if (!impl_->started) {
        return makeError(ErrorCode::IoError, "ws3 endpoint is not started");
    }
    Result<codec::Message> parsed = parseClassic(frame);
    if (!parsed.ok()) {
        return std::move(parsed).error();
    }
    const codec::Message& message = parsed.value();

    // The answer to a parked inbound exchange, correlated by messageID (the
    // session echoes the request's ID onto acks and terminal replies).
    const std::string messageId{message.messageID().value_or("")};
    std::shared_ptr<ParkedExchange> exchange;
    {
        const std::scoped_lock lock{impl_->parkedMutex};
        const auto it = impl_->parked.find(messageId);
        if (it != impl_->parked.end()) {
            exchange = it->second;
            impl_->parked.erase(it);
        }
    }
    if (exchange == nullptr) {
        impl_->postDiagnostic(
            channel, "reply to message " + messageId + " dropped: no call is waiting for it");
        return {};
    }
    Result<std::string> envelope = buildSoapResponse(message, exchange->operation);
    if (!envelope.ok()) {
        exchange->response.set_value(buildSoapFault("Server", envelope.error().message));
        return makeError(ErrorCode::ProtocolViolation, envelope.error().message);
    }
    exchange->response.set_value(std::move(envelope).value());
    return {};
}

Result<void> Ws3Endpoint::send(Channel channel, std::span<const std::uint8_t> frame) {
    if (!impl_->started) {
        return makeError(ErrorCode::IoError, "ws3 endpoint is not started");
    }
    Result<codec::Message> parsed = parseClassic(frame);
    if (!parsed.ok()) {
        return std::move(parsed).error();
    }
    const codec::Message& message = parsed.value();

    // Otherwise it opens an exchange: wrap and queue the HTTP call.
    Result<SoapRequest> request = buildSoapRequest(message);
    if (!request.ok()) {
        return std::move(request).error();
    }
    {
        const std::scoped_lock lock{impl_->workMutex};
        impl_->work.push_back({channel, std::move(request).value()});
    }
    impl_->workCv.notify_one();
    return {};
}

bool Ws3Endpoint::isConnected(Channel) const {
    return impl_->started && impl_->peerUp;
}

void Ws3Endpoint::pump(PeerSession& session) {
    ChannelEventSink sink;
    sink.connected = [&session](Channel channel) { session.onChannelConnected(channel); };
    sink.disconnected = [&session](Channel channel) { session.onChannelDisconnected(channel); };
    sink.frame = [&session](Channel channel, std::span<const std::uint8_t> frame) {
        session.onFrameReceived(channel, frame);
    };
    pump(sink);
}

void Ws3Endpoint::pump(const ChannelEventSink& sink) {
    if (impl_->started && !impl_->connectedAnnounced) {
        // HTTP is connectionless: the pipe is "up" as soon as the service
        // and worker are, until a call finds the peer unreachable.
        impl_->connectedAnnounced = true;
        for (std::size_t i = 0; i < kChannelCount; ++i) {
            if (sink.connected) {
                sink.connected(static_cast<Channel>(i));
            }
        }
    }
    std::deque<QueuedEvent> local;
    {
        const std::scoped_lock lock{impl_->eventMutex};
        local.swap(impl_->events);
    }
    for (QueuedEvent& event : local) {
        switch (event.kind) {
            case QueuedEvent::Kind::Frame:
                if (sink.frame) {
                    sink.frame(event.channel,
                               std::span<const std::uint8_t>{
                                   reinterpret_cast<const std::uint8_t*>(event.frame.data()),
                                   event.frame.size()});
                }
                break;
            case QueuedEvent::Kind::Diagnostic:
                impl_->log(LogLevel::Warning, event.diagnostic);
                if (impl_->errorHandler) {
                    impl_->errorHandler(event.channel, event.diagnostic);
                }
                break;
            case QueuedEvent::Kind::Up:
            case QueuedEvent::Kind::Down:
                for (std::size_t i = 0; i < kChannelCount; ++i) {
                    const auto& notify =
                        event.kind == QueuedEvent::Kind::Up ? sink.connected : sink.disconnected;
                    if (notify) {
                        notify(static_cast<Channel>(i));
                    }
                }
                break;
        }
    }
}

void Ws3Endpoint::setErrorHandler(Ws3ErrorHandler handler) {
    impl_->errorHandler = std::move(handler);
}

void Ws3Endpoint::setActivityHandler(ActivityHandler handler) {
    impl_->activity = std::move(handler);
}

void Ws3Endpoint::setPeerServiceUrl(std::string url) {
    const std::scoped_lock lock{impl_->workMutex};
    impl_->config.peerServiceUrl = std::move(url);
}

int Ws3Endpoint::port() const {
    return impl_->boundPort;
}

}  // namespace imos::transports::ws3
