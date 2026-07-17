/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_WEBRTC_CLIENT_H
#define ZLMEDIAKIT_WEBRTC_CLIENT_H

#include "Network/Socket.h"
#include "Sdp.h"
#include "WebRtcTransport.h"
#include "WebRtcSignalingPeer.h"
#include "WhipWhepHttpRequester.h"
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace mediakit {

// 解析webrtc 信令url的工具类
class WebRTCUrl {
public:
    bool _is_ssl = false;
    std::string _full_url;
    std::string _negotiate_url; // WHEP/WHIP 协商端点
    // Location 返回的 WHEP/WHIP 绝对会话资源 URL。
    std::string _delete_url;
    std::string _target_secret;
    std::string _params;
    std::string _host;
    uint16_t _port = 0;
    std::string _vhost;
    std::string _app;
    std::string _stream;
    WebRtcTransport::SignalingProtocols _signaling_protocols = WebRtcTransport::SignalingProtocols::WHEP_WHIP;
    std::string _peer_room_id; // peer room_id

public:
    void parse(const std::string &url, bool isPlayer);

private:
};

// 实现了webrtc代理功能
class WebRtcClient : public std::enable_shared_from_this<WebRtcClient> {
public:
    using Ptr = std::shared_ptr<WebRtcClient>;

    WebRtcClient(toolkit::EventPoller::Ptr poller);
    virtual ~WebRtcClient();

    const toolkit::EventPoller::Ptr &getPoller() const { return _poller; }
    void setPoller(toolkit::EventPoller::Ptr poller) { _poller = std::move(poller); }

    // 获取WebRTC transport，用于API查询
    const WebRtcTransport::Ptr &getWebRtcTransport() const { return _transport; }

protected:
    virtual bool isPlayer() = 0;
    virtual void startConnect();
    virtual void onResult(const toolkit::SockException &ex) = 0;
    virtual void onNegotiateFinish();
    virtual float getTimeOutSec();
    virtual std::string getWhipWhepCustomHeader() { return ""; }
    virtual std::string getWhipWhepTrustedOrigins() { return ""; }
    virtual std::string getWhipWhepProxyUrl() { return ""; }
    virtual std::string getWhipWhepNetAdapter() { return ""; }
    void setWhipWhepSocketCreator(toolkit::Socket::onCreateSocket cb) {
        _whip_whep_on_create_socket = std::move(cb);
    }
    void prepareWhipWhepSecurityState(const std::string &endpoint_url);
    virtual WhipWhepHttpRequester::Ptr createWhipWhepRequester();
    WhipWhepHttpRequester::Ptr makeWhipWhepRequester();
    std::string validateWhipWhepSessionUrl(const std::string &request_url,
                                           const std::string &location) const;
    void assignWhipWhepSessionUrl(const std::string &request_url,
                                  const std::string &location);
    static toolkit::SockException getWhipWhepRequestError(
        const toolkit::SockException &network_error,
        const WhipWhepHttpRequester::Ptr &requester);

    void doNegotiate();
    void doNegotiateWebsocket();
    void doNegotiateWhepOrWhip();
    void doAnswerWhepCounterOffer(const std::string &server_offer);
    void checkIn();
    void doBye();
    void doByeWhepOrWhip();
    void checkOut();

    void gatheringCandidate(IceServerInfo::Ptr ice_server);
    void connectivityCheck();
    void candidate(const std::string &candidate, const std::string &ufrag, const std::string &pwd);

protected:
    toolkit::EventPoller::Ptr _poller;

    // for _negotiate_sdp
    WebRTCUrl _url;
    WhipWhepHttpRequester::Ptr _negotiate = nullptr;
    WhipWhepHttpRequester::Ptr _counter_offer = nullptr;
    WhipWhepHttpRequester::Ptr _delete_requester = nullptr;
    WebRtcSignalingPeer::Ptr _peer = nullptr;
    WebRtcTransport::Ptr _transport = nullptr;
    std::string _whip_whep_custom_header;
    std::set<std::string> _whip_whep_trusted_origins;
    toolkit::Socket::onCreateSocket _whip_whep_on_create_socket;
    bool _is_negotiate_finished = false;

};

} /*namespace mediakit */
#endif /* ZLMEDIAKIT_WEBRTC_CLIENT_H */
