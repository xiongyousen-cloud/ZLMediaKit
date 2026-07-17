/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of this source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of this source tree.
 */

#ifndef ZLMEDIAKIT_WHIP_WHEP_HTTP_REQUESTER_H
#define ZLMEDIAKIT_WHIP_WHEP_HTTP_REQUESTER_H

#include "Network/Socket.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

namespace mediakit {

class Parser;

class WhipWhepHttpRequester : public std::enable_shared_from_this<WhipWhepHttpRequester> {
public:
    using Ptr = std::shared_ptr<WhipWhepHttpRequester>;

    explicit WhipWhepHttpRequester(const toolkit::EventPoller::Ptr &poller = nullptr);
    ~WhipWhepHttpRequester();

    void setCustomHeaders(const std::string &encoded_headers);
    void setApplicationSdpContentType();
    void setApplicationSdpAccept();
    void setTrustedOrigins(std::set<std::string> trusted_origins);
    bool hasCredentials() const;
    const std::string &policyError() const;
    void clear();

    void setMethod(std::string method);
    void setBody(std::string body);
    void startRequester(
        const std::string &url,
        const std::function<void(const toolkit::SockException &ex, const Parser &response)> &on_result,
        float timeout_sec = 10);
    const std::string &getUrl() const;
    void setProxyUrl(std::string proxy_url);
    void setNetAdapter(const std::string &local_ip);
    void setOnCreateSocket(toolkit::Socket::onCreateSocket cb);

protected:
    bool isRedirectAllowed(const std::string &source_url,
                           const std::string &target_url);

private:
    class Impl;

    WhipWhepHttpRequester(const WhipWhepHttpRequester &) = delete;
    WhipWhepHttpRequester &operator=(const WhipWhepHttpRequester &) = delete;

    std::shared_ptr<Impl> _impl;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_WHIP_WHEP_HTTP_REQUESTER_H
