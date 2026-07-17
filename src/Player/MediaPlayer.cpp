/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <algorithm>
#include "MediaPlayer.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MediaPlayer::MediaPlayer(const EventPoller::Ptr &poller) {
    _poller = poller ? poller : EventPollerPool::Instance().getPoller();
}

void MediaPlayer::play(const string &url) {
    _delegate = PlayerBase::createPlayer(_poller, url, (*this)[Client::kSchema]);
    assert(_delegate);
    _delegate->setOnShutdown(_on_shutdown);
    _delegate->setOnPlayResult(_on_play_result);
    _delegate->setOnResume(_on_resume);
    _delegate->setMediaSource(_media_src);
    for (auto &pr : *this) {
        (*_delegate)[pr.first] = pr.second;
    }
    _delegate->setSocketCreator(_on_create_socket);
    _delegate->play(url);
}

EventPoller::Ptr MediaPlayer::getPoller(){
    return _poller;
}

void MediaPlayer::setOnCreateSocket(Socket::onCreateSocket cb){
    setSocketCreator(std::move(cb));
}

} /* namespace mediakit */
