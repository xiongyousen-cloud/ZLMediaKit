/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of this source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of this source tree.
 */

#include "HttpPlayerAutoDetect.h"

#ifdef ENABLE_WEBRTC

#include "Common/Parser.h"
#include "Common/config.h"
#include "../../webrtc/WhipWhepProtocol.h"

#include <cstdlib>
#include <set>
#include <utility>

using namespace std;
using namespace toolkit;

namespace mediakit {

namespace {

bool isSuccessfulStatus(const string &status) {
    if (status.size() != 3) {
        return false;
    }
    char *end = nullptr;
    const auto code = strtol(status.c_str(), &end, 10);
    return end == status.c_str() + status.size() && code >= 200 && code < 300;
}

float requestTimeoutSeconds(mINI &config) {
    auto timeout_ms = config[Client::kTimeoutMS].as<int>();
    if (timeout_ms <= 0) {
        timeout_ms = 1;
    }
    return static_cast<float>(timeout_ms) / 1000.0f;
}

} // namespace

HttpPlayerAutoDetect::HttpPlayerAutoDetect(const EventPoller::Ptr &poller)
    : _poller(poller ? poller : EventPollerPool::Instance().getPoller()) {}

void HttpPlayerAutoDetect::play(const string &url) {
    ++_generation;
    resetCurrent();
    const auto generation = _generation;

    try {
        auto attempt = make_shared<AttemptConfig>();
        attempt->options = static_cast<const mINI &>(*this);
        attempt->media_source = _media_src;
        attempt->socket_creator = _on_create_socket;

        auto requester = make_shared<WhipWhepHttpRequester>(_poller);
        requester->setCustomHeaders(attempt->options[Client::kCustomHeader]);
        auto trusted_origins = WhipWhepProtocol::parseTrustedOrigins(
            attempt->options[Client::kWhipWhepTrustedOrigins]);
        trusted_origins.emplace(WhipWhepProtocol::canonicalOrigin(url));
        requester->setTrustedOrigins(std::move(trusted_origins));
        if (attempt->socket_creator) {
            requester->setOnCreateSocket(attempt->socket_creator);
        }
        requester->setProxyUrl(attempt->options[Client::kProxyUrl]);
        const auto net_adapter = attempt->options[Client::kNetAdapter];
        if (!net_adapter.empty()) {
            requester->setNetAdapter(net_adapter);
        }
        requester->setMethod("HEAD");
        requester->setApplicationSdpAccept();

        weak_ptr<HttpPlayerAutoDetect> weak_self = shared_from_this();
        _probe = requester;
        requester->startRequester(
            url,
            [weak_self, generation, requester, attempt](const SockException &ex, const Parser &response) {
                auto strong_self = weak_self.lock();
                if (!strong_self || strong_self->_generation != generation) {
                    return;
                }
                if (!requester->policyError().empty()) {
                    strong_self->fail(generation, SockException(Err_other, requester->policyError()));
                    return;
                }
                if (ex) {
                    strong_self->fail(
                        generation,
                        SockException(ex.getErrCode(), "WHEP auto-detect HEAD request failed"));
                    return;
                }
                if (!isSuccessfulStatus(response.status())) {
                    if (response.status() == "405") {
                        strong_self->fail(
                            generation,
                            SockException(Err_other, "WHEP auto-detect HEAD is not allowed; set schema=whep to skip discovery"));
                    } else {
                        strong_self->fail(
                            generation,
                            SockException(Err_other, "WHEP auto-detect HEAD returned a non-success status"));
                    }
                    return;
                }
                if (!WhipWhepProtocol::isSdpContentType(response["Content-Type"])) {
                    strong_self->fail(
                        generation,
                        SockException(Err_other, "WHEP auto-detect HEAD did not return application/sdp"));
                    return;
                }
                strong_self->startWhep(generation, requester->getUrl(), attempt);
            },
            requestTimeoutSeconds(attempt->options));
    } catch (const exception &) {
        fail(generation, SockException(Err_other, "failed to start discovered WHEP player"));
    }
}

void HttpPlayerAutoDetect::teardown() {
    ++_generation;
    resetCurrent();
}

void HttpPlayerAutoDetect::setOnShutdown(const Event &cb) {
    _on_shutdown = cb;
}

void HttpPlayerAutoDetect::setOnPlayResult(const Event &cb) {
    _on_play_result = cb;
}

void HttpPlayerAutoDetect::setOnResume(const function<void()> &cb) {
    _on_resume = cb;
}

void HttpPlayerAutoDetect::fail(uint64_t generation, const SockException &ex) {
    if (generation != _generation || _failed_generation == generation) {
        return;
    }
    _failed_generation = generation;
    if (_probe) {
        auto probe = std::move(_probe);
        probe->clear();
    }
    onPlayResult(ex);
}

void HttpPlayerAutoDetect::startWhep(uint64_t generation,
                                     string url,
                                     const shared_ptr<AttemptConfig> &attempt) {
    if (generation != _generation) {
        return;
    }
    if (_probe) {
        auto probe = std::move(_probe);
        probe->clear();
    }

    try {
        auto delegate = PlayerBase::createPlayer(_poller, url, "whep");
        configureDelegate(delegate, generation, *attempt);
        _delegate = delegate;
        delegate->play(url);
    } catch (const exception &) {
        if (_delegate) {
            auto delegate = std::move(_delegate);
            delegate->teardown();
        }
        fail(generation, SockException(Err_other, "failed to start discovered WHEP player"));
    }
}

void HttpPlayerAutoDetect::configureDelegate(const PlayerBase::Ptr &delegate,
                                             uint64_t generation,
                                             const AttemptConfig &attempt) {
    if (!delegate) {
        throw invalid_argument("invalid discovered WHEP player");
    }

    delegate->mINI::operator=(attempt.options);
    delegate->setMediaSource(attempt.media_source);
    delegate->setSocketCreator(attempt.socket_creator);

    weak_ptr<HttpPlayerAutoDetect> weak_self = shared_from_this();
    delegate->setOnPlayResult([weak_self, generation](const SockException &ex) {
        auto strong_self = weak_self.lock();
        if (strong_self && strong_self->_generation == generation) {
            strong_self->onPlayResult(ex);
        }
    });
    delegate->setOnShutdown([weak_self, generation](const SockException &ex) {
        auto strong_self = weak_self.lock();
        if (strong_self && strong_self->_generation == generation) {
            strong_self->onShutdown(ex);
        }
    });
    delegate->setOnResume([weak_self, generation]() {
        auto strong_self = weak_self.lock();
        if (strong_self && strong_self->_generation == generation) {
            strong_self->onResume();
        }
    });
}

void HttpPlayerAutoDetect::resetCurrent() {
    if (_probe) {
        auto probe = std::move(_probe);
        probe->clear();
    }
    if (_delegate) {
        auto delegate = std::move(_delegate);
        delegate->teardown();
    }
}

} // namespace mediakit

#endif // ENABLE_WEBRTC
