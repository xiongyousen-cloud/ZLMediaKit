/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "WhipWhepProtocol.h"

#include "Network/sockutil.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace std;

namespace mediakit {

namespace {

string trimCopy(string value) {
    const auto not_space = [](unsigned char ch) { return !isspace(ch); };
    auto begin = find_if(value.begin(), value.end(), not_space);
    auto end = find_if(value.rbegin(), value.rend(), not_space).base();
    if (begin >= end) {
        return "";
    }
    return string(begin, end);
}

vector<string> splitWhitespace(const string &value) {
    vector<string> result;
    size_t begin = 0;
    while (begin < value.size()) {
        while (begin < value.size() && isspace(static_cast<unsigned char>(value[begin]))) {
            ++begin;
        }
        if (begin == value.size()) {
            break;
        }
        auto end = begin;
        while (end < value.size() && !isspace(static_cast<unsigned char>(value[end]))) {
            ++end;
        }
        result.emplace_back(value.substr(begin, end - begin));
        begin = end;
    }
    return result;
}

bool isUnsignedInteger(const string &value, uint64_t upper_bound, bool allow_zero) {
    if (value.empty()) {
        return false;
    }
    uint64_t result = 0;
    for (const auto ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<uint64_t>(ch - '0');
        if (result > (upper_bound - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    return allow_zero || result != 0;
}

bool equalsIgnoreCase(const string &left, const string &right) {
    if (left.size() != right.size()) {
        return false;
    }
    return equal(left.begin(), left.end(), right.begin(), [](unsigned char lhs, unsigned char rhs) {
        return tolower(lhs) == tolower(rhs);
    });
}

bool startsWithIgnoreCase(const string &value, const string &prefix) {
    return value.size() >= prefix.size()
        && equal(prefix.begin(), prefix.end(), value.begin(), [](unsigned char lhs, unsigned char rhs) {
               return tolower(lhs) == tolower(rhs);
           });
}

void appendUnique(vector<string> &values, const string &value) {
    if (find(values.begin(), values.end(), value) == values.end()) {
        values.emplace_back(value);
    }
}

void validateCandidate(const string &candidate) {
    static const string kCandidatePrefix = "candidate:";
    if (!startsWithIgnoreCase(candidate, kCandidatePrefix)) {
        throw invalid_argument("SDP fragment candidate is missing candidate: prefix");
    }

    const auto fields = splitWhitespace(candidate.substr(kCandidatePrefix.size()));
    const auto valid_ice_token = [](const string &value) {
        return !value.empty() && all_of(value.begin(), value.end(), [](unsigned char ch) {
            return isalnum(ch) || ch == '+' || ch == '/';
        });
    };
    if (fields.size() < 8 || fields[0].size() > 32 || !valid_ice_token(fields[0])
        || !isUnsignedInteger(fields[1], 256, false) || fields[2].empty()
        || !isUnsignedInteger(fields[3], numeric_limits<int32_t>::max(), false) || fields[4].empty()
        || !isUnsignedInteger(fields[5], 65535, false) || !equalsIgnoreCase(fields[6], "typ") || fields[7].empty()) {
        throw invalid_argument("malformed ICE candidate in SDP fragment");
    }

    if ((fields.size() - 8) % 2 != 0) {
        throw invalid_argument("malformed ICE candidate extension in SDP fragment");
    }
}

void validateIceCredential(const string &value, size_t minimum_size, const char *name) {
    if (value.size() < minimum_size || value.size() > 256
        || !all_of(value.begin(), value.end(), [](unsigned char ch) {
               return isalnum(ch) || ch == '+' || ch == '/';
           })) {
        throw invalid_argument(string("SDP fragment contains an invalid ") + name);
    }
}

void validateMid(const string &mid) {
    if (mid.empty() || mid.find_first_of(" \t\r\n") != string::npos) {
        throw invalid_argument("SDP fragment contains an invalid mid");
    }
}

void validateMediaLine(const string &media_line) {
    const auto fields = splitWhitespace(media_line);
    if (fields.size() < 4) {
        throw invalid_argument("SDP fragment contains a malformed pseudo media line");
    }
}

void setIceCredential(string &target, const string &value, size_t minimum_size, const char *name) {
    validateIceCredential(value, minimum_size, name);
    if (!target.empty() && target != value) {
        throw invalid_argument(string("SDP fragment contains conflicting ") + name + " values");
    }
    target = value;
}

string lowerCopy(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    return value;
}

bool isUriScheme(const string &value) {
    return !value.empty() && isalpha(static_cast<unsigned char>(value.front()))
        && all_of(value.begin() + 1, value.end(), [](unsigned char ch) {
               return isalnum(ch) || ch == '+' || ch == '-' || ch == '.';
           });
}

struct UriReference {
    string scheme;
    string authority;
    string path;
    string query;
    bool has_authority = false;
    bool has_query = false;
    bool has_fragment = false;
};

struct UriOrigin {
    string scheme;
    string host;
    uint16_t port = 0;
    bool has_userinfo = false;
};

UriReference parseUriReference(const string &value) {
    UriReference result;
    const auto fragment = value.find('#');
    result.has_fragment = fragment != string::npos;
    auto reference = value.substr(0, fragment);
    const auto query = reference.find('?');
    if (query != string::npos) {
        result.query = reference.substr(query + 1);
        result.has_query = true;
        reference.erase(query);
    }

    const auto colon = reference.find(':');
    const auto delimiter = reference.find_first_of("/");
    if (colon != string::npos && (delimiter == string::npos || colon < delimiter)) {
        result.scheme = reference.substr(0, colon);
        if (!isUriScheme(result.scheme)) {
            throw invalid_argument("invalid WHIP/WHEP URI scheme");
        }
        reference.erase(0, colon + 1);
    }
    if (reference.compare(0, 2, "//") == 0) {
        result.has_authority = true;
        reference.erase(0, 2);
        const auto path = reference.find('/');
        result.authority = path == string::npos ? reference : reference.substr(0, path);
        result.path = path == string::npos ? string() : reference.substr(path);
    } else {
        result.path = reference;
    }
    return result;
}

UriOrigin parseHttpOrigin(const UriReference &reference) {
    if (reference.scheme.empty() || !reference.has_authority || reference.authority.empty()) {
        throw invalid_argument("invalid WHIP/WHEP URL");
    }

    UriOrigin result;
    result.scheme = lowerCopy(reference.scheme);
    if (result.scheme != "http" && result.scheme != "https") {
        throw invalid_argument("invalid WHIP/WHEP URL");
    }
    if (reference.authority.find_first_of(" \t\r\n") != string::npos) {
        throw invalid_argument("invalid WHIP/WHEP URL");
    }

    auto host_port = reference.authority;
    const auto userinfo_end = host_port.rfind('@');
    if (userinfo_end != string::npos) {
        result.has_userinfo = true;
        host_port.erase(0, userinfo_end + 1);
    }

    string port;
    if (!host_port.empty() && host_port.front() == '[') {
        const auto bracket = host_port.find(']');
        if (bracket == string::npos || bracket == 1) {
            throw invalid_argument("invalid WHIP/WHEP URL");
        }
        const auto ipv6 = host_port.substr(1, bracket - 1);
        if (!toolkit::SockUtil::is_ipv6(ipv6.c_str())) {
            throw invalid_argument("invalid WHIP/WHEP URL");
        }
        const auto address = toolkit::SockUtil::make_sockaddr(ipv6.c_str(), 0);
        if (address.ss_family != AF_INET6) {
            throw invalid_argument("invalid WHIP/WHEP URL");
        }
        const auto *address6 = reinterpret_cast<const sockaddr_in6 *>(&address);
        result.host = "[" + lowerCopy(toolkit::SockUtil::inet_ntoa(address6->sin6_addr)) + "]";
        if (bracket + 1 < host_port.size()) {
            if (host_port[bracket + 1] != ':') {
                throw invalid_argument("invalid WHIP/WHEP URL");
            }
            port = host_port.substr(bracket + 2);
        }
    } else {
        const auto colon = host_port.rfind(':');
        if (colon != string::npos) {
            result.host = host_port.substr(0, colon);
            port = host_port.substr(colon + 1);
        } else {
            result.host = host_port;
        }
        if (result.host.find(':') != string::npos) {
            throw invalid_argument("invalid WHIP/WHEP URL");
        }
    }
    if (result.host.empty() || result.host.find('@') != string::npos
        || (result.host.front() != '[' && result.host.find_first_of("[]") != string::npos)) {
        throw invalid_argument("invalid WHIP/WHEP URL");
    }
    result.host = lowerCopy(result.host);

    if (port.empty()) {
        result.port = result.scheme == "https" ? 443 : 80;
    } else {
        if (!isUnsignedInteger(port, 65535, false)) {
            throw invalid_argument("invalid WHIP/WHEP URL");
        }
        result.port = static_cast<uint16_t>(strtoul(port.c_str(), nullptr, 10));
    }
    return result;
}

string canonicalOriginImpl(const UriOrigin &origin) {
    return origin.scheme + "://" + origin.host + ":" + to_string(origin.port);
}

void removeLastPathSegment(string &path) {
    const auto slash = path.find_last_of('/');
    if (slash == string::npos) {
        path.clear();
    } else {
        path.erase(slash);
    }
}

string removeDotSegments(string path) {
    string result;
    while (!path.empty()) {
        if (path.compare(0, 3, "../") == 0) {
            path.erase(0, 3);
        } else if (path.compare(0, 2, "./") == 0) {
            path.erase(0, 2);
        } else if (path.compare(0, 3, "/./") == 0) {
            path.erase(0, 2);
        } else if (path == "/.") {
            path = "/";
        } else if (path.compare(0, 4, "/../") == 0) {
            path.erase(0, 3);
            removeLastPathSegment(result);
        } else if (path == "/..") {
            path = "/";
            removeLastPathSegment(result);
        } else if (path == "." || path == "..") {
            path.clear();
        } else {
            const auto segment_end = path.front() == '/' ? path.find('/', 1) : path.find('/');
            if (segment_end == string::npos) {
                result += path;
                path.clear();
            } else {
                result.append(path, 0, segment_end);
                path.erase(0, segment_end);
            }
        }
    }
    return result;
}

string makeAbsoluteUri(const string &scheme,
                       const string &authority,
                       const string &path,
                       bool has_query,
                       const string &query) {
    auto result = scheme + "://" + authority + path;
    if (has_query) {
        result += "?" + query;
    }
    return result;
}

} // namespace

WhipWhepSdpFrag WhipWhepSdpFrag::parse(const string &sdp_frag) {
    WhipWhepSdpFrag result;
    string current_media_line;
    string current_mid;
    string session_ice_ufrag;
    string session_ice_pwd;
    string media_ice_ufrag;
    string media_ice_pwd;
    bool saw_media = false;
    size_t begin = 0;

    while (begin < sdp_frag.size()) {
        auto end = sdp_frag.find('\n', begin);
        if (end == string::npos) {
            end = sdp_frag.size();
        }
        auto line = trimCopy(sdp_frag.substr(begin, end - begin));
        begin = end == sdp_frag.size() ? end : end + 1;
        if (line.empty()) {
            continue;
        }

        if (line.compare(0, 2, "m=") == 0) {
            if (saw_media && current_mid.empty()) {
                throw invalid_argument("SDP fragment pseudo media line has no mid");
            }
            current_media_line = line.substr(2);
            validateMediaLine(current_media_line);
            current_mid.clear();
            saw_media = true;
            continue;
        }
        if (equalsIgnoreCase(line, "a=ice-lite")) {
            result.ice_lite = true;
            continue;
        }
        static const string kIceOptionsPrefix = "a=ice-options:";
        if (startsWithIgnoreCase(line, kIceOptionsPrefix)) {
            const auto options = splitWhitespace(line.substr(kIceOptionsPrefix.size()));
            if (options.empty()) {
                throw invalid_argument("SDP fragment contains empty ice-options");
            }
            for (const auto &option : options) {
                validateIceCredential(option, 1, "ice-option");
                if (equalsIgnoreCase(option, "trickle")) {
                    result.ice_trickle = true;
                } else if (equalsIgnoreCase(option, "renomination")) {
                    result.ice_renomination = true;
                }
                appendUnique(result.ice_options, option);
            }
            continue;
        }
        static const string kIcePacingPrefix = "a=ice-pacing:";
        if (startsWithIgnoreCase(line, kIcePacingPrefix)) {
            const auto value = line.substr(kIcePacingPrefix.size());
            if (!isUnsignedInteger(value, UINT64_C(9999999999), false)) {
                throw invalid_argument("SDP fragment contains invalid ice-pacing");
            }
            if (!result.ice_pacing.empty() && result.ice_pacing != value) {
                throw invalid_argument("SDP fragment contains conflicting ice-pacing values");
            }
            result.ice_pacing = value;
            continue;
        }
        static const string kBundlePrefix = "a=group:BUNDLE";
        if (startsWithIgnoreCase(line, kBundlePrefix)) {
            const auto mids = splitWhitespace(line.substr(kBundlePrefix.size()));
            if (mids.empty()) {
                throw invalid_argument("SDP fragment contains an empty BUNDLE group");
            }
            for (const auto &mid : mids) {
                validateMid(mid);
            }
            if (!result.bundle_mids.empty() && result.bundle_mids != mids) {
                throw invalid_argument("SDP fragment contains conflicting BUNDLE groups");
            }
            result.bundle_mids = mids;
            continue;
        }
        if (startsWithIgnoreCase(line, "a=ice-ufrag:")) {
            auto &target = saw_media ? media_ice_ufrag : session_ice_ufrag;
            setIceCredential(target, line.substr(12), 4, "ice-ufrag");
            continue;
        }
        if (startsWithIgnoreCase(line, "a=ice-pwd:")) {
            auto &target = saw_media ? media_ice_pwd : session_ice_pwd;
            setIceCredential(target, line.substr(10), 22, "ice-pwd");
            continue;
        }
        if (startsWithIgnoreCase(line, "a=mid:")) {
            if (!saw_media || current_media_line.empty()) {
                throw invalid_argument("SDP fragment mid has no pseudo media line");
            }
            if (!current_mid.empty()) {
                throw invalid_argument("SDP fragment pseudo media line contains multiple mids");
            }
            current_mid = line.substr(6);
            validateMid(current_mid);
            const auto duplicate = find_if(result.media.begin(), result.media.end(), [&current_mid](const WhipWhepSdpFragMedia &media) {
                return media.mid == current_mid;
            });
            if (duplicate != result.media.end()) {
                throw invalid_argument("SDP fragment contains a duplicate mid");
            }
            result.media.emplace_back(WhipWhepSdpFragMedia{ current_mid, current_media_line });
            continue;
        }
        if (startsWithIgnoreCase(line, "a=candidate:")) {
            if (current_mid.empty()) {
                throw invalid_argument("SDP fragment candidate has no associated mid");
            }
            const auto candidate = string("candidate:") + line.substr(12);
            validateCandidate(candidate);
            result.candidates.emplace_back(WhipWhepIceCandidate{ current_mid, candidate });
            continue;
        }
        if (line == "a=end-of-candidates") {
            if (!saw_media) {
                result.end_of_candidates = true;
            } else {
                if (current_mid.empty()) {
                    throw invalid_argument("SDP fragment end-of-candidates has no associated mid");
                }
                result.completed_mids.emplace(current_mid);
            }
            continue;
        }

        // RFC 8840 为未来扩展保留属性，并要求接收端忽略无法识别的属性。
        if (startsWithIgnoreCase(line, "a=")) {
            continue;
        }
        throw invalid_argument("unsupported SDP fragment field: " + line);
    }

    if (saw_media && current_mid.empty()) {
        throw invalid_argument("SDP fragment pseudo media line has no mid");
    }
    if (session_ice_ufrag.empty() != session_ice_pwd.empty()
        || media_ice_ufrag.empty() != media_ice_pwd.empty()) {
        throw invalid_argument("SDP fragment must contain both ice-ufrag and ice-pwd at the same level");
    }
    if (!media_ice_ufrag.empty()) {
        result.ice_ufrag = std::move(media_ice_ufrag);
        result.ice_pwd = std::move(media_ice_pwd);
        result.ice_credentials_at_session_level = false;
    } else {
        result.ice_ufrag = std::move(session_ice_ufrag);
        result.ice_pwd = std::move(session_ice_pwd);
        result.ice_credentials_at_session_level = true;
    }
    if (!result.hasIceRestartCredentials()) {
        throw invalid_argument("SDP fragment must contain current ICE credentials");
    }
    return result;
}

void WhipWhepSdpFrag::swap(WhipWhepSdpFrag &other) noexcept {
    using std::swap;
    ice_ufrag.swap(other.ice_ufrag);
    ice_pwd.swap(other.ice_pwd);
    swap(ice_lite, other.ice_lite);
    swap(ice_trickle, other.ice_trickle);
    swap(ice_renomination, other.ice_renomination);
    ice_options.swap(other.ice_options);
    ice_pacing.swap(other.ice_pacing);
    bundle_mids.swap(other.bundle_mids);
    media.swap(other.media);
    candidates.swap(other.candidates);
    completed_mids.swap(other.completed_mids);
    swap(end_of_candidates, other.end_of_candidates);
    swap(ice_credentials_at_session_level, other.ice_credentials_at_session_level);
}

string WhipWhepSdpFrag::toString() const {
    if (!hasIceRestartCredentials()) {
        throw invalid_argument("SDP fragment must contain current ICE credentials");
    }
    validateIceCredential(ice_ufrag, 4, "ice-ufrag");
    validateIceCredential(ice_pwd, 22, "ice-pwd");

    string result;
    if (ice_credentials_at_session_level) {
        result += "a=ice-ufrag:" + ice_ufrag + "\r\n";
        result += "a=ice-pwd:" + ice_pwd + "\r\n";
    }
    if (ice_lite) {
        result += "a=ice-lite\r\n";
    }
    auto options = ice_options;
    if (ice_trickle) {
        appendUnique(options, "trickle");
    }
    if (ice_renomination) {
        appendUnique(options, "renomination");
    }
    if (!options.empty()) {
        result += "a=ice-options:";
        for (size_t i = 0; i < options.size(); ++i) {
            if (i) {
                result += " ";
            }
            result += options[i];
        }
        result += "\r\n";
    }
    if (!ice_pacing.empty()) {
        if (!isUnsignedInteger(ice_pacing, UINT64_C(9999999999), false)) {
            throw invalid_argument("SDP fragment contains invalid ice-pacing");
        }
        result += "a=ice-pacing:" + ice_pacing + "\r\n";
    }
    if (!bundle_mids.empty()) {
        result += "a=group:BUNDLE";
        for (const auto &mid : bundle_mids) {
            if (mid.empty() || mid.find_first_of(" \t\r\n") != string::npos) {
                throw invalid_argument("SDP fragment BUNDLE group contains an invalid mid");
            }
            result += " " + mid;
        }
        result += "\r\n";
    }
    if (end_of_candidates) {
        result += "a=end-of-candidates\r\n";
    }

    vector<string> mids;
    set<string> seen_mids;
    for (const auto &description : media) {
        validateMid(description.mid);
        validateMediaLine(description.value);
        if (!seen_mids.emplace(description.mid).second) {
            throw invalid_argument("SDP fragment contains a duplicate mid");
        }
        mids.emplace_back(description.mid);
    }
    for (const auto &candidate : candidates) {
        validateMid(candidate.mid);
        validateCandidate(candidate.value);
        if (seen_mids.emplace(candidate.mid).second) {
            mids.emplace_back(candidate.mid);
        }
    }
    for (const auto &mid : completed_mids) {
        validateMid(mid);
        if (seen_mids.emplace(mid).second) {
            mids.emplace_back(mid);
        }
    }
    if (!ice_credentials_at_session_level && mids.empty()) {
        throw invalid_argument("media-level ICE credentials require a pseudo media line");
    }

    for (const auto &mid : mids) {
        auto description = find_if(media.begin(), media.end(), [&mid](const WhipWhepSdpFragMedia &item) {
            return item.mid == mid;
        });
        result += "m=" + (description == media.end() ? string("audio 9 RTP/AVP 0") : description->value) + "\r\n";
        result += "a=mid:" + mid + "\r\n";
        if (!ice_credentials_at_session_level) {
            result += "a=ice-ufrag:" + ice_ufrag + "\r\n";
            result += "a=ice-pwd:" + ice_pwd + "\r\n";
        }
        for (const auto &candidate : candidates) {
            if (candidate.mid == mid) {
                result += "a=" + candidate.value + "\r\n";
            }
        }
        if (completed_mids.count(mid)) {
            result += "a=end-of-candidates\r\n";
        }
    }
    return result;
}

string WhipWhepProtocol::normalizeMediaType(const string &content_type) {
    auto result = content_type.substr(0, content_type.find(';'));
    result = trimCopy(std::move(result));
    return lowerCopy(std::move(result));
}

string WhipWhepProtocol::canonicalOrigin(const string &url) {
    return canonicalOriginImpl(parseHttpOrigin(parseUriReference(url)));
}

bool WhipWhepProtocol::hasUrlUserInfo(const string &url) {
    const auto reference = parseUriReference(url);
    return reference.has_authority && reference.authority.rfind('@') != string::npos;
}

set<string> WhipWhepProtocol::parseTrustedOrigins(const string &origins) {
    set<string> result;
    if (trimCopy(origins).empty()) {
        return result;
    }

    size_t begin = 0;
    while (begin <= origins.size()) {
        const auto end = origins.find(',', begin);
        const auto item = trimCopy(origins.substr(begin, end == string::npos ? string::npos : end - begin));
        if (item.empty()) {
            throw invalid_argument("WHIP/WHEP trusted origins contain an empty item");
        }
        const auto reference = parseUriReference(item);
        const auto origin = parseHttpOrigin(reference);
        if (!reference.path.empty() || reference.has_query || reference.has_fragment || origin.has_userinfo) {
            throw invalid_argument("WHIP/WHEP trusted origins must contain only origins");
        }
        result.emplace(canonicalOriginImpl(origin));
        if (end == string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

string WhipWhepProtocol::resolveSessionUrl(const string &request_url, const string &location) {
    const auto trimmed_location = trimCopy(location);
    if (trimmed_location.empty() || trimmed_location.find_first_of("\r\n") != string::npos) {
        throw invalid_argument("WHIP/WHEP response is missing the Location header");
    }

    const auto base = parseUriReference(request_url);
    const auto base_origin = parseHttpOrigin(base);
    const auto reference = parseUriReference(trimmed_location);
    if (reference.scheme.empty() && !reference.has_authority && reference.path.empty() && !reference.has_query) {
        throw invalid_argument("WHIP/WHEP Location contains an empty URI-reference");
    }

    UriReference target;
    if (!reference.scheme.empty()) {
        target = reference;
    } else {
        target.scheme = base.scheme;
        if (reference.has_authority) {
            target.authority = reference.authority;
            target.has_authority = true;
            target.path = removeDotSegments(reference.path);
            target.query = reference.query;
            target.has_query = reference.has_query;
        } else {
            target.authority = base.authority;
            target.has_authority = true;
            if (reference.path.empty()) {
                target.path = base.path;
                target.query = reference.has_query ? reference.query : base.query;
                target.has_query = reference.has_query || base.has_query;
            } else {
                if (reference.path.front() == '/') {
                    target.path = removeDotSegments(reference.path);
                } else {
                    const auto base_path = base.path.empty() ? string("/") : base.path;
                    const auto directory_end = base_path.rfind('/');
                    const auto directory = directory_end == string::npos ? string("/")
                                                                        : base_path.substr(0, directory_end + 1);
                    target.path = removeDotSegments(directory + reference.path);
                }
                target.query = reference.query;
                target.has_query = reference.has_query;
            }
        }
    }

    const auto target_origin = parseHttpOrigin(target);
    if (base_origin.has_userinfo && !target_origin.has_userinfo
        && canonicalOriginImpl(base_origin) == canonicalOriginImpl(target_origin)) {
        const auto userinfo_end = base.authority.rfind('@');
        target.authority = base.authority.substr(0, userinfo_end + 1) + target.authority;
    }
    return makeAbsoluteUri(target.scheme, target.authority, target.path, target.has_query, target.query);
}

bool WhipWhepProtocol::isTargetAllowed(const string &source_url,
                                       const string &target_url,
                                       bool has_custom_headers,
                                       const set<string> &trusted_origins,
                                       string &reason) {
    reason.clear();
    UriOrigin target;
    try {
        target = parseHttpOrigin(parseUriReference(target_url));
    } catch (const exception &) {
        reason = "invalid WHIP/WHEP target URL";
        return false;
    }
    const auto target_canonical_origin = canonicalOriginImpl(target);

    UriOrigin source;
    try {
        source = parseHttpOrigin(parseUriReference(source_url));
    } catch (const exception &) {
        reason = "invalid WHIP/WHEP source URL";
        return false;
    }
    if (source.scheme == "https" && target.scheme == "http") {
        reason = "WHIP/WHEP target is not allowed: " + target_canonical_origin;
        return false;
    }
    if (canonicalOriginImpl(source) == target_canonical_origin) {
        return true;
    }
    if (!has_custom_headers && !source.has_userinfo && !target.has_userinfo) {
        return true;
    }
    if (trusted_origins.count(target_canonical_origin)) {
        return true;
    }
    reason = "WHIP/WHEP target is not trusted: " + target_canonical_origin;
    return false;
}

bool WhipWhepProtocol::isSdpContentType(const string &content_type) {
    return WhipWhepProtocol::normalizeMediaType(content_type) == "application/sdp";
}

bool WhipWhepProtocol::isTrickleIceSdpFragContentType(const string &content_type) {
    return WhipWhepProtocol::normalizeMediaType(content_type) == "application/trickle-ice-sdpfrag";
}

WhipWhepSession::WhipWhepSession(string etag, shared_ptr<WhipWhepIceTransport> transport, EtagFactory next_etag)
    : _etag(std::move(etag)), _transport(std::move(transport)), _next_etag(std::move(next_etag)) {
    if (_etag.empty()) {
        throw invalid_argument("WHIP/WHEP session requires a strong ETag");
    }
    if (!_transport) {
        throw invalid_argument("WHIP/WHEP session requires an ICE transport");
    }
}

WhipWhepPatchResult WhipWhepSession::applyPatch(const WhipWhepSdpFrag &fragment, const string &if_match) {
    lock_guard<mutex> lock(_mtx);
    WhipWhepPatchResult result;
    if (_closed) {
        result.status = WhipWhepPatchStatus::Closed;
        return result;
    }

    if (if_match.empty()) {
        result.status = WhipWhepPatchStatus::PreconditionRequired;
        return result;
    }

    // RFC 9725 以替换凭据定义 ICE 重启，If-Match: * 是该操作的必要前置条件；
    // 普通 trickle fragment 可以重复当前凭据。
    const bool ice_restart = fragment.hasIceRestartCredentials()
        && !_transport->hasCurrentIceCredentials(fragment);
    if (ice_restart) {
        if (if_match != "*") {
            result.status = WhipWhepPatchStatus::PreconditionFailed;
            return result;
        }

        const auto next_etag = _next_etag ? _next_etag() : string();
        if (next_etag.empty() || next_etag == _etag) {
            throw runtime_error("WHIP/WHEP ICE restart did not produce a new ETag");
        }

        WhipWhepSdpFrag local_fragment;
        if (!_transport->restartIce(fragment, local_fragment)) {
            result.status = WhipWhepPatchStatus::Unsupported;
            return result;
        }

        _etag = next_etag;
        result.status = WhipWhepPatchStatus::Restarted;
        result.etag = _etag;
        result.response_fragment = std::move(local_fragment);
        return result;
    }

    if (if_match == "*" || if_match != _etag) {
        result.status = WhipWhepPatchStatus::PreconditionFailed;
        return result;
    }
    if (!_transport->applyCandidates(fragment)) {
        result.status = WhipWhepPatchStatus::Unsupported;
        return result;
    }
    result.status = WhipWhepPatchStatus::Applied;
    return result;
}

string WhipWhepSession::etag() const {
    lock_guard<mutex> lock(_mtx);
    return _etag;
}

bool WhipWhepSession::closed() const {
    lock_guard<mutex> lock(_mtx);
    return _closed;
}

void WhipWhepSession::close() {
    lock_guard<mutex> lock(_mtx);
    if (_closed) {
        return;
    }
    _closed = true;
    _transport->close();
}

} // namespace mediakit
