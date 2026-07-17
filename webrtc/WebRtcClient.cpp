/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Network/TcpClient.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "WebRtcClient.h"
#include "WhipWhepProtocol.h"

#include <cstdlib>

using namespace std;
using namespace toolkit;

namespace mediakit {

namespace {

string safeOriginForLog(const string &url) {
    try {
        return WhipWhepProtocol::canonicalOrigin(url);
    } catch (const exception &) {
        return "<invalid-whip-whep-origin>";
    }
}

bool isSdpContentType(const string &content_type) {
    return WhipWhepProtocol::isSdpContentType(content_type);
}

SockException makeHttpError(const Parser &response, const string &reason) {
    string message = reason + ", HTTP status: " + response.status();
    if (!response.content().empty()) {
        message += ", response bytes: " + to_string(response.content().size());
    }
    return SockException(Err_other, std::move(message));
}

} // namespace

// # WebRTCUrl 格式
// ## 标准 WHIP/WHEP 端点：http(s)://server_host[:port]/endpoint
// ## 基于 HTTP SFU 的 WHEP/WHIP：webrtc://server_host:server_port/{{app}}/{{streamid}}
// ## whep/whip over https sfu: webrtcs://server_host:server_port/{{app}}/{{streamid}}
// ## websocket p2p: webrtc://{{signaling_server_host}}:{{signaling_server_port}}/{{app}}/{{streamid}}?room_id={{peer_room_id}}
// ## websockets p2p: webrtcs://{{signaling_server_host}}:{{signaling_server_port}}/{{app}}/{{streamid}}?room_id={{peer_room_id}}
void WebRTCUrl::parse(const string &strUrl, bool isPlayer) {
    DebugL << "WebRTC URL origin: " << safeOriginForLog(strUrl);

    _is_ssl = false;
    _full_url = strUrl;
    _negotiate_url.clear();
    _delete_url.clear();
    _target_secret.clear();
    _params.clear();
    _host.clear();
    _port = 0;
    _vhost.clear();
    _app.clear();
    _stream.clear();
    _peer_room_id.clear();
    _signaling_protocols = WebRtcTransport::SignalingProtocols::WHEP_WHIP;

    auto url = strUrl;
    auto pos = url.find("?");
    if (pos != string::npos) {
        _params = url.substr(pos + 1);
        url.erase(pos);
    }

    auto schema_pos = url.find("://");
    bool is_standard_endpoint = false;
    if (schema_pos != string::npos) {
        auto schema = url.substr(0, schema_pos);
        if (strcasecmp(schema.c_str(), "http") == 0 || strcasecmp(schema.c_str(), "https") == 0) {
            _is_ssl = strcasecmp(schema.c_str(), "https") == 0;
            is_standard_endpoint = true;
        } else if (strcasecmp(schema.c_str(), "webrtc") == 0 || strcasecmp(schema.c_str(), "webrtcs") == 0) {
            _is_ssl = strcasecmp(schema.c_str(), "webrtcs") == 0;
        } else {
            throw invalid_argument("unsupported WebRTC URL scheme: " + schema);
        }
    } else {
        // 保留历史上无协议头 URL 的解析行为。
        schema_pos = -3;
    }
    // 设置默认端口。
    _port = _is_ssl ? 443 : 80;
    auto split_vec = split(url.substr(schema_pos + 3), "/");
    if (split_vec.size() > 0) {
        auto authority = split_vec[0];
        const auto at = authority.rfind('@');
        if (at != string::npos) {
            authority.erase(0, at + 1);
        }
        splitUrl(authority, _host, _port);
        _vhost = _host;
        if (_vhost == "localhost" || isIP(_vhost.data())) {
            // 如果访问的是localhost或ip，那么则为默认虚拟主机
            _vhost = DEFAULT_VHOST;
        }
    }
    if (split_vec.size() > 1) {
        _app = split_vec[1];
    }
    if (split_vec.size() > 2) {
        string stream_id;
        for (size_t i = 2; i < split_vec.size(); ++i) {
            stream_id.append(split_vec[i] + "/");
        }
        if (stream_id.back() == '/') {
            stream_id.pop_back();
        }
        _stream = stream_id;
    }

    // for vhost
    auto kv = Parser::parseArgs(_params);
    auto it = kv.find(VHOST_KEY);
    if (it != kv.end()) {
        _vhost = it->second;
    }

    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if (!enableVhost || _vhost.empty()) {
        // 如果关闭虚拟主机或者虚拟主机为空，则设置虚拟主机为默认
        _vhost = DEFAULT_VHOST;
    }

    // for peer_room_id
    it = kv.find("peer_room_id");
    if (it != kv.end()) {
        _peer_room_id = it->second;
    }

    if (is_standard_endpoint) {
        // HTTP URL 已经是完整的 WHEP/WHIP 端点，不再把路径或查询参数
        // 重新解释为 ZLMediaKit 私有字段。
        _signaling_protocols = WebRtcTransport::SignalingProtocols::WHEP_WHIP;
        _negotiate_url = strUrl;
        return;
    }

    it = kv.find("signaling_protocols");
    if (it != kv.end()) {
        _signaling_protocols = static_cast<WebRtcTransport::SignalingProtocols>(stoi(it->second));
    }

    auto suffix = _host + ":" + to_string(_port);
    suffix += (isPlayer ? "/index/api/whep" : "/index/api/whip");
    suffix += "?app=" + _app + "&stream=" + _stream;
    if (!_params.empty()) {
        suffix += "&" + _params;
    }
    if (_is_ssl) {
        _negotiate_url = StrPrinter << "https://" << suffix << endl;
    } else {
        _negotiate_url = StrPrinter << "http://" << suffix << endl;
    }
}

////////////  WebRtcClient //////////////////////////

WebRtcClient::WebRtcClient(toolkit::EventPoller::Ptr poller) {
    DebugL;
    _poller = poller ? std::move(poller) : EventPollerPool::Instance().getPoller();
}

WebRtcClient::~WebRtcClient() {
    doBye();
    DebugL;
}

void WebRtcClient::startConnect() {
    DebugL;
    doNegotiate();
}

void WebRtcClient::connectivityCheck() {
    DebugL;
    return _transport->connectivityCheckForSFU();
}

void WebRtcClient::onNegotiateFinish() {
    DebugL;
    _is_negotiate_finished = true;
    if (WebRtcTransport::SignalingProtocols::WEBSOCKET == _url._signaling_protocols) {
        // P2P模式需要gathering candidates
        gatheringCandidate(_peer->getIceServer());
    } else if (WebRtcTransport::SignalingProtocols::WHEP_WHIP == _url._signaling_protocols) {
        // SFU模式不会存在IP不通的情况， answer中就携带了candidates, 直接进行connectivityCheck
        connectivityCheck();
    }
}

void WebRtcClient::doNegotiate() {
    DebugL;
    switch (_url._signaling_protocols) {
        case WebRtcTransport::SignalingProtocols::WHEP_WHIP: return doNegotiateWhepOrWhip();
        case WebRtcTransport::SignalingProtocols::WEBSOCKET: return doNegotiateWebsocket();
        default: throw std::invalid_argument(StrPrinter << "not support signaling_protocols: " << (int)_url._signaling_protocols);
    }
}

WhipWhepHttpRequester::Ptr WebRtcClient::createWhipWhepRequester() {
    return make_shared<WhipWhepHttpRequester>(getPoller());
}

WhipWhepHttpRequester::Ptr WebRtcClient::makeWhipWhepRequester() {
    auto requester = createWhipWhepRequester();
    if (!requester) {
        throw invalid_argument("invalid WHIP/WHEP requester configuration");
    }
    requester->setCustomHeaders(_whip_whep_custom_header);
    requester->setTrustedOrigins(_whip_whep_trusted_origins);
    if (_whip_whep_on_create_socket) {
        requester->setOnCreateSocket(_whip_whep_on_create_socket);
    }
    requester->setProxyUrl(getWhipWhepProxyUrl());
    const auto net_adapter = getWhipWhepNetAdapter();
    if (!net_adapter.empty()) {
        requester->setNetAdapter(net_adapter);
    }
    return requester;
}

void WebRtcClient::prepareWhipWhepSecurityState(const string &endpoint_url) {
    try {
        auto trusted_origins = WhipWhepProtocol::parseTrustedOrigins(getWhipWhepTrustedOrigins());
        trusted_origins.emplace(WhipWhepProtocol::canonicalOrigin(endpoint_url));
        auto custom_header = getWhipWhepCustomHeader();
        _whip_whep_custom_header = std::move(custom_header);
        _whip_whep_trusted_origins = std::move(trusted_origins);
    } catch (const exception &) {
        throw invalid_argument("invalid WHIP/WHEP trusted origin configuration");
    }
}

string WebRtcClient::validateWhipWhepSessionUrl(const string &request_url, const string &location) const {
    const auto session_url = WhipWhepProtocol::resolveSessionUrl(request_url, location);
    string policy_error;
    if (!WhipWhepProtocol::isTargetAllowed(request_url,
                                           session_url,
                                           !_whip_whep_custom_header.empty(),
                                           _whip_whep_trusted_origins,
                                           policy_error)) {
        throw invalid_argument(policy_error);
    }
    return session_url;
}

void WebRtcClient::assignWhipWhepSessionUrl(const string &request_url, const string &location) {
    auto session_url = validateWhipWhepSessionUrl(request_url, location);
    _url._delete_url = std::move(session_url);
}

SockException WebRtcClient::getWhipWhepRequestError(
    const SockException &network_error, const WhipWhepHttpRequester::Ptr &requester) {
    if (requester && !requester->policyError().empty()) {
        return SockException(Err_other, requester->policyError());
    }
    if (network_error) {
        return SockException(network_error.getErrCode(), "WHIP/WHEP HTTP request failed");
    }
    return SockException();
}

void WebRtcClient::doNegotiateWhepOrWhip() {
    DebugL << "WHIP/WHEP negotiate origin: " << safeOriginForLog(_url._negotiate_url);

    try {
        prepareWhipWhepSecurityState(_url._negotiate_url);
    } catch (const exception &) {
        onResult(SockException(Err_other, "invalid WHIP/WHEP trusted origin configuration"));
        return;
    }

    WhipWhepHttpRequester::Ptr requester;
    try {
        requester = makeWhipWhepRequester();
    } catch (const exception &) {
        onResult(SockException(Err_other, "invalid WHIP/WHEP requester configuration"));
        return;
    }

    weak_ptr<WebRtcClient> weak_self = static_pointer_cast<WebRtcClient>(shared_from_this());
    auto offer_sdp = _transport->createOfferSdp();
    DebugL << "send WHIP/WHEP offer, bytes: " << offer_sdp.size();

    _negotiate = requester;
    requester->setMethod("POST");
    requester->setApplicationSdpContentType();
    requester->setBody(std::move(offer_sdp));
    try {
        requester->startRequester(
            _url._negotiate_url,
            [weak_self, requester](const toolkit::SockException &ex, const Parser &response) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }

                const auto request_error = strong_self->getWhipWhepRequestError(ex, requester);
                if (request_error) {
                    strong_self->onResult(request_error);
                    return;
                }

                DebugL << "status: " << response.status() << ", Location origin: " << safeOriginForLog(response["Location"])
                       << ", answer bytes: " << response.content().size();

                const auto &request_url = requester->getUrl();
                if (response.status() == "406" && strong_self->isPlayer()) {
                    try {
                        strong_self->assignWhipWhepSessionUrl(request_url, response["Location"]);
                    } catch (const exception &) {
                        strong_self->onResult(SockException(Err_other, "invalid WHIP/WHEP session Location"));
                        return;
                    }
                    try {
                        if (!isSdpContentType(response["Content-Type"])) {
                            throw invalid_argument("WHEP counter-offer response must use Content-Type: application/sdp");
                        }
                        if (response.content().empty()) {
                            throw invalid_argument("WHEP counter-offer response has an empty SDP body");
                        }
                        strong_self->doAnswerWhepCounterOffer(response.content());
                    } catch (const exception &) {
                        strong_self->onResult(SockException(Err_other, "invalid WHEP counter-offer response"));
                    }
                    return;
                }

                if (response.status() != "201") {
                    strong_self->onResult(makeHttpError(response, "WHIP/WHEP negotiation failed"));
                    return;
                }
                try {
                    strong_self->assignWhipWhepSessionUrl(request_url, response["Location"]);
                } catch (const exception &) {
                    strong_self->onResult(SockException(Err_other, "invalid WHIP/WHEP session Location"));
                    return;
                }
                try {
                    if (!isSdpContentType(response["Content-Type"])) {
                        throw invalid_argument("WHIP/WHEP 201 response must use Content-Type: application/sdp");
                    }
                    if (response.content().empty()) {
                        throw invalid_argument("WHIP/WHEP 201 response has an empty SDP body");
                    }

                    strong_self->_transport->setAnswerSdp(response.content());
                    strong_self->onNegotiateFinish();
                } catch (const exception &) {
                    strong_self->onResult(SockException(Err_other, "invalid WHIP/WHEP negotiation response"));
                }
            },
            getTimeOutSec());
    } catch (...) {
        onResult(SockException(Err_other, "WHIP/WHEP HTTP request failed"));
    }
}

void WebRtcClient::doAnswerWhepCounterOffer(const string &server_offer) {
    string answer_sdp;
    try {
        answer_sdp = _transport->getAnswerSdp(server_offer);
    } catch (const exception &) {
        onResult(SockException(Err_other, "invalid WHEP counter-offer"));
        return;
    }

    DebugL << "send WHEP counter-offer answer, bytes: " << answer_sdp.size();
    WhipWhepHttpRequester::Ptr requester;
    try {
        requester = makeWhipWhepRequester();
    } catch (const exception &) {
        onResult(SockException(Err_other, "invalid WHIP/WHEP requester configuration"));
        return;
    }
    _counter_offer = requester;
    requester->setMethod("PATCH");
    requester->setApplicationSdpContentType();
    requester->setBody(std::move(answer_sdp));

    weak_ptr<WebRtcClient> weak_self = static_pointer_cast<WebRtcClient>(shared_from_this());
    try {
        requester->startRequester(
            _url._delete_url,
            [weak_self, requester](const toolkit::SockException &ex, const Parser &response) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }

                const auto request_error = strong_self->getWhipWhepRequestError(ex, requester);
                if (request_error) {
                    strong_self->onResult(request_error);
                    return;
                }
                if (response.status() != "204") {
                    strong_self->onResult(makeHttpError(response, "WHEP counter-offer answer was rejected"));
                    return;
                }
                strong_self->onNegotiateFinish();
            },
            getTimeOutSec());
    } catch (...) {
        onResult(SockException(Err_other, "WHIP/WHEP HTTP request failed"));
    }
}

void WebRtcClient::doNegotiateWebsocket() {
    DebugL;
#if 0
    //TODO: 当前暂将每一路呼叫都使用一个独立的peer_connection,不复用
    _peer = getWebrtcRoomKeeper(_url._host, _url._port);
    if (_peer) {
        checkIn();
        return;
    }
#endif

    // 未注册的,先增加注册流程，并在此次播放结束后注销
    InfoL << (StrPrinter << "register to signaling server " << _url._host << "::" << _url._port << " first");
    auto room_id = "ringing_" + makeRandStr(16);
    _peer = make_shared<WebRtcSignalingPeer>(_url._host, _url._port, _url._is_ssl, room_id);
    weak_ptr<WebRtcClient> weak_self = static_pointer_cast<WebRtcClient>(shared_from_this());
    _peer->setOnConnect([weak_self](const SockException &ex) {
        if (auto strong_self = weak_self.lock()) {
            if (ex) {
                strong_self->onResult(ex);
                return;
            }

            auto cb = [weak_self](const SockException &ex, const string &key) {
                if (auto strong_self = weak_self.lock()) {
                    strong_self->checkIn();
                }
            };
            strong_self->_peer->regist(cb);
        }
    });
    _peer->connect();
}

void WebRtcClient::checkIn() {
    DebugL;
    weak_ptr<WebRtcClient> weak_self = static_pointer_cast<WebRtcClient>(shared_from_this());
    auto tuple = MediaTuple(_url._vhost, _url._app, _url._stream, _url._params);
    _peer->checkIn(_url._peer_room_id, tuple, _transport->getIdentifier(), _transport->createOfferSdp(), isPlayer(),
                   [weak_self](const SockException &ex, const std::string &answer) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        if (ex) {
            WarnL << "network err:" << ex;
            strong_self->onResult(ex);
            return;
        }

        strong_self->_transport->setAnswerSdp(answer);
        strong_self->onNegotiateFinish();
    }, getTimeOutSec());
}

void WebRtcClient::checkOut() {
    DebugL;
    auto tuple = MediaTuple(_url._vhost, _url._app, _url._stream);
    if (_peer) {
        _peer->checkOut(_url._peer_room_id);
        _peer->unregist([](const SockException &ex) {});
    }
}

void WebRtcClient::candidate(const std::string &candidate, const std::string &ufrag, const std::string &pwd) {
    _peer->candidate(_transport->getIdentifier(), candidate, ufrag, pwd);
}

void WebRtcClient::gatheringCandidate(IceServerInfo::Ptr ice_server) {
    DebugL;
    std::weak_ptr<WebRtcClient> weak_self = std::static_pointer_cast<WebRtcClient>(shared_from_this());
    _transport->gatheringCandidate(ice_server, [weak_self](const std::string& transport_identifier, const std::string& candidate,
        const std::string& ufrag, const std::string& pwd) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->candidate(candidate, ufrag, pwd);
    });
}

void WebRtcClient::doBye() {
    DebugL;
    if (!_is_negotiate_finished) {
        return;
    }

    _is_negotiate_finished = false;

    switch (_url._signaling_protocols) {
        case WebRtcTransport::SignalingProtocols::WHEP_WHIP:
            doByeWhepOrWhip();
            break;
        case WebRtcTransport::SignalingProtocols::WEBSOCKET:
            checkOut();
            break;
        default: throw std::invalid_argument(StrPrinter << "not support signaling_protocols: " << (int)_url._signaling_protocols);
    }
}

void WebRtcClient::doByeWhepOrWhip() {
    DebugL;
    if (_url._delete_url.empty()) {
        WarnL << "WHIP/WHEP session resource URL is empty; skip DELETE";
        return;
    }

    WhipWhepHttpRequester::Ptr requester;
    try {
        requester = makeWhipWhepRequester();
    } catch (const exception &) {
        WarnL << "invalid WHIP/WHEP requester configuration";
        return;
    }
    _delete_requester = requester;
    requester->setMethod("DELETE");
    const auto delete_origin = safeOriginForLog(_url._delete_url);
    try {
        requester->startRequester(
            _url._delete_url,
            [requester, delete_origin](const toolkit::SockException &ex, const Parser &response) {
                const auto request_error = WebRtcClient::getWhipWhepRequestError(ex, requester);
                if (request_error) {
                    WarnL << "WHIP/WHEP DELETE request failed, error code: " << static_cast<int>(request_error.getErrCode()) << ", origin: " << delete_origin;
                    return;
                }
                if (response.status() != "200") {
                    WarnL << "WHIP/WHEP DELETE unexpected status: " << response.status() << ", response bytes: " << response.content().size()
                          << ", origin: " << delete_origin;
                    return;
                }
                DebugL << "WHIP/WHEP DELETE status: " << response.status() << ", origin: " << delete_origin;
            },
            getTimeOutSec());
    } catch (...) {
        WarnL << "WHIP/WHEP DELETE request start failed, origin: " << delete_origin;
    }
}

float WebRtcClient::getTimeOutSec() {
    GET_CONFIG(uint32_t, timeout, Rtc::kTimeOutSec);
    if (timeout <= 0) {
        WarnL << "config rtc. " << Rtc::kTimeOutSec << ": " << timeout << " not vaild";
        return 5.0;
    }
    return (float)timeout;
}

} /* namespace mediakit */
