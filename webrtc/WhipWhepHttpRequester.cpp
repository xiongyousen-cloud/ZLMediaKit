/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of this source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of this source tree.
 */

#include "WhipWhepHttpRequester.h"

#include "Common/Parser.h"
#include "Http/HttpRequester.h"
#include "WhipWhepProtocol.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace std;

namespace mediakit {

namespace {

bool isHttpFieldNameChar(unsigned char ch) {
    if (isalnum(ch)) {
        return true;
    }
    switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~': return true;
        default: return false;
    }
}

bool isTransportManagedHeader(const string &name) {
    static const char *kManagedHeaders[] = {
        "Host",
        "Content-Length",
        "Transfer-Encoding",
        "Connection",
        "Proxy-Connection",
        "Keep-Alive",
        "TE",
        "Trailer",
        "Upgrade",
        "Expect",
    };
    for (const auto header : kManagedHeaders) {
        if (strcasecmp(name.c_str(), header) == 0) {
            return true;
        }
    }
    return false;
}

void validateCustomHeader(const string &name, const string &value) {
    if (name.empty() || !all_of(name.begin(), name.end(), [](unsigned char ch) { return isHttpFieldNameChar(ch); })) {
        throw invalid_argument("invalid WHIP/WHEP custom header configuration");
    }
    if (isTransportManagedHeader(name)) {
        throw invalid_argument("invalid WHIP/WHEP custom header configuration");
    }
    if (any_of(value.begin(), value.end(), [](unsigned char ch) {
            return (ch < 0x20 && ch != '\t') || ch == 0x7F;
        })) {
        throw invalid_argument("invalid WHIP/WHEP custom header configuration");
    }
}

struct RequestLifetime {
    WhipWhepHttpRequester::Ptr owner;
    function<void(const toolkit::SockException &, const Parser &)> on_result;
};

struct RequestTimeout {
    size_t milliseconds;
    float seconds;
};

RequestTimeout normalizeRequestTimeout(float timeout_sec) {
    if (!isfinite(timeout_sec)) {
        throw invalid_argument("invalid WHIP/WHEP request timeout");
    }

    const long double requested_ms = timeout_sec > 0
        ? ceil(static_cast<long double>(timeout_sec) * 1000.0L)
        : 1.0L;
    if (requested_ms > static_cast<long double>(numeric_limits<int32_t>::max())) {
        throw invalid_argument("invalid WHIP/WHEP request timeout");
    }

    auto seconds = static_cast<float>(static_cast<size_t>(requested_ms)) / 1000.0f;
    auto milliseconds = static_cast<size_t>(seconds * 1000.0f);
    if (milliseconds == 0) {
        seconds = 0.001f;
        milliseconds = 1;
    }
    return { milliseconds, seconds };
}

} // namespace

class WhipWhepHttpRequester::Impl final : public HttpRequester {
public:
    explicit Impl(const toolkit::EventPoller::Ptr &poller) {
        if (poller) {
            setPoller(poller);
        }
    }

    void setCustomHeaders(const string &encoded_headers) {
        if (_custom_headers_configured) {
            throw invalid_argument("WHIP/WHEP custom headers already configured");
        }
        _custom_headers_configured = true;
        if (encoded_headers.find_first_of("\r\n") != string::npos) {
            throw invalid_argument("invalid WHIP/WHEP custom header configuration");
        }

        const HttpHeader headers = Parser::parseArgs(encoded_headers);
        for (const auto &header : headers) {
            validateCustomHeader(header.first, header.second);
        }
        for (const auto &header : headers) {
            addHeader(header.first, header.second, true);
        }
        _has_custom_headers = !headers.empty();
    }

    void setApplicationSdpContentType() {
        addHeader("Content-Type", "application/sdp", true);
    }

    void setApplicationSdpAccept() {
        addHeader("Accept", "application/sdp", true);
    }

    void setTrustedOrigins(set<string> trusted_origins) {
        _trusted_origins = std::move(trusted_origins);
    }

    bool hasCredentials() const {
        return _has_custom_headers || WhipWhepProtocol::hasUrlUserInfo(getUrl());
    }

    const string &policyError() const {
        return _policy_error;
    }

    void keepOwnerUntilNextPollerTurn(WhipWhepHttpRequester::Ptr owner) {
        // false 强制排队，避免在当前 completion 栈内同步释放最后一个 wrapper owner。
        getPoller()->async([owner]() {}, false);
    }

    void abortRequestStart() {
        // HttpRequester stores the completion trampoline before sendRequest(). Remove it before
        // shutdown can emit an error, then cancel any timer/socket created by a partial start.
        clear();
        shutdown(toolkit::SockException(toolkit::Err_shutdown, "WHIP/WHEP HTTP request start failed"));
    }

    bool isRedirectAllowed(const string &source_url, const string &target_url) {
        _policy_error.clear();
        const bool has_credentials = _has_custom_headers || WhipWhepProtocol::hasUrlUserInfo(source_url);
        return WhipWhepProtocol::isTargetAllowed(
            source_url, target_url, has_credentials, _trusted_origins, _policy_error);
    }

    void clear() override {
        HttpRequester::clear();
        _custom_headers_configured = false;
        _has_custom_headers = false;
        _policy_error.clear();
    }

private:
    bool onRedirectUrl(const string &url, bool temporary) override {
        (void)temporary;
        return isRedirectAllowed(getUrl(), url);
    }

private:
    bool _custom_headers_configured = false;
    bool _has_custom_headers = false;
    set<string> _trusted_origins;
    string _policy_error;
};

WhipWhepHttpRequester::WhipWhepHttpRequester(const toolkit::EventPoller::Ptr &poller)
    : _impl(make_shared<Impl>(poller)) {}

WhipWhepHttpRequester::~WhipWhepHttpRequester() = default;

void WhipWhepHttpRequester::setCustomHeaders(const string &encoded_headers) {
    _impl->setCustomHeaders(encoded_headers);
}

void WhipWhepHttpRequester::setApplicationSdpContentType() {
    _impl->setApplicationSdpContentType();
}

void WhipWhepHttpRequester::setApplicationSdpAccept() {
    _impl->setApplicationSdpAccept();
}

void WhipWhepHttpRequester::setTrustedOrigins(set<string> trusted_origins) {
    _impl->setTrustedOrigins(std::move(trusted_origins));
}

bool WhipWhepHttpRequester::hasCredentials() const {
    return _impl->hasCredentials();
}

const string &WhipWhepHttpRequester::policyError() const {
    return _impl->policyError();
}

void WhipWhepHttpRequester::clear() {
    // 先局部保活 Impl：HttpRequester::clear() 销毁 stored callback 并解除临时环时，即使最后一个 wrapper
    // owner 随之释放，也不会让 Impl 在自身 clear() 栈内析构；此路径不调用 wrapper::shared_from_this()。
    auto impl = _impl;
    impl->clear();
}

void WhipWhepHttpRequester::setMethod(string method) {
    _impl->setMethod(std::move(method));
}

void WhipWhepHttpRequester::setBody(string body) {
    _impl->setBody(std::move(body));
}

void WhipWhepHttpRequester::startRequester(
    const string &url,
    const function<void(const toolkit::SockException &, const Parser &)> &on_result,
    float timeout_sec) {
    const auto timeout = normalizeRequestTimeout(timeout_sec);
    _impl->setHeaderTimeout(timeout.milliseconds);
    auto lifetime = make_shared<RequestLifetime>();
    lifetime->owner = shared_from_this();
    lifetime->on_result = on_result;

    try {
        _impl->startRequester(
            url,
            [lifetime](const toolkit::SockException &ex, const Parser &response) {
                // 请求期引用图是 wrapper -> Impl -> _on_result -> RequestLifetime -> wrapper。
                // 先移空 RequestLifetime，再把 owner 强制排入 Impl poller 的下一轮，最后才调用业务 callback。
                // 因此正常返回时 base 可先执行 `_on_result = nullptr`，异常或 callback 内 clear() 时当前栈也仍有
                // queued owner/局部 Impl 保活；只有 completion 栈退出后的 poller 任务才释放最终 owner。
                auto owner = std::move(lifetime->owner);
                auto callback = std::move(lifetime->on_result);
                if (!owner) {
                    return;
                }
                auto impl = owner->_impl;
                impl->keepOwnerUntilNextPollerTurn(owner);
                if (callback) {
                    callback(ex, response);
                }
            },
            timeout.seconds);
    } catch (...) {
        // Keep wrapper and Impl alive while detaching the already-installed trampoline and
        // cancelling a partially started TcpClient. The empty-owner guard covers any stale event.
        auto owner = std::move(lifetime->owner);
        auto impl = _impl;
        lifetime->on_result = nullptr;
        impl->abortRequestStart();
        (void)owner;
        throw;
    }
}

const string &WhipWhepHttpRequester::getUrl() const {
    return _impl->getUrl();
}

void WhipWhepHttpRequester::setProxyUrl(string proxy_url) {
    _impl->setProxyUrl(std::move(proxy_url));
}

void WhipWhepHttpRequester::setNetAdapter(const string &local_ip) {
    _impl->setNetAdapter(local_ip);
}

void WhipWhepHttpRequester::setOnCreateSocket(toolkit::Socket::onCreateSocket cb) {
    _impl->setOnCreateSocket(std::move(cb));
}

bool WhipWhepHttpRequester::isRedirectAllowed(const string &source_url, const string &target_url) {
    return _impl->isRedirectAllowed(source_url, target_url);
}

} // namespace mediakit
