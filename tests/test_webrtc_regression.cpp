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
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(ENABLE_OPENSSL)
#include <openssl/crypto.h>
#endif

#include "Common/Parser.h"
#include "Common/strCoding.h"
#include "Http/HttpClient.h"
#include "Http/HttpRequester.h"
#include "Poller/EventPoller.h"
#include "../webrtc/Sdp.h"
#include "../webrtc/WebRtcClient.h"
#include "../webrtc/WhipWhepHttpRequester.h"
#include "../webrtc/WhipWhepProtocol.h"

using namespace std;
using namespace mediakit;

namespace {

void expect(bool cond, const string &msg) {
    if (!cond) {
        throw runtime_error(msg);
    }
}

void releaseOpenSslThreadStateForLeakSanitizer() {
#if defined(ENABLE_OPENSSL) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    toolkit::EventPollerPool::Instance().for_each([](const toolkit::TaskExecutor::Ptr &executor) {
        executor->sync([]() { OPENSSL_thread_stop(); });
    });
    OPENSSL_thread_stop();
#endif
}

void expectThrows(const function<void()> &fn, const string &msg) {
    try {
        fn();
    } catch (const exception &) {
        return;
    }
    throw runtime_error(msg);
}

string exceptionMessage(const function<void()> &fn, const string &msg) {
    try {
        fn();
    } catch (const exception &ex) {
        return ex.what();
    }
    throw runtime_error(msg);
}

string makeBundleOnlyDatachannelOffer() {
    return
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "a=msid-semantic: WMS\r\n"
        "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:remoteUfrag\r\n"
        "a=ice-pwd:remotePassword1234567890\r\n"
        "a=fingerprint:sha-256 "
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
        "a=setup:actpass\r\n"
        "a=mid:0\r\n"
        "a=bundle-only\r\n"
        "a=sctp-port:5000\r\n";
}

void testBundleOnlyDatachannelAnswer() {
    RtcSession offer;
    offer.loadFrom(makeBundleOnlyDatachannelOffer());
    offer.checkValid();

    expect(offer.media.size() == 1, "offer should contain a single application m-line");
    expect(offer.media[0].bundle_only, "offer application m-line should preserve a=bundle-only");

    SdpAttrFingerprint local_fingerprint;
    local_fingerprint.algorithm = "sha-256";
    local_fingerprint.hash =
        "FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00:"
        "FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00";

    RtcConfigure configure;
    configure.setDefaultSetting("localUfrag", "localPassword1234567890", RtpDirection::sendrecv, local_fingerprint);

    auto answer = configure.createAnswer(offer);
    expect(answer != nullptr, "createAnswer should return a session");
    expect(answer->media.size() == 1, "answer should contain a single application m-line");

#ifdef ENABLE_SCTP
    answer->checkValid();
    expect(answer->media[0].port == 9, "bundle-only application m-line should use port 9 in answer");
    expect(answer->group.mids.size() == 1 && answer->group.mids[0] == "0",
           "accepted bundle-only application m-line should remain in group:BUNDLE");
#else
    expect(answer->media[0].port == 0, "application m-line should stay rejected when SCTP is disabled");
#endif
}

void testWhipWhepOfferConstraints() {
    SdpAttrFingerprint fingerprint;
    fingerprint.algorithm = "sha-256";
    fingerprint.hash =
        "FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00:"
        "FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00";

    RtcConfigure configure;
    configure.setDefaultSetting("localUfrag", "localPassword1234567890", RtpDirection::sendrecv, fingerprint);
    const auto offer = configure.createOffer();
    expect(offer && offer->media.size() >= 2, "a standard WebRTC offer should contain audio and video media");

    const auto rendered = offer->toString();
    size_t mux_only_count = 0;
    size_t ice2_count = 0;
    for (size_t pos = 0; (pos = rendered.find("a=rtcp-mux-only\r\n", pos)) != string::npos; ++pos) {
        ++mux_only_count;
    }
    for (size_t pos = 0; (pos = rendered.find("a=ice-options:trickle ice2\r\n", pos)) != string::npos; ++pos) {
        ++ice2_count;
    }
    expect(mux_only_count == offer->media.size(), "every bundled RTP media section must advertise rtcp-mux-only");
    expect(ice2_count == offer->media.size(), "every offered media section must advertise ICE RFC 8445 support");

    string media_stream_id;
    for (const auto &media : offer->media) {
        expect(!media.rtp_rtx_ssrc.empty(), "sendrecv media should carry an msid");
        const auto separator = media.rtp_rtx_ssrc.front().msid.find(' ');
        expect(separator != string::npos, "msid should contain a MediaStream and MediaStreamTrack identifier");
        const auto current_stream_id = media.rtp_rtx_ssrc.front().msid.substr(0, separator);
        if (media_stream_id.empty()) {
            media_stream_id = current_stream_id;
        } else {
            expect(current_stream_id == media_stream_id,
                   "all WHIP/WHEP media sections must belong to the same MediaStream");
        }
    }
}

void testDeleteWebrtcLocationQueryRoundTrip() {
    const string raw_id = "Ab+/9";
    const string raw_token = "token+/9";

    HttpArgs args;
    args["id"] = raw_id;
    args["token"] = raw_token;
    auto query = args.make();

    expect(query.find("id=Ab%2B%2F9") != string::npos, "id should be URL-encoded in delete_webrtc query");
    expect(query.find("token=token%2B%2F9") != string::npos,
           "token should be URL-encoded in delete_webrtc query");

    auto parsed = Parser::parseArgs(query);
    expect(strCoding::UrlDecodeComponent(parsed["id"]) == raw_id, "encoded id should round-trip through query parsing");
    expect(strCoding::UrlDecodeComponent(parsed["token"]) == raw_token,
           "encoded token should round-trip through query parsing");
}

void testStandardWhipWhepEndpointUrls() {
    const string whep_endpoint = "https://whep.example.com/live/channel?token=viewer-token";
    WebRTCUrl whep_url;
    whep_url.parse(whep_endpoint, true);

    expect(whep_url._negotiate_url == whep_endpoint,
           "an https WHEP endpoint must be used without rewriting its path or query");
    expect(whep_url._is_ssl, "an https WHEP endpoint must enable TLS");
    expect(whep_url._signaling_protocols == WebRtcTransport::SignalingProtocols::WHEP_WHIP,
           "an http(s) endpoint must use WHIP/WHEP signaling");

    const string whip_endpoint = "http://whip.example.com:8080/custom/ingest?stream=primary";
    WebRTCUrl whip_url;
    whip_url.parse(whip_endpoint, false);

    expect(whip_url._negotiate_url == whip_endpoint,
           "an http WHIP endpoint must be used without rewriting its path or query");
    expect(!whip_url._is_ssl, "an http WHIP endpoint must not enable TLS");
}

void testLegacyWebrtcUrlStillMapsToBuiltinEndpoint() {
    WebRTCUrl legacy_url;
    legacy_url.parse("webrtcs://media.example.com/live/camera?token=publisher-token", false);

    expect(legacy_url._negotiate_url
               == "https://media.example.com:443/index/api/whip?app=live&stream=camera&token=publisher-token",
           "legacy webrtcs URLs must retain the built-in WHIP endpoint mapping");
}

class InspectableWhipWhepRequester : public WhipWhepHttpRequester {
public:
    using WhipWhepHttpRequester::WhipWhepHttpRequester;

    bool evaluateRedirect(const string &source_url, const string &target_url) {
        return isRedirectAllowed(source_url, target_url);
    }
};

template <typename T>
class HasPublicAddHeader {
private:
    template <typename U>
    static auto test(int) -> decltype(
        std::declval<U &>().addHeader(std::string(), std::string(), true),
        std::true_type());
    template <typename>
    static std::false_type test(...);

public:
    static const bool value = decltype(test<T>(0))::value;
};

template <typename T>
class HasPublicSetHeader {
private:
    template <typename U>
    static auto test(int) -> decltype(
        std::declval<U &>().setHeader(HttpClient::HttpHeader()),
        std::true_type());
    template <typename>
    static std::false_type test(...);

public:
    static const bool value = decltype(test<T>(0))::value;
};

static_assert(!HasPublicAddHeader<WhipWhepHttpRequester>::value,
              "WhipWhepHttpRequester must hide addHeader");
static_assert(!HasPublicSetHeader<WhipWhepHttpRequester>::value,
              "WhipWhepHttpRequester must hide setHeader");
static_assert(!std::is_base_of<HttpRequester, WhipWhepHttpRequester>::value,
              "WhipWhepHttpRequester must not derive from HttpRequester");
static_assert(!std::is_convertible<WhipWhepHttpRequester *, HttpRequester *>::value,
              "WhipWhepHttpRequester pointers must not convert to HttpRequester pointers");
static_assert(!std::is_convertible<WhipWhepHttpRequester::Ptr, HttpRequester::Ptr>::value,
              "WhipWhepHttpRequester shared_ptrs must not convert to HttpRequester shared_ptrs");
static_assert(HasPublicAddHeader<HttpRequester>::value,
              "header visibility test must detect HttpRequester::addHeader");
static_assert(HasPublicSetHeader<HttpRequester>::value,
              "header visibility test must detect HttpRequester::setHeader");

class SessionPolicyWebRtcClient final : public WebRtcClient {
public:
    SessionPolicyWebRtcClient() : WebRtcClient(nullptr) {}

    void setSecurityConfiguration(string custom_headers, string trusted_origins) {
        _custom_headers = std::move(custom_headers);
        _trusted_origins = std::move(trusted_origins);
    }

    WhipWhepHttpRequester::Ptr prepareRequester(const string &endpoint_url) {
        prepareWhipWhepSecurityState(endpoint_url);
        return makeWhipWhepRequester();
    }

    shared_ptr<InspectableWhipWhepRequester> inspectableRequester() const {
        return _inspectable_requester;
    }

    void setThrowingSocketCreator(const string &sentinel) {
        setWhipWhepSocketCreator([sentinel](const toolkit::EventPoller::Ptr &) -> toolkit::Socket::Ptr {
            throw runtime_error(sentinel);
        });
    }

    void deleteSession(const string &session_url) {
        _url._delete_url = session_url;
        doByeWhepOrWhip();
    }

    void assignSessionUrl(const string &request_url, const string &location) {
        assignWhipWhepSessionUrl(request_url, location);
    }

    const string &sessionUrl() const {
        return _url._delete_url;
    }

    void setSessionUrl(string value) {
        _url._delete_url = std::move(value);
    }

    static toolkit::SockException selectRequestError(
        const toolkit::SockException &network_error,
        const WhipWhepHttpRequester::Ptr &requester) {
        return getWhipWhepRequestError(network_error, requester);
    }

protected:
    bool isPlayer() override { return true; }
    void onResult(const toolkit::SockException &) override {}
    string getWhipWhepCustomHeader() override { return _custom_headers; }
    string getWhipWhepTrustedOrigins() override { return _trusted_origins; }
    WhipWhepHttpRequester::Ptr createWhipWhepRequester() override {
        _inspectable_requester = make_shared<InspectableWhipWhepRequester>(getPoller());
        return _inspectable_requester;
    }

private:
    string _custom_headers;
    string _trusted_origins;
    shared_ptr<InspectableWhipWhepRequester> _inspectable_requester;
};

void testWhipWhepRequesterHeaderApiIsNarrow() {
    expect(!HasPublicAddHeader<WhipWhepHttpRequester>::value,
           "the dedicated requester must hide the general addHeader API");
    expect(!HasPublicSetHeader<WhipWhepHttpRequester>::value,
           "the dedicated requester must hide the general setHeader API");

    WhipWhepHttpRequester requester;
    requester.setApplicationSdpContentType();
    requester.setApplicationSdpAccept();
}

void testWhipWhepRequesterRejectsRoutingHeaders() {
    const vector<string> invalid_headers = {
        "Host=attacker.example.com",
        "Content-Length=1",
        "Connection=close",
        "\rX-Test=value",
        "\nX-Test=value",
        "X-Test=value\r",
        "X-Test=value\n",
        "Bad\rName=value",
        "Bad\nName=value",
        "X-Test=good\rInjected: bad",
        "X-Test=good\nInjected: bad",
    };

    for (const auto &encoded_headers : invalid_headers) {
        InspectableWhipWhepRequester requester;
        const auto error = exceptionMessage(
            [&requester, &encoded_headers]() { requester.setCustomHeaders(encoded_headers); },
            "custom WHIP/WHEP routing or CR/LF headers must be rejected atomically");
        expect(error == "invalid WHIP/WHEP custom header configuration",
               "routing and raw CR/LF rejection must use a fixed configuration error");
        expect(!requester.hasCredentials(), "a rejected custom-header configuration must not be installed");
    }

    InspectableWhipWhepRequester requester;
    const auto error = exceptionMessage(
        [&requester]() { requester.setCustomHeaders("A-Valid=installed-first&Host=attacker.example.com"); },
        "mixed valid and invalid custom headers must be rejected atomically");
    expect(error == "invalid WHIP/WHEP custom header configuration",
           "custom-header configuration failures must use a fixed redacted error");
    expect(error.find("attacker.example.com") == string::npos,
           "custom-header configuration failures must not echo header values");
    expect(!requester.hasCredentials(), "atomic validation must not retain a valid prefix before a bad header");
}

void testWhipWhepRequesterLifecycleIsCoherent() {
    InspectableWhipWhepRequester requester;
    requester.setCustomHeaders("X-Trace-Id=first-value");
    const auto second_error = exceptionMessage(
        [&requester]() { requester.setCustomHeaders("Authorization=second-value"); },
        "a second custom-header configuration without clear must fail");
    expect(second_error == "WHIP/WHEP custom headers already configured",
           "a second custom-header configuration must use a fixed error");
    expect(requester.hasCredentials(), "a rejected second configuration must retain the first credential state");

    expect(!requester.evaluateRedirect("https://a.example/offer", "https://untrusted.example/session"),
           "the first credential state must remain effective after second-config rejection");
    expect(!requester.policyError().empty(), "a rejected redirect must set policyError before clear");

    requester.clear();
    expect(!requester.hasCredentials(), "clear must reset custom-header credential state");
    expect(requester.policyError().empty(), "clear must reset policyError");
    requester.setCustomHeaders("");
    expect(!requester.hasCredentials(), "an empty first configuration after clear must remain uncredentialed");
    expectThrows(
        [&requester]() { requester.setCustomHeaders("X-New=value"); },
        "an empty successful configuration must still consume the one-shot lifecycle");

    requester.clear();
    expectThrows(
        [&requester]() { requester.setCustomHeaders("\rX-Rejected=value"); },
        "raw CR must reject a reused requester configuration");
    expectThrows(
        [&requester]() { requester.setCustomHeaders("X-Must-Clear=value"); },
        "a rejected configuration must consume the one-shot lifecycle until clear");
    requester.clear();
    requester.setCustomHeaders("X-Reconfigured=value");
    expect(requester.hasCredentials(), "a rejected configuration followed by clear must allow clean reconfiguration");
}

void testWhipWhepRequesterMarksEveryCustomHeaderAsCredential() {
    InspectableWhipWhepRequester requester;
    requester.setCustomHeaders("X-Trace-Id=trace-1");
    expect(requester.hasCredentials(), "every custom WHIP/WHEP header must be classified as credential material");
}

void testWhipWhepRequesterRedirectPolicy() {
    InspectableWhipWhepRequester requester;
    requester.setCustomHeaders("X-Trace-Id=trace-1");

    expect(requester.evaluateRedirect("https://a.example/offer", "https://a.example/session"),
           "same-origin redirects must preserve credentials");
    expect(requester.policyError().empty(), "an allowed same-origin redirect must clear old policy errors");

    expect(!requester.evaluateRedirect("https://a.example/offer", "https://untrusted.example/session"),
           "credentialed cross-origin redirects must require an allowlist entry");
    expect(!requester.policyError().empty(), "a rejected cross-origin redirect must expose a policy error");

    expect(!requester.evaluateRedirect("https://a.example/offer", "http://a.example/session"),
           "HTTPS redirects must never downgrade to HTTP");
    expect(!requester.policyError().empty(), "a rejected TLS downgrade must expose a policy error");

    auto trusted_origins = WhipWhepProtocol::parseTrustedOrigins("https://b.example");
    trusted_origins.emplace(WhipWhepProtocol::canonicalOrigin("https://a.example/offer"));
    requester.setTrustedOrigins(trusted_origins);
    expect(requester.evaluateRedirect("https://a.example/offer", "https://b.example/session"),
           "an allowlisted cross-origin redirect must be accepted");
    expect(requester.evaluateRedirect("https://b.example/session", "https://a.example/final"),
           "the runtime trusted set must allow a credentialed A-to-B-to-A redirect chain");
}

void testWhipWhepRequesterUrlCredentialPolicy() {
    InspectableWhipWhepRequester requester;
    expect(!requester.evaluateRedirect(
               "https://user:password@a.example/offer", "https://untrusted.example/session"),
           "URL userinfo alone must make a cross-origin redirect credentialed");
    expect(requester.evaluateRedirect("https://a.example/offer", "https://untrusted.example/session"),
           "an uncredentialed cross-origin redirect may be followed");
}

void testWhipWhepRequesterSynchronousStartFailure() {
    const string socket_error_sentinel = "throwing-socket-creator-sentinel";
    auto requester = make_shared<WhipWhepHttpRequester>();
    requester->setCustomHeaders("X-Trace-Id=before-start-failure");
    requester->setOnCreateSocket(
        [socket_error_sentinel](const toolkit::EventPoller::Ptr &) -> toolkit::Socket::Ptr {
            throw runtime_error(socket_error_sentinel);
        });

    bool callback_called = false;
    const auto error = exceptionMessage(
        [&requester, &callback_called]() {
            requester->startRequester(
                "https://a.example/offer",
                [&callback_called](const toolkit::SockException &, const Parser &) { callback_called = true; });
        },
        "a synchronous socket-creation failure must be reported to the caller");
    expect(error == socket_error_sentinel, "the direct wrapper must preserve its synchronous failure contract");
    expect(!callback_called, "a synchronous start failure must not also invoke the asynchronous completion callback");
    expect(!requester->hasCredentials(), "failed-start cleanup must reset installed custom-header state");
    requester->setCustomHeaders("X-Trace-Id=after-start-failure");
    expect(requester->hasCredentials(), "the requester must remain reusable after failed-start cleanup");

    SessionPolicyWebRtcClient client;
    client.setThrowingSocketCreator(socket_error_sentinel);
    try {
        client.deleteSession("https://a.example/session/delete");
    } catch (...) {
        throw runtime_error("a synchronous DELETE start failure must not escape the WebRTC client teardown path");
    }
}

void testWhipWhepSessionLocationPolicy() {
    const string endpoint_url = "https://user:secret@a.example/offer?token=request-token";
    SessionPolicyWebRtcClient client;
    client.setSecurityConfiguration("X-Trace-Id=trace-1", "https://b.example");
    const auto requester = client.prepareRequester(endpoint_url);
    expect(requester->hasCredentials(), "the production factory must install prepared custom headers");
    const auto inspectable_requester = client.inspectableRequester();
    expect(inspectable_requester && inspectable_requester == requester,
           "the production factory must configure the requester returned by its creation seam");
    expect(inspectable_requester->evaluateRedirect(endpoint_url, "https://b.example/session/redirected"),
           "the production factory must install explicitly trusted redirect origins");
    expect(inspectable_requester->evaluateRedirect(
               "https://b.example/session/redirected", "https://a.example/session/return"),
           "the production factory must retain the initial origin for an A-to-B-to-A redirect chain");

    client.assignSessionUrl("https://a.example/offer", "session/same-origin");
    expect(client.sessionUrl() == "https://a.example/session/same-origin",
           "a relative same-origin Location must resolve and update the session URL atomically");

    client.assignSessionUrl("https://b.example/redirected", "https://a.example/session/return");
    expect(client.sessionUrl() == "https://a.example/session/return",
           "production security preparation must trust the initial endpoint for B-to-A return");

    const string previous_session_url = "https://a.example/session/previous";
    client.setSessionUrl(previous_session_url);
    const auto untrusted_error = exceptionMessage(
        [&client]() {
            client.assignSessionUrl(
                "https://a.example/offer", "https://user-info@untrusted.example/path-sentinel?token=query-sentinel");
        },
        "an untrusted 201/406-style Location must be rejected");
    expect(client.sessionUrl() == previous_session_url,
           "an untrusted 201/406-style Location must leave the existing session URL unchanged");
    expect(untrusted_error.find("user-info") == string::npos,
           "session assignment errors must redact Location userinfo");
    expect(untrusted_error.find("path-sentinel") == string::npos,
           "session assignment errors must redact Location paths");
    expect(untrusted_error.find("query-sentinel") == string::npos,
           "session assignment errors must redact Location queries");

    expectThrows(
        [&client]() { client.assignSessionUrl("https://a.example/offer", "http://a.example/session/downgrade"); },
        "HTTPS 201/406-style Location values must not downgrade to HTTP");
    expect(client.sessionUrl() == previous_session_url,
           "a rejected HTTPS downgrade must leave the existing session URL unchanged");

    client.assignSessionUrl("https://a.example/offer", "https://b.example/session/two");
    expect(client.sessionUrl() == "https://b.example/session/two",
           "a trusted 201/406-style Location must update the session URL");
}

void testWhipWhepPolicyErrorsRedactCredentialMaterial() {
    const string userinfo_sentinel = "userinfo-sentinel";
    const string query_sentinel = "query-token-sentinel";
    const string header_sentinel = "header-value-sentinel";
    auto requester = make_shared<InspectableWhipWhepRequester>();
    requester->setCustomHeaders("Authorization=" + header_sentinel);

    expect(!requester->evaluateRedirect(
               "https://source-user:source-secret@a.example/offer?token=" + query_sentinel,
               "https://" + userinfo_sentinel + ":secret@evil.example/session?token=" + query_sentinel),
           "a credentialed redirect to an untrusted target must be rejected");
    const auto &error = requester->policyError();
    expect(error.find("https://evil.example:443") != string::npos,
           "a redirect policy error must identify only the canonical target origin");
    expect(error.find(userinfo_sentinel) == string::npos, "policy errors must redact target userinfo");
    expect(error.find(query_sentinel) == string::npos, "policy errors must redact query credentials");
    expect(error.find(header_sentinel) == string::npos, "policy errors must redact custom-header credentials");

    const string network_sentinel = "network-error-sentinel";
    const auto selected_error = SessionPolicyWebRtcClient::selectRequestError(
        toolkit::SockException(toolkit::Err_other, network_sentinel),
        requester);
    expect(selected_error.getErrCode() == toolkit::Err_other, "a redirect policy failure must be reported as an error");
    expect(string(selected_error.what()).find("https://evil.example:443") != string::npos,
           "policyError must take precedence over a network error");
    expect(string(selected_error.what()).find(network_sentinel) == string::npos,
           "policyError precedence must not leak raw network error text");

    auto clean_requester = make_shared<WhipWhepHttpRequester>();
    const auto redacted_network_error = SessionPolicyWebRtcClient::selectRequestError(
        toolkit::SockException(toolkit::Err_timeout, network_sentinel),
        clean_requester);
    expect(redacted_network_error.getErrCode() == toolkit::Err_timeout,
           "network error redaction must preserve the original error code");
    expect(string(redacted_network_error.what()) == "WHIP/WHEP HTTP request failed",
           "network error redaction must return fixed WHIP/WHEP failure text");
    expect(string(redacted_network_error.what()).find(network_sentinel) == string::npos,
           "returned network errors must not contain raw exception text");
}

class InspectableWebRtcTransport : public WebRtcTransportImp {
public:
    struct TrackAddressSnapshot {
        const RtcSession *offer = nullptr;
        const RtcSession *answer = nullptr;
        const RtcMedia *media = nullptr;
        const RtcCodecPlan *plan_rtp = nullptr;
        const RtcCodecPlan *plan_rtx = nullptr;
    };

    explicit InspectableWebRtcTransport(const toolkit::EventPoller::Ptr &poller)
        : WebRtcTransportImp(poller) {}

    ~InspectableWebRtcTransport() override {
        WebRtcTransport::onDestory();
    }

    void initializeForTest() {
        // Avoid the manager self-reference and timeout timer installed by
        // WebRtcTransportImp::onCreate(); these tests only exercise SDP negotiation.
        WebRtcTransport::onCreate();
    }

    void createRegisteredForTest() {
        WebRtcTransportImp::onCreate();
        _registered_for_test = true;
    }

    void destroyRegisteredForTest() {
        if (_registered_for_test) {
            WebRtcTransportImp::onDestory();
            _registered_for_test = false;
        }
    }

    void startWebRtcForTest() {
        WebRtcTransportImp::onStartWebRTC();
    }

    TrackAddressSnapshot trackAddressSnapshot(TrackType type) const {
        TrackAddressSnapshot result;
        result.offer = _offer_sdp.get();
        result.answer = _answer_sdp.get();
        const auto *track = mediaTrack(type);
        if (track) {
            result.media = track->media;
            result.plan_rtp = track->plan_rtp;
            result.plan_rtx = track->plan_rtx;
        }
        return result;
    }

    const RtcSession::Ptr &offerSdpForTest() const {
        return _offer_sdp;
    }

    string iceUfragForTest() const {
        return _ice_agent ? _ice_agent->getUfrag() : string();
    }

    string icePasswordForTest() const {
        return _ice_agent ? _ice_agent->getPassword() : string();
    }

    Json::Value iceChecklistForTest() const {
        return _ice_agent ? _ice_agent->getChecklistInfo() : Json::Value();
    }

    set<string> completedMidsForTest() const {
        return whipWhepCompletedMids();
    }

    void setNextIceRestartCredentials(string ufrag, string pwd) {
        _next_restart_ufrag = std::move(ufrag);
        _next_restart_pwd = std::move(pwd);
    }

    void failNextIceAgentRestart() {
        _fail_next_ice_agent_restart = true;
    }

    bool hasAppliedRemoteCandidate() const {
        return _has_applied_remote_candidate;
    }

    const CandidateInfo &lastAppliedRemoteCandidate() const {
        return _last_applied_remote_candidate;
    }

    const vector<CandidateInfo> &lastRestartRemoteCandidates() const {
        return _last_restart_remote_candidates;
    }

    void rejectNextAnswerCheck() {
        _reject_next_answer_check = true;
    }

    LocalSdpRole answerCheckObservedRole() const {
        return _answer_check_observed_role;
    }

    bool stagedAnswerWasPublishedDuringCheck() const {
        return _staged_answer_was_published;
    }

    using WebRtcTransport::localDirectionForAnswerMedia;
    using WebRtcTransport::localDtlsRole;
    using WebRtcTransport::localMedia;
    using WebRtcTransport::localSdp;
    using WebRtcTransport::makeRemoteCandidatesForSFU;
    using WebRtcTransport::remoteMedia;
    using WebRtcTransport::remoteDtlsFingerprint;
    using WebRtcTransport::remoteSdp;
    using WebRtcTransportImp::canRecvRtp;
    using WebRtcTransportImp::canSendRtp;

protected:
    bool addWhipWhepRemoteCandidate(const CandidateInfo &candidate) override {
        _last_applied_remote_candidate = candidate;
        _has_applied_remote_candidate = true;
        return WebRtcTransport::addWhipWhepRemoteCandidate(candidate);
    }

    bool restartWhipWhepIceAgent(const string &ufrag, const string &password,
                                 const vector<CandidateInfo> &remote_candidates) override {
        _last_restart_remote_candidates = remote_candidates;
        if (_fail_next_ice_agent_restart) {
            _fail_next_ice_agent_restart = false;
            return false;
        }
        return WebRtcTransport::restartWhipWhepIceAgent(ufrag, password, remote_candidates);
    }

    string makeIceRestartCredential(size_t bytes) const override {
        if (bytes == 12 && !_next_restart_ufrag.empty()) {
            return _next_restart_ufrag;
        }
        if (bytes == 16 && !_next_restart_pwd.empty()) {
            return _next_restart_pwd;
        }
        return WebRtcTransport::makeIceRestartCredential(bytes);
    }

    void onCheckSdp(SdpType type, RtcSession &sdp) override {
        if (type == SdpType::answer) {
            _answer_check_observed_role = localSdpRole();
            _staged_answer_was_published = answerSdp().get() == &sdp;
        }
        WebRtcTransportImp::onCheckSdp(type, sdp);
        if (type == SdpType::answer && _reject_next_answer_check) {
            _reject_next_answer_check = false;
            throw runtime_error("injected answer validation failure");
        }
    }

private:
    bool _reject_next_answer_check = false;
    bool _staged_answer_was_published = false;
    bool _registered_for_test = false;
    bool _fail_next_ice_agent_restart = false;
    bool _has_applied_remote_candidate = false;
    CandidateInfo _last_applied_remote_candidate;
    vector<CandidateInfo> _last_restart_remote_candidates;
    string _next_restart_ufrag;
    string _next_restart_pwd;
    LocalSdpRole _answer_check_observed_role = LocalSdpRole::Unset;
};

class RegisteredWebRtcTransportFixture {
public:
    explicit RegisteredWebRtcTransportFixture(WebRtcTransport::Role role = WebRtcTransport::Role::PEER)
        : _poller(toolkit::EventPollerPool::Instance().getPoller())
        , _transport(make_shared<InspectableWebRtcTransport>(_poller)) {
        _poller->sync([this, role]() {
            _transport->setRole(role);
            _transport->createRegisteredForTest();
        });
    }

    ~RegisteredWebRtcTransportFixture() {
        if (_transport) {
            auto transport = _transport;
            _poller->sync([transport]() { transport->destroyRegisteredForTest(); });
            _transport.reset();
        }
    }

    const shared_ptr<InspectableWebRtcTransport> &transport() const {
        return _transport;
    }

    template <typename Func>
    void sync(Func func) const {
        _poller->sync(std::move(func));
    }

private:
    toolkit::EventPoller::Ptr _poller;
    shared_ptr<InspectableWebRtcTransport> _transport;
};

SdpAttrFingerprint makeNegotiationTestFingerprint() {
    SdpAttrFingerprint fingerprint;
    fingerprint.algorithm = "sha-256";
    fingerprint.hash =
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    return fingerprint;
}

RtcConfigure makeNegotiationTestConfigure(const string &ufrag, const string &pwd, RtpDirection direction) {
    RtcConfigure configure;
    configure.setDefaultSetting(ufrag, pwd, direction, makeNegotiationTestFingerprint());
    return configure;
}

SdpAttrCandidate makeNegotiationTestCandidate(const string &address, uint16_t port) {
    SdpAttrCandidate candidate;
    candidate.foundation = "test";
    candidate.component = 1;
    candidate.transport = "udp";
    candidate.priority = 2113937151;
    candidate.address = address;
    candidate.port = port;
    candidate.type = "host";
    return candidate;
}

const CandidateInfo *findRemoteCandidate(const vector<CandidateInfo> &candidates, const string &address,
                                         uint16_t port) {
    for (const auto &candidate : candidates) {
        if (candidate._addr._host == address && candidate._addr._port == port) {
            return &candidate;
        }
    }
    return nullptr;
}

using IceCredentialList = vector<pair<string, string>>;

struct IceStateSnapshot {
    const RtcSession *offer = nullptr;
    const RtcSession *answer = nullptr;
    IceCredentialList offer_credentials;
    IceCredentialList answer_credentials;
    string agent_ufrag;
    string agent_pwd;
    set<string> completed_mids;
    string checklist;
};

IceCredentialList sessionIceCredentials(const RtcSession::Ptr &session) {
    IceCredentialList result;
    if (!session) {
        return result;
    }
    for (const auto &media : session->media) {
        result.emplace_back(media.ice_ufrag, media.ice_pwd);
    }
    return result;
}

IceStateSnapshot captureIceState(const InspectableWebRtcTransport &transport) {
    IceStateSnapshot result;
    result.offer = transport.offerSdpForTest().get();
    result.answer = transport.answerSdp().get();
    result.offer_credentials = sessionIceCredentials(transport.offerSdpForTest());
    result.answer_credentials = sessionIceCredentials(transport.answerSdp());
    result.agent_ufrag = transport.iceUfragForTest();
    result.agent_pwd = transport.icePasswordForTest();
    result.completed_mids = transport.completedMidsForTest();
    result.checklist = transport.iceChecklistForTest().toStyledString();
    return result;
}

void expectIceStateEquals(const IceStateSnapshot &actual, const IceStateSnapshot &expected, const string &context) {
    expect(actual.offer == expected.offer, context + ": offer address changed");
    expect(actual.answer == expected.answer, context + ": answer address changed");
    expect(actual.offer_credentials == expected.offer_credentials, context + ": offer ICE credentials changed");
    expect(actual.answer_credentials == expected.answer_credentials, context + ": answer ICE credentials changed");
    expect(actual.agent_ufrag == expected.agent_ufrag, context + ": ICE agent ufrag changed");
    expect(actual.agent_pwd == expected.agent_pwd, context + ": ICE agent password changed");
    expect(actual.completed_mids == expected.completed_mids, context + ": completed MID state changed");
    expect(actual.checklist == expected.checklist, context + ": ICE candidate/checklist state changed");
}

bool everyMediaUsesCredentials(const RtcSession::Ptr &session, const string &ufrag, const string &pwd) {
    if (!session || session->media.empty()) {
        return false;
    }
    for (const auto &media : session->media) {
        if (media.ice_ufrag != ufrag || media.ice_pwd != pwd) {
            return false;
        }
    }
    return true;
}

bool checklistHasRemoteHost(const Json::Value &checklist, const string &host) {
    const auto &candidates = checklist["remote_candidates"];
    for (Json::ArrayIndex index = 0; index < candidates.size(); ++index) {
        if (candidates[index]["host"].asString() == host) {
            return true;
        }
    }
    return false;
}

void negotiateAsLocalAnswer(InspectableWebRtcTransport &transport) {
    auto remote_configure = makeNegotiationTestConfigure(
        "initialRemoteOfferUfrag", "initialRemoteOfferPassword1234", RtpDirection::sendrecv);
    auto remote_offer = remote_configure.createOffer();
    const auto candidate = makeNegotiationTestCandidate("203.0.113.40", 44000);
    for (auto &media : remote_offer->media) {
        media.direction = RtpDirection::recvonly;
        media.candidate.emplace_back(candidate);
    }
    remote_offer->checkValid();
    transport.getAnswerSdp(remote_offer->toString());
}

void negotiateAsLocalOffer(InspectableWebRtcTransport &transport) {
    transport.createOfferSdp();
    auto remote_configure = makeNegotiationTestConfigure(
        "initialRemoteAnswerUfrag", "initialRemoteAnswerPassword123", RtpDirection::recvonly);
    remote_configure.addCandidate(makeNegotiationTestCandidate("203.0.113.41", 44001));
    auto remote_answer = remote_configure.createAnswer(*transport.localSdp());
    remote_answer->checkValid();
    transport.setAnswerSdp(remote_answer->toString());
}

WhipWhepSdpFrag makeRestartFragment(const RtcSession &offer, string ufrag, string pwd,
                                    const string &address, uint16_t port, bool completed) {
    WhipWhepSdpFrag fragment;
    fragment.ice_ufrag = std::move(ufrag);
    fragment.ice_pwd = std::move(pwd);
    fragment.bundle_mids = offer.group.mids;
    const auto &offerer_tag = offer.group.mids.front();
    const auto candidate = makeNegotiationTestCandidate(address, port);
    fragment.candidates.emplace_back(WhipWhepIceCandidate{ offerer_tag, "candidate:" + candidate.toString() });
    if (completed) {
        fragment.completed_mids.emplace(offerer_tag);
    }
    return fragment;
}

WhipWhepSdpFrag makeCurrentCompletedFragment(const InspectableWebRtcTransport &transport) {
    const auto &offer = *transport.offerSdpForTest();
    const auto &offerer_tag = offer.group.mids.front();
    const auto *remote_media = transport.remoteMedia(offerer_tag);
    WhipWhepSdpFrag fragment;
    fragment.ice_ufrag = remote_media->ice_ufrag;
    fragment.ice_pwd = remote_media->ice_pwd;
    fragment.bundle_mids = offer.group.mids;
    fragment.completed_mids.emplace(offerer_tag);
    return fragment;
}

WhipWhepSdpFrag makeCurrentCandidateFragment(const InspectableWebRtcTransport &transport) {
    const auto &offer = *transport.offerSdpForTest();
    const auto &offerer_tag = offer.group.mids.front();
    const auto *remote_media = transport.remoteMedia(offerer_tag);
    if (!remote_media || remote_media->candidate.empty()) {
        throw runtime_error("candidate ownership fixture requires a remote candidate");
    }

    WhipWhepSdpFrag fragment;
    fragment.ice_ufrag = remote_media->ice_ufrag;
    fragment.ice_pwd = remote_media->ice_pwd;
    fragment.bundle_mids = offer.group.mids;
    fragment.candidates.emplace_back(
        WhipWhepIceCandidate{ offerer_tag, "candidate:" + remote_media->candidate.front().toString() });
    return fragment;
}

vector<string> mediaCandidateValues(const RtcMedia *media) {
    vector<string> result;
    if (!media) {
        return result;
    }
    for (const auto &candidate : media->candidate) {
        result.emplace_back("candidate:" + candidate.toString());
    }
    return result;
}

vector<string> fragmentCandidateValues(const WhipWhepSdpFrag &fragment) {
    vector<string> result;
    for (const auto &candidate : fragment.candidates) {
        result.emplace_back(candidate.value);
    }
    return result;
}

void expectMediaOwnershipViews(const InspectableWebRtcTransport &transport, TrackType type) {
    const auto &local = transport.localSdp();
    const auto &remote = transport.remoteSdp();
    const auto *expected_local = local->getMedia(type);
    const auto *expected_remote = remote->getMedia(type);

    expect(expected_local != nullptr && expected_remote != nullptr,
           "both SDP ownership views should contain the selected media type");
    expect(transport.localMedia(type) == expected_local,
           "local media lookup by type must return an element of localSdp");
    expect(transport.remoteMedia(type) == expected_remote,
           "remote media lookup by type must return an element of remoteSdp");
    expect(transport.localMedia(expected_local->mid) == expected_local,
           "local media lookup by MID must stay within localSdp");
    expect(transport.remoteMedia(expected_remote->mid) == expected_remote,
           "remote media lookup by MID must stay within remoteSdp");
}

void testClientOffer201UsesRemoteAnswer() {
    InspectableWebRtcTransport transport(toolkit::EventPollerPool::Instance().getPoller());
    transport.setRole(WebRtcTransport::Role::CLIENT);
    transport.initializeForTest();

    transport.createOfferSdp();
    const auto local_offer = transport.localSdp();
    expect(transport.localSdpRole() == WebRtcTransport::LocalSdpRole::Offer,
           "createOfferSdp must establish the local endpoint as offerer");

    const string remote_ufrag = "remote201Ufrag";
    const string remote_pwd = "remote201Password1234567890";
    const string remote_address = "203.0.113.201";
    const uint16_t remote_port = 42001;
    auto remote_configure = makeNegotiationTestConfigure(remote_ufrag, remote_pwd, RtpDirection::recvonly);
    remote_configure.addCandidate(makeNegotiationTestCandidate(remote_address, remote_port));
    const auto generated_remote_answer = remote_configure.createAnswer(*local_offer);
    generated_remote_answer->checkValid();

    transport.setAnswerSdp(generated_remote_answer->toString());
    const auto wire_answer = transport.answerSdp();

    expect(transport.localSdpRole() == WebRtcTransport::LocalSdpRole::Offer,
           "a normal 201 exchange must retain the local offerer role");
    expect(transport.answerCheckObservedRole() == WebRtcTransport::LocalSdpRole::Offer,
           "remote-answer validation should observe the already validated local offerer role");
    expect(!transport.stagedAnswerWasPublishedDuringCheck(),
           "a remote answer must not be published before its validation hook succeeds");
    expect(transport.localSdp().get() == local_offer.get(),
           "a normal 201 exchange must keep the offer as local SDP");
    expect(transport.remoteSdp().get() == wire_answer.get(),
           "a normal 201 exchange must expose the answer as remote SDP");
    expectMediaOwnershipViews(transport, TrackVideo);

    const auto remote_candidates = transport.makeRemoteCandidatesForSFU();
    const auto *remote_candidate = findRemoteCandidate(remote_candidates, remote_address, remote_port);
    expect(remote_candidate != nullptr, "201 connectivity checks must use a candidate from the remote answer");
    expect(remote_candidate->_ufrag == remote_ufrag && remote_candidate->_pwd == remote_pwd,
           "201 remote candidates must carry credentials from the remote answer media");

    const auto *answer_media = wire_answer->getMedia(TrackVideo);
    expect(answer_media != nullptr && answer_media->direction == RtpDirection::recvonly,
           "the generated remote answer should be recvonly for the direction regression");
    expect(transport.localDirectionForAnswerMedia(*answer_media) == RtpDirection::sendonly,
           "an offerer must reverse the answer direction to obtain its local direction");
    expect(transport.canSendRtp(*answer_media) && !transport.canRecvRtp(*answer_media),
           "a local sendonly 201 result must enable sending and disable receiving");
    expect(transport.canSendRtp() && !transport.canRecvRtp(),
           "transport-wide capability must preserve the local sendonly 201 result");
    expect(transport.localDtlsRole() == DtlsRole::active,
           "an offerer must reverse a passive remote answer into the local active DTLS role");
    expect(transport.remoteDtlsFingerprint().value == wire_answer->media[0].fingerprint.hash,
           "a normal 201 exchange must install the fingerprint from the remote answer");
}

void testWhep406CounterOfferUsesRemoteOffer() {
    InspectableWebRtcTransport transport(toolkit::EventPollerPool::Instance().getPoller());
    transport.setRole(WebRtcTransport::Role::CLIENT);
    transport.initializeForTest();

    // WHEP first sends a local offer; a 406 response then replaces the exchange
    // with a server counter-offer and a local answer.
    transport.createOfferSdp();
    const auto initial_local_offer = transport.localSdp();

    const string remote_ufrag = "remote406Ufrag";
    const string remote_pwd = "remote406Password1234567890";
    const string remote_address = "203.0.113.206";
    const uint16_t remote_port = 42006;
    auto remote_configure = makeNegotiationTestConfigure(remote_ufrag, remote_pwd, RtpDirection::sendrecv);
    const auto generated_remote_offer = remote_configure.createOffer();
    const auto sentinel = makeNegotiationTestCandidate(remote_address, remote_port);
    for (auto &media : generated_remote_offer->media) {
        media.direction = RtpDirection::sendonly;
        media.candidate.emplace_back(sentinel);
    }
    generated_remote_offer->checkValid();

    const auto remote_offer_text = generated_remote_offer->toString();
    transport.rejectNextAnswerCheck();
    expectThrows(
        [&transport, &remote_offer_text]() { transport.getAnswerSdp(remote_offer_text); },
        "a failed WHEP counter-offer answer check must reject the negotiation");
    expect(transport.localSdpRole() == WebRtcTransport::LocalSdpRole::Offer,
           "a failed WHEP counter-offer must restore the prior local offerer role");
    expect(transport.localSdp().get() == initial_local_offer.get(),
           "a failed WHEP counter-offer must restore the prior local offer pointer");
    expectThrows(
        [&transport]() { static_cast<void>(transport.remoteSdp()); },
        "a failed WHEP counter-offer must not leave a staged remote SDP visible");

    transport.getAnswerSdp(remote_offer_text);
    const auto wire_answer = transport.answerSdp();

    expect(transport.localSdpRole() == WebRtcTransport::LocalSdpRole::Answer,
           "a WHEP 406 counter-offer must establish the local endpoint as answerer");
    expect(transport.answerCheckObservedRole() == WebRtcTransport::LocalSdpRole::Unset,
           "local Answer ownership must not be committed before answer validation succeeds");
    expect(!transport.stagedAnswerWasPublishedDuringCheck(),
           "a local answer must not be published before its validation hook succeeds");
    expect(transport.localSdp().get() == wire_answer.get(),
           "a WHEP 406 exchange must expose the answer as local SDP");
    expect(transport.remoteSdp().get() != wire_answer.get(),
           "a WHEP 406 exchange must expose the counter-offer as remote SDP");
    expectMediaOwnershipViews(transport, TrackVideo);

    const auto remote_candidates = transport.makeRemoteCandidatesForSFU();
    const auto *remote_candidate = findRemoteCandidate(remote_candidates, remote_address, remote_port);
    expect(remote_candidate != nullptr, "406 connectivity checks must use a candidate from the remote offer");
    expect(remote_candidate->_ufrag == remote_ufrag && remote_candidate->_pwd == remote_pwd,
           "406 remote candidates must carry credentials from the remote offer media");

    const auto *answer_media = wire_answer->getMedia(TrackVideo);
    expect(answer_media != nullptr && answer_media->direction == RtpDirection::recvonly,
           "the local answer to a sendonly counter-offer should be recvonly");
    expect(transport.localDirectionForAnswerMedia(*answer_media) == RtpDirection::recvonly,
           "an answerer must use the local answer direction without reversing it");
    expect(transport.canRecvRtp(*answer_media) && !transport.canSendRtp(*answer_media),
           "a local recvonly 406 answer must enable receiving and disable sending");
    expect(transport.canRecvRtp() && !transport.canSendRtp(),
           "transport-wide media capability must preserve the local recvonly 406 answer");
    expect(transport.localDtlsRole() == DtlsRole::passive,
           "an answerer must use the passive role selected in its local answer");
    expect(transport.remoteDtlsFingerprint().value == transport.remoteSdp()->media[0].fingerprint.hash,
           "a WHEP 406 exchange must install the fingerprint from the remote offer");
    expect(transport.remoteDtlsFingerprint().value != wire_answer->media[0].fingerprint.hash,
           "a WHEP 406 exchange must not install the fingerprint from its local answer");
}

static_assert(noexcept(std::declval<WhipWhepSdpFrag &>().swap(std::declval<WhipWhepSdpFrag &>())),
              "WhipWhepSdpFrag::swap must be noexcept for the post-agent restart commit");

void testWhipWhepSdpFragSwapIsComplete() {
    WhipWhepSdpFrag first;
    first.ice_ufrag = "firstUfrag";
    first.ice_pwd = "firstPassword123456789012";
    first.ice_lite = true;
    first.ice_trickle = true;
    first.ice_renomination = false;
    first.ice_options = { "trickle", "ice2" };
    first.ice_pacing = "50";
    first.bundle_mids = { "0" };
    first.media.emplace_back(WhipWhepSdpFragMedia{ "0", "audio 9 UDP/TLS/RTP/SAVPF 111" });
    first.candidates.emplace_back(
        WhipWhepIceCandidate{ "0", "candidate:first 1 udp 2113937151 203.0.113.50 45000 typ host" });
    first.completed_mids.emplace("0");
    first.end_of_candidates = true;
    first.ice_credentials_at_session_level = false;

    WhipWhepSdpFrag second;
    second.ice_ufrag = "secondUfrag";
    second.ice_pwd = "secondPassword12345678901";
    second.ice_lite = false;
    second.ice_trickle = false;
    second.ice_renomination = true;
    second.ice_options = { "renomination" };
    second.ice_pacing = "75";
    second.bundle_mids = { "1" };
    second.media.emplace_back(WhipWhepSdpFragMedia{ "1", "video 9 UDP/TLS/RTP/SAVPF 96" });
    second.candidates.emplace_back(
        WhipWhepIceCandidate{ "1", "candidate:second 1 udp 2113937150 203.0.113.51 45001 typ host" });
    second.end_of_candidates = false;
    second.ice_credentials_at_session_level = true;

    const auto first_rendered = first.toString();
    const auto second_rendered = second.toString();
    first.swap(second);
    expect(first.toString() == second_rendered, "SDP fragment swap must move every second-side field to first");
    expect(second.toString() == first_rendered, "SDP fragment swap must move every first-side field to second");
}

void testWhipWhepIceRestartPreservesTrackAddresses() {
    RegisteredWebRtcTransportFixture fixture;
    const auto transport = fixture.transport();
    InspectableWebRtcTransport::TrackAddressSnapshot before;
    InspectableWebRtcTransport::TrackAddressSnapshot after;
    WhipWhepSdpFrag local_fragment;
    bool restarted = false;

    fixture.sync([&]() {
        negotiateAsLocalAnswer(*transport);
        transport->startWebRtcForTest();
        before = transport->trackAddressSnapshot(TrackVideo);
        transport->setNextIceRestartCredentials(
            "localRestartAddressUfrag", "localRestartAddressPassword1");
        const auto remote_fragment = makeRestartFragment(
            *transport->offerSdpForTest(), "remoteRestartAddressUfrag", "remoteRestartAddressPassword",
            "203.0.113.60", 46000, true);
        restarted = transport->restartWhipWhepIce(remote_fragment, local_fragment);
        after = transport->trackAddressSnapshot(TrackVideo);
    });

    expect(restarted, "a valid ICE restart should succeed");
    expect(before.offer && before.answer && before.media && before.plan_rtp,
           "the active video track snapshot must contain negotiated addresses");
    expect(after.offer == before.offer, "ICE restart must preserve the offer session address");
    expect(after.answer == before.answer, "ICE restart must preserve the answer session address");
    expect(after.media == before.media, "ICE restart must preserve the track media address");
    expect(after.plan_rtp == before.plan_rtp, "ICE restart must preserve the RTP codec-plan address");
    expect(after.plan_rtx == before.plan_rtx, "ICE restart must preserve the RTX codec-plan address");
    expect(!before.media->mid.empty(), "the preserved media pointer must remain dereferenceable after restart");
    expect(before.plan_rtp->pt == after.plan_rtp->pt,
           "the preserved RTP plan pointer must remain dereferenceable after restart");
    if (before.plan_rtx) {
        expect(before.plan_rtx->pt == after.plan_rtx->pt,
               "the preserved RTX plan pointer must remain dereferenceable after restart");
    }
    expect(local_fragment.ice_ufrag == "localRestartAddressUfrag",
           "restart response must advertise the committed local ICE generation");
}

void testWhipWhepIceRestartUsesRemoteSdpOwnership() {
    {
        RegisteredWebRtcTransportFixture fixture;
        const auto transport = fixture.transport();
        WhipWhepSdpFrag local_fragment;
        bool restarted = false;
        fixture.sync([&]() {
            negotiateAsLocalAnswer(*transport);
            const auto current_remote_fragment = makeCurrentCandidateFragment(*transport);
            const auto expected_local_candidates = mediaCandidateValues(
                transport->localMedia(transport->offerSdpForTest()->group.mids.front()));
            expect(transport->hasCurrentWhipWhepIceCredentials(current_remote_fragment),
                   "a local-answer transport must recognize credentials from its remote offer");
            expect(transport->applyWhipWhepIceFragment(current_remote_fragment),
                   "a local-answer transport must apply a fragment against its remote offer");
            expect(transport->hasAppliedRemoteCandidate()
                       && transport->lastAppliedRemoteCandidate()._ufrag == current_remote_fragment.ice_ufrag
                       && transport->lastAppliedRemoteCandidate()._pwd == current_remote_fragment.ice_pwd,
                   "local-answer trickle candidates must use remote-offer credentials");
            transport->setNextIceRestartCredentials(
                "localAnswerRestartUfrag", "localAnswerRestartPassword12");
            const auto remote_fragment = makeRestartFragment(
                *transport->offerSdpForTest(), "remoteOfferRestartUfrag", "remoteOfferRestartPassword12",
                "203.0.113.61", 46001, false);
            restarted = transport->restartWhipWhepIce(remote_fragment, local_fragment);
            expect(everyMediaUsesCredentials(
                       transport->offerSdpForTest(), "remoteOfferRestartUfrag", "remoteOfferRestartPassword12"),
                   "a local-answer restart must write remote credentials into the wire offer");
            expect(everyMediaUsesCredentials(
                       transport->answerSdp(), "localAnswerRestartUfrag", "localAnswerRestartPassword12"),
                   "a local-answer restart must write local credentials into the wire answer");
            expect(transport->iceUfragForTest() == "localAnswerRestartUfrag",
                   "ICE agent must use the local-answer restart generation");
            expect(transport->lastRestartRemoteCandidates().size() == 1
                       && transport->lastRestartRemoteCandidates()[0]._ufrag == "remoteOfferRestartUfrag"
                       && transport->lastRestartRemoteCandidates()[0]._pwd == "remoteOfferRestartPassword12",
                   "local-answer restart candidates must use remote-offer restart credentials");
            expect(fragmentCandidateValues(local_fragment) == expected_local_candidates,
                   "local-answer response candidates must come from the local answer SDP");
        });
        expect(restarted, "local-answer ownership restart should succeed");
        expect(local_fragment.ice_ufrag == "localAnswerRestartUfrag",
               "local-answer response fragment must use local answer credentials");
    }

    {
        RegisteredWebRtcTransportFixture fixture;
        const auto transport = fixture.transport();
        WhipWhepSdpFrag local_fragment;
        bool restarted = false;
        fixture.sync([&]() {
            negotiateAsLocalOffer(*transport);
            const auto current_remote_fragment = makeCurrentCandidateFragment(*transport);
            const auto expected_local_candidates = mediaCandidateValues(
                transport->localMedia(transport->offerSdpForTest()->group.mids.front()));
            expect(transport->hasCurrentWhipWhepIceCredentials(current_remote_fragment),
                   "a local-offer transport must recognize credentials from its remote answer");
            expect(transport->applyWhipWhepIceFragment(current_remote_fragment),
                   "a local-offer transport must apply a fragment against its remote answer");
            expect(transport->hasAppliedRemoteCandidate()
                       && transport->lastAppliedRemoteCandidate()._ufrag == current_remote_fragment.ice_ufrag
                       && transport->lastAppliedRemoteCandidate()._pwd == current_remote_fragment.ice_pwd,
                   "local-offer trickle candidates must use remote-answer credentials");
            transport->setNextIceRestartCredentials(
                "localOfferRestartUfrag", "localOfferRestartPassword123");
            const auto remote_fragment = makeRestartFragment(
                *transport->offerSdpForTest(), "remoteAnswerRestartUfrag", "remoteAnswerRestartPassword1",
                "203.0.113.62", 46002, false);
            restarted = transport->restartWhipWhepIce(remote_fragment, local_fragment);
            expect(everyMediaUsesCredentials(
                       transport->offerSdpForTest(), "localOfferRestartUfrag", "localOfferRestartPassword123"),
                   "a local-offer restart must write local credentials into the wire offer");
            expect(everyMediaUsesCredentials(
                       transport->answerSdp(), "remoteAnswerRestartUfrag", "remoteAnswerRestartPassword1"),
                   "a local-offer restart must write remote credentials into the wire answer");
            expect(transport->iceUfragForTest() == "localOfferRestartUfrag",
                   "ICE agent must use the local-offer restart generation");
            expect(transport->lastRestartRemoteCandidates().size() == 1
                       && transport->lastRestartRemoteCandidates()[0]._ufrag == "remoteAnswerRestartUfrag"
                       && transport->lastRestartRemoteCandidates()[0]._pwd == "remoteAnswerRestartPassword1",
                   "local-offer restart candidates must use remote-answer restart credentials");
            expect(fragmentCandidateValues(local_fragment) == expected_local_candidates,
                   "local-offer response candidates must come from the local offer SDP");
        });
        expect(restarted, "local-offer ownership restart should succeed");
        expect(local_fragment.ice_ufrag == "localOfferRestartUfrag",
               "local-offer response fragment must use local offer credentials");
    }
}

void testWhipWhepIceRestartFailureIsAtomic() {
    RegisteredWebRtcTransportFixture fixture;
    RegisteredWebRtcTransportFixture blocker;
    const auto transport = fixture.transport();
    const auto blocker_transport = blocker.transport();
    IceStateSnapshot baseline;
    string original_ufrag;
    string blocker_ufrag;

    fixture.sync([&]() {
        negotiateAsLocalAnswer(*transport);
        const auto current_fragment = makeCurrentCompletedFragment(*transport);
        expect(transport->applyWhipWhepIceFragment(current_fragment),
               "atomicity setup should install a completed MID state");
        baseline = captureIceState(*transport);
        original_ufrag = transport->iceUfragForTest();
    });
    blocker.sync([&]() { blocker_ufrag = blocker_transport->iceUfragForTest(); });
    expect(blocker_ufrag != original_ufrag, "manager collision fixture must use a distinct ICE ufrag");

    auto assert_unchanged = [&](const string &context) {
        IceStateSnapshot actual;
        fixture.sync([&]() { actual = captureIceState(*transport); });
        expectIceStateEquals(actual, baseline, context);
        expect(WebRtcTransportManager::Instance().getItemByIceUfrag(original_ufrag).get() == transport.get(),
               context + ": manager lost the original ufrag registration");
    };

    WhipWhepSdpFrag response;
    response.ice_ufrag = "unchanged-response";
    bool restarted = true;
    fixture.sync([&]() {
        auto fragment = makeRestartFragment(
            *transport->offerSdpForTest(), "unknownMidRemoteUfrag", "unknownMidRemotePassword123",
            "203.0.113.63", 46003, false);
        fragment.candidates[0].mid = "unknown-mid";
        restarted = transport->restartWhipWhepIce(fragment, response);
    });
    expect(!restarted, "an ICE restart with an unknown MID must fail");
    expect(response.ice_ufrag == "unchanged-response", "failed unknown-MID restart must not publish a response");
    assert_unchanged("unknown MID failure");

    fixture.sync([&]() {
        auto fragment = makeRestartFragment(
            *transport->offerSdpForTest(), "invalidCandidateUfrag", "invalidCandidatePassword1234",
            "203.0.113.64", 46004, false);
        fragment.candidates[0].value = "candidate:malformed";
        restarted = transport->restartWhipWhepIce(fragment, response);
    });
    expect(!restarted, "an ICE restart with a malformed candidate must fail");
    expect(response.ice_ufrag == "unchanged-response", "failed candidate restart must not publish a response");
    assert_unchanged("malformed candidate failure");

    const string failed_agent_ufrag = "failedAgentLocalUfrag";
    fixture.sync([&]() {
        transport->setNextIceRestartCredentials(failed_agent_ufrag, "failedAgentLocalPassword123");
        transport->failNextIceAgentRestart();
        const auto fragment = makeRestartFragment(
            *transport->offerSdpForTest(), "failedAgentRemoteUfrag", "failedAgentRemotePassword123",
            "203.0.113.66", 46006, false);
        restarted = transport->restartWhipWhepIce(fragment, response);
    });
    expect(!restarted, "an injected ICE agent restart failure must fail the transport restart");
    expect(response.ice_ufrag == "unchanged-response", "agent failure must not publish a response fragment");
    assert_unchanged("ICE agent failure");
    expect(!WebRtcTransportManager::Instance().getItemByIceUfrag(failed_agent_ufrag),
           "agent failure rollback must remove the prepared new manager registration");

    fixture.sync([&]() {
        transport->setNextIceRestartCredentials(blocker_ufrag, "managerCollisionPassword123");
        const auto fragment = makeRestartFragment(
            *transport->offerSdpForTest(), "managerCollisionRemote", "managerCollisionRemotePassword",
            "203.0.113.65", 46005, false);
        restarted = transport->restartWhipWhepIce(fragment, response);
    });
    expect(!restarted, "an ICE restart whose local ufrag collides in the manager must fail");
    expect(response.ice_ufrag == "unchanged-response", "manager collision must not publish a response fragment");
    assert_unchanged("manager collision failure");
    expect(WebRtcTransportManager::Instance().getItemByIceUfrag(blocker_ufrag).get() == blocker_transport.get(),
           "manager collision must preserve the existing transport registration");
}

void testWhipWhepRepeatedIceRestartRotatesOneGenerationAtATime() {
    RegisteredWebRtcTransportFixture fixture;
    const auto transport = fixture.transport();
    string initial_local_ufrag;
    string offerer_tag;
    WhipWhepSdpFrag first_local_fragment;
    WhipWhepSdpFrag second_local_fragment;
    Json::Value first_checklist;
    Json::Value second_checklist;
    set<string> first_completed;
    set<string> second_completed;
    bool first_restarted = false;
    bool second_restarted = false;

    fixture.sync([&]() {
        negotiateAsLocalAnswer(*transport);
        initial_local_ufrag = transport->iceUfragForTest();
        offerer_tag = transport->offerSdpForTest()->group.mids.front();

        transport->setNextIceRestartCredentials(
            "repeatedLocalGenerationOne", "repeatedLocalPasswordOne12");
        const auto first_remote = makeRestartFragment(
            *transport->offerSdpForTest(), "repeatedRemoteGenerationOne", "repeatedRemotePasswordOne1",
            "203.0.113.71", 47001, true);
        first_restarted = transport->restartWhipWhepIce(first_remote, first_local_fragment);
        first_checklist = transport->iceChecklistForTest();
        first_completed = transport->completedMidsForTest();

        transport->setNextIceRestartCredentials(
            "repeatedLocalGenerationTwo", "repeatedLocalPasswordTwo12");
        const auto second_remote = makeRestartFragment(
            *transport->offerSdpForTest(), "repeatedRemoteGenerationTwo", "repeatedRemotePasswordTwo1",
            "203.0.113.72", 47002, false);
        second_restarted = transport->restartWhipWhepIce(second_remote, second_local_fragment);
        second_checklist = transport->iceChecklistForTest();
        second_completed = transport->completedMidsForTest();

        expect(everyMediaUsesCredentials(
                   transport->remoteSdp(), "repeatedRemoteGenerationTwo", "repeatedRemotePasswordTwo1"),
               "second restart must replace remote SDP credentials from the first generation");
        expect(everyMediaUsesCredentials(
                   transport->localSdp(), "repeatedLocalGenerationTwo", "repeatedLocalPasswordTwo12"),
               "second restart must replace local SDP credentials from the first generation");
        expect(transport->iceUfragForTest() == "repeatedLocalGenerationTwo",
               "ICE agent must expose only the second local generation");
    });

    expect(first_restarted && second_restarted, "two consecutive valid ICE restarts should both succeed");
    expect(first_local_fragment.ice_ufrag == "repeatedLocalGenerationOne",
           "first response must retain its own local generation");
    expect(second_local_fragment.ice_ufrag == "repeatedLocalGenerationTwo",
           "second response must advertise only the second local generation");
    expect(first_completed.count(offerer_tag) == 1, "first generation should record its completed MID");
    expect(second_completed.empty(), "second generation must not retain completed MIDs from the first");
    expect(first_checklist["remote_candidates_count"].asUInt64() == 1
               && checklistHasRemoteHost(first_checklist, "203.0.113.71"),
           "first ICE agent generation should contain only its remote candidate");
    expect(second_checklist["remote_candidates_count"].asUInt64() == 1
               && checklistHasRemoteHost(second_checklist, "203.0.113.72")
               && !checklistHasRemoteHost(second_checklist, "203.0.113.71"),
           "second ICE agent generation must replace first-generation remote candidates");
    expect(!WebRtcTransportManager::Instance().getItemByIceUfrag(initial_local_ufrag),
           "manager must remove the pre-restart local generation");
    expect(!WebRtcTransportManager::Instance().getItemByIceUfrag("repeatedLocalGenerationOne"),
           "manager must remove the first restarted generation after the second restart");
    expect(WebRtcTransportManager::Instance().getItemByIceUfrag("repeatedLocalGenerationTwo").get() == transport.get(),
           "manager must register only the second restarted generation");
}

} // namespace

int main() {
    try {
        testBundleOnlyDatachannelAnswer();
        testWhipWhepOfferConstraints();
        testDeleteWebrtcLocationQueryRoundTrip();
        testStandardWhipWhepEndpointUrls();
        testLegacyWebrtcUrlStillMapsToBuiltinEndpoint();
        testWhipWhepRequesterHeaderApiIsNarrow();
        testWhipWhepRequesterRejectsRoutingHeaders();
        testWhipWhepRequesterLifecycleIsCoherent();
        testWhipWhepRequesterMarksEveryCustomHeaderAsCredential();
        testWhipWhepRequesterRedirectPolicy();
        testWhipWhepRequesterUrlCredentialPolicy();
        testWhipWhepRequesterSynchronousStartFailure();
        testWhipWhepSessionLocationPolicy();
        testWhipWhepPolicyErrorsRedactCredentialMaterial();
        testClientOffer201UsesRemoteAnswer();
        testWhep406CounterOfferUsesRemoteOffer();
        testWhipWhepSdpFragSwapIsComplete();
        testWhipWhepIceRestartPreservesTrackAddresses();
        testWhipWhepIceRestartUsesRemoteSdpOwnership();
        testWhipWhepIceRestartFailureIsAtomic();
        testWhipWhepRepeatedIceRestartRotatesOneGenerationAtATime();
        releaseOpenSslThreadStateForLeakSanitizer();
        cout << "test_webrtc_regression passed" << endl;
        return 0;
    } catch (const exception &ex) {
        releaseOpenSslThreadStateForLeakSanitizer();
        cerr << "test_webrtc_regression failed: " << ex.what() << endl;
        return EXIT_FAILURE;
    }
}
