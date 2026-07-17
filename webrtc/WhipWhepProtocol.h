/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_WHIP_WHEP_PROTOCOL_H
#define ZLMEDIAKIT_WHIP_WHEP_PROTOCOL_H

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace mediakit {

struct WhipWhepIceCandidate {
    std::string mid;
    // 不含前导 "a=" 的 SDP candidate 属性。
    std::string value;
};

struct WhipWhepSdpFragMedia {
    std::string mid;
    // 不含前导 "m=" 的伪媒体行。
    std::string value;
};

// WHIP/WHEP PATCH 使用的 RFC 8840 SDP fragment。该结构有意与 RtcSession 解耦，
// 因为 fragment 不包含 RtcSession 所需的完整 SDP 字段。
struct WhipWhepSdpFrag {
    std::string ice_ufrag;
    std::string ice_pwd;
    bool ice_lite { false };
    bool ice_trickle { false };
    bool ice_renomination { false };
    // 保留对端声明的全部选项（包括 ice2 等未来 IANA 选项），避免仅因本端暂不解析
    // 某项语义就拒绝符合标准的对端。
    std::vector<std::string> ice_options;
    std::string ice_pacing;
    std::vector<std::string> bundle_mids;
    std::vector<WhipWhepSdpFragMedia> media;
    std::vector<WhipWhepIceCandidate> candidates;
    std::set<std::string> completed_mids;
    bool end_of_candidates { false };
    bool ice_credentials_at_session_level { true };

    static WhipWhepSdpFrag parse(const std::string &sdp_frag);
    std::string toString() const;
    void swap(WhipWhepSdpFrag &other) noexcept;

    bool hasIceRestartCredentials() const {
        return !ice_ufrag.empty() && !ice_pwd.empty();
    }
};

class WhipWhepProtocol {
public:
    static std::string normalizeMediaType(const std::string &content_type);
    static bool isSdpContentType(const std::string &content_type);
    static bool isTrickleIceSdpFragContentType(const std::string &content_type);
    static std::string canonicalOrigin(const std::string &url);
    static bool hasUrlUserInfo(const std::string &url);
    static std::set<std::string> parseTrustedOrigins(const std::string &origins);
    static std::string resolveSessionUrl(const std::string &request_url, const std::string &location);
    static bool isTargetAllowed(const std::string &source_url,
                                const std::string &target_url,
                                bool has_custom_headers,
                                const std::set<std::string> &trusted_origins,
                                std::string &reason);
};

// 该窄接口使 HTTP PATCH 状态与 ICE 状态切换保持原子且可独立测试。实现必须在返回前
// 完成调用；WhipWhepSession 会将其与 ETag 状态一起串行化。
class WhipWhepIceTransport {
public:
    virtual ~WhipWhepIceTransport() = default;

    virtual bool hasCurrentIceCredentials(const WhipWhepSdpFrag &fragment) = 0;
    virtual bool applyCandidates(const WhipWhepSdpFrag &fragment) = 0;
    virtual bool restartIce(const WhipWhepSdpFrag &remote_fragment,
                            WhipWhepSdpFrag &local_fragment) = 0;
    virtual void close() = 0;
};

enum class WhipWhepPatchStatus {
    Applied,
    Restarted,
    PreconditionRequired,
    PreconditionFailed,
    Unsupported,
    Closed,
};

struct WhipWhepPatchResult {
    WhipWhepPatchStatus status { WhipWhepPatchStatus::Unsupported };
    std::string etag;
    WhipWhepSdpFrag response_fragment;
};

class WhipWhepSession {
public:
    using EtagFactory = std::function<std::string()>;

    WhipWhepSession(std::string etag, std::shared_ptr<WhipWhepIceTransport> transport, EtagFactory next_etag);

    WhipWhepPatchResult applyPatch(const WhipWhepSdpFrag &fragment, const std::string &if_match);
    std::string etag() const;
    bool closed() const;
    void close();

private:
    mutable std::mutex _mtx;
    std::string _etag;
    std::shared_ptr<WhipWhepIceTransport> _transport;
    EtagFactory _next_etag;
    bool _closed { false };
};

} // namespace mediakit

#endif // ZLMEDIAKIT_WHIP_WHEP_PROTOCOL_H
