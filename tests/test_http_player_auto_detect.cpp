/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of this source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of this source tree.
 */

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Common/config.h"
#include "Http/HlsPlayer.h"
#include "Http/HttpSession.h"
#include "Http/TsPlayerImp.h"
#include "Network/TcpServer.h"
#include "Player/HttpPlayerAutoDetect.h"
#include "Player/MediaPlayer.h"
#include "Pusher/PusherBase.h"
#include "Rtmp/FlvPlayer.h"
#include "Rtmp/RtmpMediaSource.h"
#include "Rtsp/RtspMediaSource.h"
#include "Util/NoticeCenter.h"
#include "webrtc/WebRtcProxyPlayerImp.h"
#include "webrtc/WebRtcProxyPusher.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace {

void expect(bool condition, const string &message) {
    if (!condition) {
        throw runtime_error(message);
    }
}

void expectThrows(const function<void()> &fn, const string &message) {
    try {
        fn();
    } catch (const exception &) {
        return;
    }
    throw runtime_error(message);
}

class CallbackState {
public:
    void add(const SockException &ex) {
        lock_guard<mutex> lock(_mutex);
        ++_count;
        _last_error = ex.what();
        _condition.notify_all();
    }

    bool waitFor(size_t count, chrono::milliseconds timeout = chrono::milliseconds(3000)) {
        unique_lock<mutex> lock(_mutex);
        return _condition.wait_for(lock, timeout, [&]() { return _count >= count; });
    }

    size_t count() const {
        lock_guard<mutex> lock(_mutex);
        return _count;
    }

    string lastError() const {
        lock_guard<mutex> lock(_mutex);
        return _last_error;
    }

private:
    mutable mutex _mutex;
    condition_variable _condition;
    size_t _count = 0;
    string _last_error;
};

class TwoOriginHttpFixture {
public:
    struct Request {
        size_t origin = 0;
        string method;
        string path;
        StrCaseMap headers;
    };

    struct Response {
        int status = 404;
        StrCaseMap headers;
        string body;
        bool delayed = false;
    };

    TwoOriginHttpFixture() {
        _poller = EventPollerPool::Instance().getPoller();
        NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastHttpRequest, [this](BroadcastHttpRequestArgs) {
            onRequest(parser, invoker, consumed, sender);
        });
        for (size_t i = 0; i < 2; ++i) {
            auto poller = _poller;
            _servers[i] = TcpServer::Ptr(new TcpServer(poller), [poller](TcpServer *server) {
                poller->async([server]() { delete server; });
            });
            _servers[i]->start<HttpSession>(0, "127.0.0.1");
            _ports[i] = _servers[i]->getPort();
        }
    }

    ~TwoOriginHttpFixture() {
        NoticeCenter::Instance().delListener(this, Broadcast::kBroadcastHttpRequest);
        _servers[0].reset();
        _servers[1].reset();
        if (_poller && !_poller->isCurrentThread()) {
            _poller->sync([]() {});
        }
    }

    const EventPoller::Ptr &poller() const {
        return _poller;
    }

    string origin(size_t index) const {
        return string("http://127.0.0.1:") + to_string(_ports[index]);
    }

    string url(size_t index, const string &path) const {
        return origin(index) + path;
    }

    void plan(size_t origin_index,
              const string &method,
              const string &path,
              int status,
              const string &content_type = string(),
              const string &location = string(),
              bool delayed = false) {
        Response response;
        response.status = status;
        response.delayed = delayed;
        if (!content_type.empty()) {
            response.headers["Content-Type"] = content_type;
        }
        if (!location.empty()) {
            response.headers["Location"] = location;
        }
        lock_guard<mutex> lock(_mutex);
        _responses[makeKey(origin_index, method, path)] = std::move(response);
    }

    bool waitForRequests(size_t origin_index,
                         const string &method,
                         const string &path,
                         size_t count,
                         chrono::milliseconds timeout = chrono::milliseconds(3000)) {
        unique_lock<mutex> lock(_mutex);
        return _condition.wait_for(lock, timeout, [&]() {
            return countRequestsLocked(origin_index, method, path) >= count;
        });
    }

    size_t requestCount(size_t origin_index, const string &method, const string &path) const {
        lock_guard<mutex> lock(_mutex);
        return countRequestsLocked(origin_index, method, path);
    }

    string lastHeader(size_t origin_index,
                      const string &method,
                      const string &path,
                      const string &name) const {
        lock_guard<mutex> lock(_mutex);
        for (auto it = _requests.rbegin(); it != _requests.rend(); ++it) {
            if (it->origin == origin_index && it->method == method && it->path == path) {
                const auto header = it->headers.find(name);
                return header == it->headers.end() ? string() : header->second;
            }
        }
        return string();
    }

    void respondDelayed(size_t origin_index, const string &method, const string &path) {
        shared_ptr<HttpSession::HttpResponseInvoker> invoker;
        Response response;
        {
            lock_guard<mutex> lock(_mutex);
            const auto key = makeKey(origin_index, method, path);
            auto invoker_it = _delayed.find(key);
            expect(invoker_it != _delayed.end(), "missing delayed HTTP response invoker");
            invoker = std::move(invoker_it->second);
            _delayed.erase(invoker_it);
            response = _responses[key];
        }
        (*invoker)(response.status, response.headers, response.body);
    }

private:
    static string makeKey(size_t origin_index, const string &method, const string &path) {
        return to_string(origin_index) + "\n" + method + "\n" + path;
    }

    size_t countRequestsLocked(size_t origin_index, const string &method, const string &path) const {
        size_t result = 0;
        for (const auto &request : _requests) {
            if (request.origin == origin_index && request.method == method && request.path == path) {
                ++result;
            }
        }
        return result;
    }

    void onRequest(const Parser &parser,
                   const HttpSession::HttpResponseInvoker &invoker,
                   bool &consumed,
                   SockInfo &sender) {
        size_t origin_index = 2;
        for (size_t i = 0; i < 2; ++i) {
            if (sender.get_local_port() == _ports[i]) {
                origin_index = i;
                break;
            }
        }
        if (origin_index >= 2) {
            return;
        }

        Response response;
        const auto key = makeKey(origin_index, parser.method(), parser.url());
        {
            lock_guard<mutex> lock(_mutex);
            Request request;
            request.origin = origin_index;
            request.method = parser.method();
            request.path = parser.url();
            request.headers = parser.getHeader();
            _requests.emplace_back(std::move(request));
            auto response_it = _responses.find(key);
            if (response_it != _responses.end()) {
                response = response_it->second;
            }
            if (response.delayed) {
                _delayed[key] = make_shared<HttpSession::HttpResponseInvoker>(invoker);
            }
            consumed = true;
            _condition.notify_all();
        }
        if (!response.delayed) {
            invoker(response.status, response.headers, response.body);
        }
    }

private:
    EventPoller::Ptr _poller;
    TcpServer::Ptr _servers[2];
    uint16_t _ports[2] = { 0, 0 };
    mutable mutex _mutex;
    condition_variable _condition;
    vector<Request> _requests;
    map<string, Response> _responses;
    map<string, shared_ptr<HttpSession::HttpResponseInvoker> > _delayed;
};

void configureCallbacks(PlayerBase &player, CallbackState &state) {
    player.setOnPlayResult([&state](const SockException &ex) { state.add(ex); });
    player.setOnShutdown([](const SockException &) {});
    player.setOnResume([]() {});
}

class CallbackSetterSpy : public PlayerBase {
public:
    void setMediaSource(const MediaSource::Ptr &) override {}

    void setOnShutdown(const Event &) override {
        ++shutdown_set_count;
    }

    void setOnPlayResult(const Event &) override {
        ++play_result_set_count;
    }

    void setOnResume(const function<void()> &) override {
        ++resume_set_count;
    }

    size_t shutdown_set_count = 0;
    size_t play_result_set_count = 0;
    size_t resume_set_count = 0;

protected:
    void onResume() override {}
    void onShutdown(const SockException &) override {}
    void onPlayResult(const SockException &) override {}
};

class InspectableHttpPlayerAutoDetect : public HttpPlayerAutoDetect {
public:
    explicit InspectableHttpPlayerAutoDetect(const EventPoller::Ptr &poller)
        : HttpPlayerAutoDetect(poller) {}

    void installDelegate(const PlayerBase::Ptr &delegate) {
        _delegate = delegate;
    }

    void emitPlayResult(const SockException &ex) {
        onPlayResult(ex);
    }
};

void testAutoDetectKeepsGenerationGuardWhenCallbacksChange() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = make_shared<InspectableHttpPlayerAutoDetect>(poller);
    auto delegate = make_shared<CallbackSetterSpy>();
    player->installDelegate(delegate);

    size_t callback_count = 0;
    player->setOnShutdown([](const SockException &) {});
    player->setOnPlayResult([&callback_count](const SockException &) { ++callback_count; });
    player->setOnResume([]() {});

    expect(delegate->shutdown_set_count == 0 && delegate->play_result_set_count == 0
               && delegate->resume_set_count == 0,
           "callback updates must not replace the auto-detect delegate generation guards");
    player->emitPlayResult(SockException());
    expect(callback_count == 1,
           "callback updates must remain stored on the auto-detect wrapper");
}

void testRequesterTimeoutNormalization() {
    TwoOriginHttpFixture fixture;
    fixture.plan(0, "HEAD", "/tiny-timeout", 200, "application/sdp");
    auto requester = make_shared<WhipWhepHttpRequester>(fixture.poller());
    requester->setMethod("HEAD");

    try {
        requester->startRequester(
            fixture.url(0, "/tiny-timeout"),
            [](const SockException &, const Parser &) {},
            0.0001f);
    } catch (const exception &) {
        throw runtime_error("positive sub-millisecond requester timeout must clamp to one millisecond");
    }
    requester->clear();

    auto invalid_requester = make_shared<WhipWhepHttpRequester>(fixture.poller());
    expectThrows(
        [&]() {
            invalid_requester->startRequester(
                fixture.url(0, "/invalid-timeout"),
                [](const SockException &, const Parser &) {},
                numeric_limits<float>::infinity());
        },
        "non-finite requester timeout must be rejected before integer conversion");
}

void testKnownHttpPlayersBypassHeadProbe() {
    TwoOriginHttpFixture fixture;
    const auto &poller = fixture.poller();

    expect(dynamic_pointer_cast<HlsPlayerImp>(PlayerBase::createPlayer(poller, fixture.url(0, "/known.m3u8"))) != nullptr,
           "m3u8 URL must retain HLS dispatch");
    expect(dynamic_pointer_cast<TsPlayerImp>(PlayerBase::createPlayer(poller, fixture.url(0, "/known.ts"))) != nullptr,
           "ts URL must retain TS dispatch");
    expect(dynamic_pointer_cast<FlvPlayerImp>(PlayerBase::createPlayer(poller, fixture.url(0, "/known.flv"))) != nullptr,
           "flv URL must retain FLV dispatch");
    expect(dynamic_pointer_cast<HlsPlayerImp>(PlayerBase::createPlayer(poller, fixture.url(0, "/known"), "hls")) != nullptr,
           "explicit HLS schema must retain HLS dispatch");
    expect(dynamic_pointer_cast<TsPlayerImp>(PlayerBase::createPlayer(poller, fixture.url(0, "/known"), "ts")) != nullptr,
           "explicit TS schema must retain TS dispatch");
    expect(dynamic_pointer_cast<FlvPlayerImp>(PlayerBase::createPlayer(poller, fixture.url(0, "/known"), "flv")) != nullptr,
           "explicit FLV schema must retain FLV dispatch");
    expect(dynamic_pointer_cast<HlsPlayerImp>(PlayerBase::createPlayer(poller, fixture.url(0, "/priority.m3u8"), "whep")) != nullptr,
           "existing HLS extension dispatch must precede WHEP schema dispatch");
    expectThrows(
        [&]() { PlayerBase::createPlayer(poller, fixture.url(0, "/unsupported"), "unsupported"); },
        "explicit unsupported HTTP schema must retain the existing factory error");
    expect(fixture.requestCount(0, "HEAD", "/known") == 0,
           "known HTTP players must not send a WHEP HEAD probe");
}

void testExplicitWhepSchemaBypassesHeadProbe() {
    TwoOriginHttpFixture fixture;
    auto player = PlayerBase::createPlayer(fixture.poller(), fixture.url(0, "/explicit"), "whep");
    expect(dynamic_pointer_cast<WebRtcProxyPlayerImp>(player) != nullptr,
           "schema=whep must create a WebRTC proxy player directly");
    expect(fixture.requestCount(0, "HEAD", "/explicit") == 0,
           "schema=whep factory dispatch must not issue a HEAD probe");
}

void testUnknownHttpPlayerDiscoversWhepWithHead() {
    TwoOriginHttpFixture fixture;
    fixture.plan(0, "HEAD", "/discover", 200, "Application/SDP; charset=utf-8");
    fixture.plan(0, "POST", "/discover", 500, "text/plain");

    MediaPlayer player(fixture.poller());
    CallbackState callbacks;
    configureCallbacks(player, callbacks);
    player[Client::kCustomHeader] = "X-Task5=continuity";
    player[Client::kNetAdapter] = "127.0.0.1";
    player[Client::kTimeoutMS] = 3000;

    mutex socket_mutex;
    vector<const EventPoller *> socket_pollers;
    player.setOnCreateSocket([&](const EventPoller::Ptr &poller) {
        lock_guard<mutex> lock(socket_mutex);
        socket_pollers.emplace_back(poller.get());
        return Socket::createSocket(poller, true);
    });

    player.play(fixture.url(0, "/discover"));
    expect(dynamic_pointer_cast<HttpPlayerAutoDetect>(player.getDelegate()) != nullptr,
           "unknown HTTP URL must create HttpPlayerAutoDetect through MediaPlayer");
    expect(fixture.waitForRequests(0, "POST", "/discover", 1),
           "successful SDP HEAD discovery must start exactly one WHEP POST");
    expect(callbacks.waitFor(1), "WHEP POST rejection must finish the public player attempt");
    expect(fixture.requestCount(0, "HEAD", "/discover") == 1,
           "unknown HTTP URL must send exactly one HEAD probe");
    expect(fixture.requestCount(0, "POST", "/discover") == 1,
           "successful HEAD discovery must send exactly one SDP POST");
    expect(fixture.lastHeader(0, "HEAD", "/discover", "Accept") == "application/sdp",
           "HEAD discovery must advertise Accept: application/sdp");
    expect(fixture.lastHeader(0, "HEAD", "/discover", "X-Task5") == "continuity",
           "HEAD discovery must receive the configured custom header");
    expect(fixture.lastHeader(0, "POST", "/discover", "X-Task5") == "continuity",
           "discovered WHEP delegate must receive the same custom header");
    {
        lock_guard<mutex> lock(socket_mutex);
        expect(socket_pollers.size() >= 2,
               "HEAD and WHEP POST must both use the caller socket creator");
        for (const auto poller : socket_pollers) {
            expect(poller == fixture.poller().get(),
                   "HEAD and WHEP POST socket creation must stay on the factory poller");
        }
    }
}

void testHeadRejectsWrongStatusOrContentTypeWithoutPost() {
    TwoOriginHttpFixture fixture;
    struct Case {
        const char *path;
        int status;
        const char *content_type;
    };
    const Case cases[] = {
        { "/status-401", 401, "application/sdp" },
        { "/status-403", 403, "application/sdp" },
        { "/status-405", 405, "application/sdp" },
        { "/status-500", 500, "application/sdp" },
        { "/missing-content-type", 200, "" },
        { "/wrong-content-type", 200, "text/plain" },
    };

    for (const auto &test_case : cases) {
        fixture.plan(0, "HEAD", test_case.path, test_case.status, test_case.content_type);
        auto player = PlayerBase::createPlayer(fixture.poller(), fixture.url(0, test_case.path));
        CallbackState callbacks;
        configureCallbacks(*player, callbacks);
        player->play(fixture.url(0, test_case.path));
        expect(callbacks.waitFor(1), string("HEAD rejection must report one failure for ") + test_case.path);
        this_thread::sleep_for(chrono::milliseconds(20));
        expect(callbacks.count() == 1, string("HEAD rejection must report once for ") + test_case.path);
        expect(fixture.requestCount(0, "HEAD", test_case.path) == 1,
               string("HEAD rejection must issue exactly one probe for ") + test_case.path);
        expect(fixture.requestCount(0, "POST", test_case.path) == 0,
               string("rejected HEAD must not POST for ") + test_case.path);
        if (test_case.status == 405) {
            expect(callbacks.lastError().find("schema=whep") != string::npos,
                   "405 discovery error must explain the schema=whep escape hatch");
        }
    }
}

void testLateHeadResponseCannotReplaceNewGeneration() {
    TwoOriginHttpFixture fixture;
    fixture.plan(0, "HEAD", "/late-old", 200, "application/sdp", string(), true);
    fixture.plan(0, "HEAD", "/late-new", 200, "application/sdp");
    fixture.plan(0, "POST", "/late-new", 500, "text/plain");

    auto player = PlayerBase::createPlayer(fixture.poller(), fixture.url(0, "/late-old"));
    CallbackState callbacks;
    configureCallbacks(*player, callbacks);
    player->play(fixture.url(0, "/late-old"));
    expect(fixture.waitForRequests(0, "HEAD", "/late-old", 1), "first delayed HEAD must arrive");

    player->play(fixture.url(0, "/late-new"));
    expect(fixture.waitForRequests(0, "POST", "/late-new", 1),
           "second generation must discover and start WHEP");
    expect(callbacks.waitFor(1), "second generation WHEP result must be observed");

    fixture.respondDelayed(0, "HEAD", "/late-old");
    this_thread::sleep_for(chrono::milliseconds(100));
    expect(fixture.requestCount(0, "POST", "/late-old") == 0,
           "late first-generation HEAD must not create a stale WHEP delegate");
    expect(fixture.requestCount(0, "POST", "/late-new") == 1,
           "only the current generation may create a WHEP delegate");
    expect(callbacks.count() == 1,
           "late first-generation completion must not report another result");
}

void testCredentialedCrossOriginHeadRedirectIsBlocked() {
    TwoOriginHttpFixture fixture;
    fixture.plan(0, "HEAD", "/redirect-blocked", 307, string(), fixture.url(1, "/redirect-target"));
    fixture.plan(1, "HEAD", "/redirect-target", 405, "text/plain");

    auto player = PlayerBase::createPlayer(fixture.poller(), fixture.url(0, "/redirect-blocked"));
    CallbackState callbacks;
    configureCallbacks(*player, callbacks);
    (*player)[Client::kCustomHeader] = "Authorization=Bearer-task5-secret";
    player->play(fixture.url(0, "/redirect-blocked"));
    expect(callbacks.waitFor(1), "blocked credentialed redirect must report a failure");
    expect(fixture.requestCount(1, "HEAD", "/redirect-target") == 0,
           "untrusted cross-origin target must receive no credentialed HEAD request");
}

void testTrustedCrossOriginHeadRedirectIsFollowed() {
    TwoOriginHttpFixture fixture;
    fixture.plan(0, "HEAD", "/redirect-trusted", 307, string(), fixture.url(1, "/trusted-target"));
    fixture.plan(1, "HEAD", "/trusted-target", 200, "application/sdp");
    fixture.plan(1, "POST", "/trusted-target", 500, "text/plain");

    auto player = PlayerBase::createPlayer(fixture.poller(), fixture.url(0, "/redirect-trusted"));
    CallbackState callbacks;
    configureCallbacks(*player, callbacks);
    (*player)[Client::kCustomHeader] = "Authorization=Bearer-task5-secret";
    (*player)[Client::kWhipWhepTrustedOrigins] = fixture.origin(1);
    player->play(fixture.url(0, "/redirect-trusted"));

    expect(fixture.waitForRequests(1, "HEAD", "/trusted-target", 1),
           "explicitly trusted cross-origin HEAD redirect must be followed");
    expect(fixture.waitForRequests(1, "POST", "/trusted-target", 1),
           "successful trusted redirect discovery must POST to the final target URL");
    expect(callbacks.waitFor(1), "trusted target POST rejection must finish discovery");
    expect(fixture.lastHeader(1, "HEAD", "/trusted-target", "Authorization") == "Bearer-task5-secret",
           "trusted redirected HEAD must preserve the configured credential value");
    expect(fixture.lastHeader(1, "POST", "/trusted-target", "Authorization") == "Bearer-task5-secret",
           "discovered WHEP POST must preserve credentials only for the trusted final origin");
    expect(fixture.requestCount(0, "POST", "/redirect-trusted") == 0,
           "redirect discovery must not POST back to the pre-redirect endpoint");
}

void testCredentialAddedDuringHeadDoesNotEscapeGeneration() {
    TwoOriginHttpFixture fixture;
    fixture.plan(0, "HEAD", "/late-credential", 307, string(), fixture.url(1, "/late-credential-target"));
    fixture.plan(1, "HEAD", "/late-credential-target", 200, "application/sdp", string(), true);
    fixture.plan(1, "POST", "/late-credential-target", 500, "text/plain");

    auto player = PlayerBase::createPlayer(fixture.poller(), fixture.url(0, "/late-credential"));
    CallbackState callbacks;
    configureCallbacks(*player, callbacks);
    player->play(fixture.url(0, "/late-credential"));
    expect(fixture.waitForRequests(1, "HEAD", "/late-credential-target", 1),
           "credential-free cross-origin HEAD redirect must reach its target");

    fixture.poller()->sync([player]() {
        (*player)[Client::kCustomHeader] = "Authorization=Bearer-added-after-head";
    });
    fixture.respondDelayed(1, "HEAD", "/late-credential-target");

    expect(fixture.waitForRequests(1, "POST", "/late-credential-target", 1),
           "successful redirected discovery must start WHEP at the final URL");
    expect(callbacks.waitFor(1), "WHEP rejection must finish the snapshotted generation");
    expect(fixture.lastHeader(1, "POST", "/late-credential-target", "Authorization").empty(),
           "credentials added after HEAD starts must not escape into that generation's cross-origin POST");
}

void testHttpPusherFactoryCreatesWhipDelegate() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto source = make_shared<RtspMediaSource>(MediaTuple("__defaultVhost__", "live", "task5-rtsp"));
    auto pusher = PusherBase::createPusher(poller, source, "https://whip.example.test/ingest");
    expect(dynamic_pointer_cast<WebRtcProxyPusherImp>(pusher) != nullptr,
           "HTTP(S) pusher factory must create a WHIP WebRTC delegate for RTSP sources");
}

void testHttpPusherRejectsNonRtspMediaSource() {
    auto poller = EventPollerPool::Instance().getPoller();
    auto source = make_shared<RtmpMediaSource>(MediaTuple("__defaultVhost__", "live", "task5-rtmp"));
    expectThrows(
        [&]() { PusherBase::createPusher(poller, source, "https://whip.example.test/ingest"); },
        "HTTP(S) WHIP pusher must reject non-RTSP media sources");
}

} // namespace

int main() {
    try {
        testKnownHttpPlayersBypassHeadProbe();
        testExplicitWhepSchemaBypassesHeadProbe();
        testAutoDetectKeepsGenerationGuardWhenCallbacksChange();
        testRequesterTimeoutNormalization();
        testUnknownHttpPlayerDiscoversWhepWithHead();
        testHeadRejectsWrongStatusOrContentTypeWithoutPost();
        testLateHeadResponseCannotReplaceNewGeneration();
        testCredentialedCrossOriginHeadRedirectIsBlocked();
        testTrustedCrossOriginHeadRedirectIsFollowed();
        testCredentialAddedDuringHeadDoesNotEscapeGeneration();
        testHttpPusherFactoryCreatesWhipDelegate();
        testHttpPusherRejectsNonRtspMediaSource();
        cout << "test_http_player_auto_detect passed" << endl;
        return 0;
    } catch (const exception &ex) {
        cerr << "test_http_player_auto_detect failed: " << ex.what() << endl;
        return EXIT_FAILURE;
    }
}
