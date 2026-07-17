/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "../webrtc/WhipWhepHttpRequester.h"
#include "../webrtc/WhipWhepProtocol.h"

using namespace std;
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

void testRequesterSynchronousStartFailureIsClean() {
    const string socket_error_sentinel = "throwing-socket-creator-sentinel";
    auto requester = make_shared<WhipWhepHttpRequester>();
    requester->setCustomHeaders("X-Trace-Id=before-start-failure");
    requester->setOnCreateSocket(
        [socket_error_sentinel](const toolkit::EventPoller::Ptr &) -> toolkit::Socket::Ptr {
            throw runtime_error(socket_error_sentinel);
        });

    bool callback_called = false;
    expectThrows(
        [&requester, &callback_called]() {
            requester->startRequester(
                "https://a.example/offer",
                [&callback_called](const toolkit::SockException &, const Parser &) { callback_called = true; });
        },
        "a synchronous socket-creation failure must be reported to the caller");
    expect(!callback_called, "a synchronous start failure must not invoke the asynchronous completion callback");
    expect(!requester->hasCredentials(), "failed-start cleanup must reset installed custom-header state");
    requester->setCustomHeaders("X-Trace-Id=after-start-failure");
    expect(requester->hasCredentials(), "the requester must remain reusable after failed-start cleanup");
    requester->clear();
}

void testParseTrickleIceSdpFrag() {
    const auto fragment = WhipWhepSdpFrag::parse(
        "a=ice-ufrag:remoteUfrag\r\n"
        "a=ice-pwd:remotePassword1234567890\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=candidate:842163049 1 udp 1677729535 192.0.2.1 54400 typ srflx raddr 10.0.0.1 rport 54400\r\n"
        "a=end-of-candidates\r\n");

    expect(fragment.ice_ufrag == "remoteUfrag", "fragment should retain ice-ufrag");
    expect(fragment.ice_pwd == "remotePassword1234567890", "fragment should retain ice-pwd");
    expect(fragment.candidates.size() == 1, "fragment should contain one candidate");
    expect(fragment.candidates.front().mid == "0", "candidate should be associated with its mid");
    expect(fragment.candidates.front().value.find("candidate:842163049") == 0,
           "candidate should retain the SDP candidate attribute");
    expect(fragment.completed_mids.size() == 1 && fragment.completed_mids.count("0"),
           "end-of-candidates should be associated with its mid");

    const auto rendered = fragment.toString();
    expect(rendered.find("a=ice-ufrag:remoteUfrag\r\n") != string::npos,
           "rendered fragment should include ice-ufrag");
    const auto media_line = rendered.find("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n");
    const auto mid_line = rendered.find("a=mid:0\r\n");
    expect(media_line != string::npos, "rendered fragment should preserve its pseudo media line");
    expect(mid_line != string::npos && media_line < mid_line,
           "the pseudo media line must precede its mid attribute");
    expect(rendered.find("a=mid:0\r\n") != string::npos,
           "rendered fragment should include mid");
    expect(rendered.find("a=end-of-candidates\r\n") != string::npos,
           "rendered fragment should include end-of-candidates");
}

void testRejectMalformedTrickleIceSdpFrag() {
    expectThrows(
        []() { WhipWhepSdpFrag::parse("a=mid:0\r\na=candidate:not-a-candidate\r\n"); },
        "malformed candidates must be rejected");
    expectThrows(
        []() { WhipWhepSdpFrag::parse("a=ice-ufrag:\r\n"); },
        "empty ice-ufrag must be rejected");
    expectThrows(
        []() {
            WhipWhepSdpFrag::parse(
                "m=audio 9 RTP/AVP 0\r\n"
                "a=mid:0\r\n"
                "a=end-of-candidates\r\n");
        },
        "end-of-candidates without current ICE credentials must be rejected");
    expectThrows(
        []() {
            WhipWhepSdpFrag::parse(
                "a=ice-ufrag:remoteUfrag\r\n"
                "a=ice-pwd:remotePassword1234567890\r\n"
                "a=mid:0\r\n"
                "a=candidate:1 1 udp 2122260223 192.0.2.2 54401 typ host\r\n");
        },
        "candidate updates without a pseudo media line must be rejected");
    expectThrows(
        []() {
            WhipWhepSdpFrag::parse(
                "a=ice-ufrag:abc\r\n"
                "a=ice-pwd:remotePassword1234567890\r\n");
        },
        "ICE username fragments shorter than four characters must be rejected");
    expectThrows(
        []() {
            WhipWhepSdpFrag::parse(
                "a=ice-ufrag:remoteUfrag\r\n"
                "a=ice-pwd:too-short\r\n");
        },
        "ICE passwords shorter than 22 characters must be rejected");
    expectThrows(
        []() {
            WhipWhepSdpFrag::parse(
                "a=ice-ufrag:remoteUfrag\r\n"
                "a=ice-pwd:remotePassword1234567890\r\n"
                "m=audio 9 RTP/AVP 0\r\n"
                "a=mid:0\r\n"
                "a=candidate:1 257 udp 2122260223 192.0.2.2 54401 typ host\r\n");
        },
        "ICE candidate component IDs greater than 256 must be rejected");
}

void testParseIceOptionsInSdpFrag() {
    const auto fragment = WhipWhepSdpFrag::parse(
        "a=ice-lite\r\n"
        "a=ice-options:trickle renomination\r\n"
        "a=ice-ufrag:localUfrag\r\n"
        "a=ice-pwd:localPassword1234567890\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=mid:0\r\n"
        "a=end-of-candidates\r\n");

    expect(fragment.ice_lite, "server ICE-lite marker should be retained");
    expect(fragment.ice_trickle, "trickle ICE option should be retained");
    expect(fragment.ice_renomination, "ICE renomination option should be retained");
    const auto rendered = fragment.toString();
    expect(rendered.find("a=ice-lite\r\n") != string::npos, "rendered fragment should include ice-lite");
    expect(rendered.find("a=ice-options:trickle renomination\r\n") != string::npos,
           "rendered fragment should include ICE options");
}

void testParseStandardBundledTrickleIceSdpFrag() {
    const auto fragment = WhipWhepSdpFrag::parse(
        "a=ice-ufrag:remoteUfrag\r\n"
        "a=ice-pwd:remotePassword1234567890\r\n"
        "a=ice-lite\r\n"
        "a=ice-options:trickle ice2 renomination\r\n"
        "a=ice-pacing:50\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=candidate:1 1 udp 2122260223 192.0.2.2 54401 typ host\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:1\r\n"
        "a=end-of-candidates\r\n");

    expect(fragment.bundle_mids.size() == 2 && fragment.bundle_mids[0] == "0" && fragment.bundle_mids[1] == "1",
           "standard BUNDLE groups should be retained");
    expect(fragment.ice_options.size() == 3 && fragment.ice_options[1] == "ice2",
           "unknown but standard ICE options should be retained");
    expect(fragment.ice_pacing == "50", "ICE pacing should be retained");
    const auto rendered = fragment.toString();
    expect(rendered.find("a=group:BUNDLE 0 1\r\n") != string::npos,
           "rendered fragment should include BUNDLE group");
    expect(rendered.find("a=ice-options:trickle ice2 renomination\r\n") != string::npos,
           "rendered fragment should preserve ICE options");
    expect(rendered.find("a=ice-pacing:50\r\n") != string::npos,
           "rendered fragment should include ICE pacing");
}

void testParseSessionCompletionAndExtensionAttributes() {
    const auto fragment = WhipWhepSdpFrag::parse(
        "a=ICE-UFRAG:remoteUfrag\r\n"
        "a=ICE-PWD:remotePassword1234567890\r\n"
        "a=x-future-session-extension:value\r\n"
        "a=end-of-candidates\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=MID:0\r\n"
        "a=rtcp-mux\r\n"
        "a=x-future-media-extension:value\r\n");

    expect(fragment.ice_ufrag == "remoteUfrag", "legacy SDP attribute names should be case insensitive");
    expect(fragment.ice_pwd == "remotePassword1234567890",
           "case-insensitive ICE credentials should retain their values");
    const auto rendered = fragment.toString();
    const auto completion = rendered.find("a=end-of-candidates\r\n");
    const auto media_line = rendered.find("m=audio 9 RTP/AVP 0\r\n");
    expect(completion != string::npos && media_line != string::npos && completion < media_line,
           "a session-level end-of-candidates marker must precede pseudo media descriptions");
}

void testRenderGeneratedMediaLevelIceFragment() {
    WhipWhepSdpFrag fragment;
    fragment.ice_ufrag = "localUfrag";
    fragment.ice_pwd = "localPassword1234567890";
    fragment.ice_lite = true;
    fragment.ice_trickle = true;
    fragment.ice_credentials_at_session_level = false;
    fragment.bundle_mids = { "0", "1" };
    fragment.candidates.emplace_back(WhipWhepIceCandidate{
        "0", "candidate:1 1 udp 2122260223 192.0.2.2 54401 typ host"
    });
    fragment.completed_mids.emplace("0");

    const auto rendered = fragment.toString();
    const auto media_line = rendered.find("m=audio 9 RTP/AVP 0\r\n");
    const auto mid_line = rendered.find("a=mid:0\r\n");
    const auto credentials = rendered.find("a=ice-ufrag:localUfrag\r\n");
    expect(rendered.find("a=group:BUNDLE 0 1\r\n") != string::npos,
           "generated restart fragments should retain the negotiated BUNDLE group");
    expect(media_line != string::npos && mid_line != string::npos && credentials != string::npos,
           "generated fragments should contain a complete pseudo media description");
    expect(media_line < mid_line && mid_line < credentials,
           "media-level ICE credentials should follow the pseudo media line and mid");
}

void testMediaLevelIceCredentialsOverrideSessionDefaults() {
    const auto fragment = WhipWhepSdpFrag::parse(
        "a=ice-ufrag:sessionUfrag\r\n"
        "a=ice-pwd:sessionPassword1234567890\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=mid:0\r\n"
        "a=ice-ufrag:mediaUfrag\r\n"
        "a=ice-pwd:mediaPassword1234567890\r\n"
        "a=candidate:1 1 udp 2122260223 192.0.2.2 54401 typ host\r\n");

    expect(fragment.ice_ufrag == "mediaUfrag" && fragment.ice_pwd == "mediaPassword1234567890",
           "media-level ICE credentials should override session defaults");
    expect(!fragment.ice_credentials_at_session_level,
           "effective media-level ICE credentials should retain their placement");

    const auto rendered = fragment.toString();
    const auto mid = rendered.find("a=mid:0\r\n");
    const auto ufrag = rendered.find("a=ice-ufrag:mediaUfrag\r\n");
    expect(mid != string::npos && ufrag != string::npos && mid < ufrag,
           "overriding ICE credentials should serialize at media level");
}

void testWhipWhepMediaTypes() {
    expect(WhipWhepProtocol::isSdpContentType("application/sdp"), "application/sdp should be accepted");
    expect(WhipWhepProtocol::isSdpContentType("Application/SDP; charset=utf-8"),
           "application/sdp parameters should be accepted case-insensitively");
    expect(WhipWhepProtocol::isTrickleIceSdpFragContentType("application/trickle-ice-sdpfrag"),
           "trickle ICE SDP fragments should be accepted");
    expect(!WhipWhepProtocol::isSdpContentType("text/plain"), "unrelated media types must be rejected");
    expect(!WhipWhepProtocol::isTrickleIceSdpFragContentType("application/sdp"),
           "SDP is not a trickle ICE SDP fragment");
}

void testWhipWhepOriginCanonicalization() {
    expect(WhipWhepProtocol::canonicalOrigin("HTTPS://Media.Example:443/path") == "https://media.example:443",
           "HTTPS origins should normalize scheme, host, and explicit port");
    expect(WhipWhepProtocol::canonicalOrigin("http://media.example/path") == "http://media.example:80",
           "HTTP origins should include the effective default port");
    expect(WhipWhepProtocol::canonicalOrigin("https://[2001:DB8::1]/path")
               == WhipWhepProtocol::canonicalOrigin("https://[2001:db8::1]:443/path"),
           "IPv6 origins should treat the default and explicit default ports as equal");
    expect(WhipWhepProtocol::canonicalOrigin("https://[2001:db8::1]/path")
               == WhipWhepProtocol::canonicalOrigin("https://[2001:0db8:0000:0000:0000:0000:0000:0001]/path"),
           "equivalent compressed and expanded IPv6 origins should canonicalize identically");
    expectThrows(
        []() { WhipWhepProtocol::canonicalOrigin("https://[not-an-ipv6]/path"); },
        "bracketed hosts must contain valid IPv6 literals");
}

void testWhipWhepTrustedOriginParsing() {
    const auto origins = WhipWhepProtocol::parseTrustedOrigins(
        " https://edge.example , http://origin.example:80 , https://edge.example ");
    expect(origins.size() == 2, "trusted origins should trim and deduplicate configured origins");
    expect(origins.count("https://edge.example:443") == 1,
           "trusted origins should store canonical HTTPS origins");
    expect(origins.count("http://origin.example:80") == 1,
           "trusted origins should retain canonical HTTP origins");
    expect(WhipWhepProtocol::parseTrustedOrigins(" \t ").empty(),
           "an empty trusted-origin configuration should produce an empty set");

    expectThrows(
        []() { WhipWhepProtocol::parseTrustedOrigins("https://edge.example/path"); },
        "trusted origins must reject paths");
    expectThrows(
        []() { WhipWhepProtocol::parseTrustedOrigins("https://edge.example?token=1"); },
        "trusted origins must reject queries");
    expectThrows(
        []() { WhipWhepProtocol::parseTrustedOrigins("https://edge.example#fragment"); },
        "trusted origins must reject fragments");
    expectThrows(
        []() { WhipWhepProtocol::parseTrustedOrigins("https://user:secret@edge.example"); },
        "trusted origins must reject userinfo");
    expectThrows(
        []() { WhipWhepProtocol::parseTrustedOrigins("ftp://edge.example"); },
        "trusted origins must reject non-HTTP schemes");
    expectThrows(
        []() { WhipWhepProtocol::parseTrustedOrigins("https://edge.example,,https://other.example"); },
        "trusted origins must reject empty comma-separated items");
}

void testWhipWhepCredentialedTargetPolicy() {
    string reason;
    const auto trusted = WhipWhepProtocol::parseTrustedOrigins("https://edge.example");

    expect(WhipWhepProtocol::isTargetAllowed("https://origin.example/live", "https://origin.example/session/1", true,
                                              trusted, reason),
           "same-origin targets should allow custom headers");
    expect(WhipWhepProtocol::isTargetAllowed("http://origin.example/live", "http://sink.example/session/1", false,
                                              trusted, reason),
           "HTTP cross-origin targets without credentials should be allowed");

    expect(!WhipWhepProtocol::isTargetAllowed(
               "https://user:secret@origin.example/live", "https://sink.example/session/1", false, trusted, reason),
           "credentialed cross-origin targets must be rejected by default");
    expect(reason.find("https://sink.example:443") != string::npos,
           "policy errors should identify only the normalized target origin");
    expect(reason.find("secret") == string::npos, "policy errors must not expose URL credentials");

    expect(!WhipWhepProtocol::isTargetAllowed("https://origin.example/live", "https://sink.example/session/1", true,
                                               trusted, reason),
           "cross-origin custom headers should require an allowlist entry");
    expect(!WhipWhepProtocol::isTargetAllowed("https://origin.example/live",
                                               "https://user:secret@sink.example/session/1", false, trusted, reason),
           "cross-origin target userinfo should require an allowlist entry");
    expect(WhipWhepProtocol::isTargetAllowed("https://origin.example/live", "https://edge.example/session/1", true,
                                              trusted, reason),
           "allowlisted canonical origins should allow credentialed cross-origin targets");
    expect(!WhipWhepProtocol::isTargetAllowed("https://origin.example/live",
                                               "https://trusted.example.attacker/session/1", true,
                                               WhipWhepProtocol::parseTrustedOrigins("https://trusted.example"), reason),
           "trusted origins must use exact canonical-origin matching");
    expect(!WhipWhepProtocol::isTargetAllowed("https://origin.example/live", "http://edge.example/session/1", false,
                                               WhipWhepProtocol::parseTrustedOrigins("http://edge.example"), reason),
           "HTTPS to HTTP targets must be rejected even when allowlisted");
}

void testWhipWhepInvalidTargetPolicyErrors() {
    const auto trusted = WhipWhepProtocol::parseTrustedOrigins("https://edge.example");
    const auto expect_invalid_target = [&trusted](const string &target, const string &sentinel) {
        string reason;
        expect(!WhipWhepProtocol::isTargetAllowed("https://origin.example/live", target, false, trusted, reason),
               "malformed and non-HTTP(S) targets must be rejected");
        expect(reason == "invalid WHIP/WHEP target URL", "invalid targets must use the fixed policy reason");
        expect(reason.find(sentinel) == string::npos, "invalid target errors must not expose raw URL data");
    };

    expect_invalid_target("https://raw-user:raw-secret@[not-an-ipv6]/session?raw-query=1", "raw-secret");
    expect_invalid_target("ftp://raw-user:raw-secret@target.example/session?raw-query=1", "raw-query");
}

void testWhipWhepUrlUserInfoDetection() {
    expect(WhipWhepProtocol::hasUrlUserInfo("https://user:secret@media.example/live"),
           "URL userinfo should be detected directly");
    expect(!WhipWhepProtocol::hasUrlUserInfo("https://media.example/live"),
           "URLs without userinfo should not be reported as credentialed");
}

void testWhipWhepSessionLocationResolution() {
    expect(WhipWhepProtocol::resolveSessionUrl("https://media.example.com/live/channel", "../sessions//abc")
               == "https://media.example.com/sessions//abc",
           "relative Location resolution must preserve empty path segments");
    expect(WhipWhepProtocol::resolveSessionUrl("https://user:secret@media.example.com/live/channel",
                                                "https://media.example.com:443/session/abc")
               == "https://user:secret@media.example.com:443/session/abc",
           "same-origin Location URLs must preserve userinfo across an explicit default port");
    expect(WhipWhepProtocol::resolveSessionUrl("https://user:secret@media.example.com/live/channel",
                                                "https://other.example.com/session/abc")
               == "https://other.example.com/session/abc",
           "cross-origin Location URLs must not inherit userinfo");
    expectThrows(
        []() { WhipWhepProtocol::resolveSessionUrl("https://media.example.com/live/channel", "   "); },
        "an empty Location URI-reference must be rejected");
}

class FakeIceTransport : public WhipWhepIceTransport {
public:
    bool hasCurrentIceCredentials(const WhipWhepSdpFrag &fragment) override {
        return fragment.ice_ufrag == current_remote_ufrag && fragment.ice_pwd == current_remote_pwd;
    }

    bool applyCandidates(const WhipWhepSdpFrag &fragment) override {
        ++apply_candidates_calls;
        last_candidates = fragment;
        return apply_candidates_result;
    }

    bool restartIce(const WhipWhepSdpFrag &remote_fragment, WhipWhepSdpFrag &local_fragment) override {
        ++restart_calls;
        last_remote_fragment = remote_fragment;
        if (!restart_result) {
            return false;
        }
        current_remote_ufrag = remote_fragment.ice_ufrag;
        current_remote_pwd = remote_fragment.ice_pwd;
        local_fragment.ice_ufrag = "localUfrag";
        local_fragment.ice_pwd = "localPassword1234567890";
        local_fragment.completed_mids.emplace("0");
        return true;
    }

    void close() override {
        ++close_calls;
    }

    bool apply_candidates_result = true;
    bool restart_result = true;
    size_t apply_candidates_calls = 0;
    size_t restart_calls = 0;
    size_t close_calls = 0;
    string current_remote_ufrag = "currentRemoteUfrag";
    string current_remote_pwd = "currentRemotePassword123456";
    WhipWhepSdpFrag last_candidates;
    WhipWhepSdpFrag last_remote_fragment;
};

WhipWhepSdpFrag makeCandidatePatch() {
    return WhipWhepSdpFrag::parse(
        "a=ice-ufrag:currentRemoteUfrag\r\n"
        "a=ice-pwd:currentRemotePassword123456\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=mid:0\r\n"
        "a=candidate:1 1 udp 2122260223 192.0.2.2 54401 typ host\r\n"
        "a=end-of-candidates\r\n");
}

WhipWhepSdpFrag makeRestartPatch() {
    return WhipWhepSdpFrag::parse(
        "a=ice-ufrag:newRemoteUfrag\r\n"
        "a=ice-pwd:newRemotePassword123456\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=mid:0\r\n"
        "a=candidate:2 1 udp 2122260223 192.0.2.3 54402 typ host\r\n");
}

void testCandidatePatchRequiresCurrentEtagAndIsAtomic() {
    auto transport = make_shared<FakeIceTransport>();
    WhipWhepSession session("etag-1", transport, []() { return "etag-2"; });
    const auto fragment = makeCandidatePatch();

    auto result = session.applyPatch(fragment, "");
    expect(result.status == WhipWhepPatchStatus::PreconditionRequired,
           "candidate PATCH without If-Match should require a precondition");
    expect(transport->apply_candidates_calls == 0, "missing If-Match must not mutate transport state");

    result = session.applyPatch(fragment, "etag-old");
    expect(result.status == WhipWhepPatchStatus::PreconditionFailed,
           "candidate PATCH with an old ETag should fail");
    expect(transport->apply_candidates_calls == 0, "mismatched ETag must not mutate transport state");

    result = session.applyPatch(fragment, "etag-1");
    expect(result.status == WhipWhepPatchStatus::Applied, "candidate PATCH with the current ETag should apply");
    expect(result.etag.empty(), "ordinary candidate PATCH must not return a new ETag");
    expect(transport->apply_candidates_calls == 1, "candidate PATCH should be applied exactly once");
    expect(transport->last_candidates.candidates.size() == 1, "transport should receive parsed candidates");
    expect(session.etag() == "etag-1", "ordinary candidate PATCH must preserve the ETag");
}

void testIceRestartRequiresWildcardAndRotatesEtag() {
    auto transport = make_shared<FakeIceTransport>();
    WhipWhepSession session("etag-1", transport, []() { return "etag-2"; });
    const auto fragment = makeRestartPatch();

    auto result = session.applyPatch(fragment, "etag-1");
    expect(result.status == WhipWhepPatchStatus::PreconditionFailed,
           "ICE restart must require If-Match: *");
    expect(transport->restart_calls == 0, "invalid restart precondition must not call the transport");

    result = session.applyPatch(fragment, "*");
    expect(result.status == WhipWhepPatchStatus::Restarted, "ICE restart with wildcard should apply");
    expect(result.etag == "etag-2", "ICE restart should return a new ETag");
    expect(result.response_fragment.hasIceRestartCredentials(),
           "ICE restart should return replacement local credentials");
    expect(transport->last_remote_fragment.ice_ufrag == "newRemoteUfrag", "restart should pass the remote ufrag to transport");
    expect(transport->last_remote_fragment.ice_pwd == "newRemotePassword123456", "restart should pass the remote pwd to transport");
    expect(transport->last_remote_fragment.candidates.size() == 1,
           "restart should pass accompanying remote candidates to transport");
    expect(session.etag() == "etag-2", "ICE restart should rotate session ETag");
}

void testCandidatePatchWithCurrentCredentialsIsNotAnIceRestart() {
    auto transport = make_shared<FakeIceTransport>();
    WhipWhepSession session("etag-1", transport, []() { return "etag-2"; });
    const auto fragment = WhipWhepSdpFrag::parse(
        "a=ice-ufrag:currentRemoteUfrag\r\n"
        "a=ice-pwd:currentRemotePassword123456\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=mid:0\r\n"
        "a=candidate:3 1 udp 2122260223 192.0.2.4 54403 typ host\r\n");

    const auto result = session.applyPatch(fragment, "etag-1");
    expect(result.status == WhipWhepPatchStatus::Applied,
           "a candidate PATCH with current ICE credentials must not be treated as a restart");
    expect(transport->apply_candidates_calls == 1, "candidate PATCH with credentials should reach the candidate adapter");
    expect(transport->restart_calls == 0, "only If-Match: * should select the ICE restart adapter");

    const auto wildcard_result = session.applyPatch(makeCandidatePatch(), "*");
    expect(wildcard_result.status == WhipWhepPatchStatus::PreconditionFailed,
           "If-Match: * without replacement credentials must not be accepted as a restart");
    expect(transport->restart_calls == 0, "missing restart credentials must not mutate the transport");
}

void testUnsupportedRestartAndClosedSessionDoNotMutateState() {
    auto transport = make_shared<FakeIceTransport>();
    transport->restart_result = false;
    WhipWhepSession session("etag-1", transport, []() { return "etag-2"; });

    auto result = session.applyPatch(makeRestartPatch(), "*");
    expect(result.status == WhipWhepPatchStatus::Unsupported,
           "an unsupported transport restart should map to Unsupported");
    expect(session.etag() == "etag-1", "an unsupported restart must not rotate the ETag");

    session.close();
    session.close();
    expect(transport->close_calls == 1, "session close should be idempotent");
    result = session.applyPatch(makeCandidatePatch(), "etag-1");
    expect(result.status == WhipWhepPatchStatus::Closed, "a closed session must reject later PATCH requests");
    expect(transport->apply_candidates_calls == 0, "closed session must not mutate transport state");
}

void testIceRestartDoesNotMutateTransportWhenEtagGenerationFails() {
    auto transport = make_shared<FakeIceTransport>();
    WhipWhepSession session("etag-1", transport, []() -> string {
        throw runtime_error("secure random source failed");
    });

    expectThrows(
        [&session]() { session.applyPatch(makeRestartPatch(), "*"); },
        "ETag generation failures should propagate to the HTTP adapter");
    expect(transport->restart_calls == 0,
           "ETag generation must complete before the transport starts an ICE restart");
    expect(session.etag() == "etag-1", "failed ETag generation must preserve the current ETag");
}

} // namespace

int main() {
    try {
        testRequesterSynchronousStartFailureIsClean();
        testParseTrickleIceSdpFrag();
        testRejectMalformedTrickleIceSdpFrag();
        testParseIceOptionsInSdpFrag();
        testParseStandardBundledTrickleIceSdpFrag();
        testParseSessionCompletionAndExtensionAttributes();
        testRenderGeneratedMediaLevelIceFragment();
        testMediaLevelIceCredentialsOverrideSessionDefaults();
        testWhipWhepMediaTypes();
        testWhipWhepOriginCanonicalization();
        testWhipWhepTrustedOriginParsing();
        testWhipWhepCredentialedTargetPolicy();
        testWhipWhepInvalidTargetPolicyErrors();
        testWhipWhepUrlUserInfoDetection();
        testWhipWhepSessionLocationResolution();
        testCandidatePatchRequiresCurrentEtagAndIsAtomic();
        testIceRestartRequiresWildcardAndRotatesEtag();
        testCandidatePatchWithCurrentCredentialsIsNotAnIceRestart();
        testUnsupportedRestartAndClosedSessionDoNotMutateState();
        testIceRestartDoesNotMutateTransportWhenEtagGenerationFails();
        cout << "test_whip_whep_protocol passed" << endl;
        return 0;
    } catch (const exception &ex) {
        cerr << "test_whip_whep_protocol failed: " << ex.what() << endl;
        return EXIT_FAILURE;
    }
}
