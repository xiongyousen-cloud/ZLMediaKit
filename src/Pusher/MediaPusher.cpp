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
#include "MediaPusher.h"
#include "PusherBase.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MediaPusher::MediaPusher(const MediaSource::Ptr &src,
                         const EventPoller::Ptr &poller) {
    _src = src;
    _poller = poller ? poller : EventPollerPool::Instance().getPoller();
}

MediaPusher::MediaPusher(const string &schema,
                         const string &vhost,
                         const string &app,
                         const string &stream,
                         const EventPoller::Ptr &poller) :
        MediaPusher(MediaSource::find(schema, vhost, app, stream), poller){
}

void MediaPusher::publish(const string &url) {
    _delegate = PusherBase::createPusher(_poller, _src.lock(), url);
    assert(_delegate);
    _delegate->setOnShutdown(_on_shutdown);
    _delegate->setOnPublished(_on_publish);
    _delegate->mINI::operator=(*this);
    _delegate->setSocketCreator(_on_create_socket);
    _delegate->publish(url);
    _url = url;
}

EventPoller::Ptr MediaPusher::getPoller(){
    return _poller;
}

void MediaPusher::setOnCreateSocket(Socket::onCreateSocket cb){
    setSocketCreator(std::move(cb));
}

} /* namespace mediakit */
