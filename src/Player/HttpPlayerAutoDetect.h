/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of this source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of this source tree.
 */

#ifndef ZLMEDIAKIT_HTTP_PLAYER_AUTO_DETECT_H
#define ZLMEDIAKIT_HTTP_PLAYER_AUTO_DETECT_H

#ifdef ENABLE_WEBRTC

#include "PlayerBase.h"
#include "../../webrtc/WhipWhepHttpRequester.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mediakit {

class HttpPlayerAutoDetect
    : public PlayerImp<PlayerBase, PlayerBase>
    , public std::enable_shared_from_this<HttpPlayerAutoDetect> {
public:
    explicit HttpPlayerAutoDetect(const toolkit::EventPoller::Ptr &poller);

    void play(const std::string &url) override;
    void teardown() override;
    void setOnShutdown(const Event &cb) override;
    void setOnPlayResult(const Event &cb) override;
    void setOnResume(const std::function<void()> &cb) override;

private:
    struct AttemptConfig {
        toolkit::mINI options;
        MediaSource::Ptr media_source;
        toolkit::Socket::onCreateSocket socket_creator;
    };

    void fail(uint64_t generation, const toolkit::SockException &ex);
    void startWhep(uint64_t generation,
                   std::string url,
                   const std::shared_ptr<AttemptConfig> &attempt);
    void configureDelegate(const PlayerBase::Ptr &delegate,
                           uint64_t generation,
                           const AttemptConfig &attempt);
    void resetCurrent();

private:
    toolkit::EventPoller::Ptr _poller;
    WhipWhepHttpRequester::Ptr _probe;
    uint64_t _generation = 0;
    uint64_t _failed_generation = 0;
};

} // namespace mediakit

#endif // ENABLE_WEBRTC
#endif // ZLMEDIAKIT_HTTP_PLAYER_AUTO_DETECT_H
