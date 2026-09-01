// Codec v0.4 version negotiation for llama-server.
//
// C++ mirror of:
//   - wdunn001/sglang main: python/sglang/srt/entrypoints/codec_version.py
//   - wdunn001/vllm   main: vllm/entrypoints/codec_version.py
//
// See spec/versions/v0.4.md:
//   § Capabilities are opt-on at the server (two-stage)
//   § Graceful downgrade (response shaping)
//   § Version Compatibility Signaling — 426 path
//
// Header-only — keeps the CMake graph unchanged. The functions here are
// pure (read env each call) so they pick up runtime changes from a
// container restart without a recompile.
//
// Config (env, mirrors sglang + vllm):
//   CODEC_SAFETY_POLICY=<id>             enable safety-policy
//   CODEC_SAFETY_POLICY_REQUIRED=1       enforce safety-policy
//   CODEC_VERSION_POLICY=advisory|strict  enable + enforce version policy
//   CODEC_DEPLOYMENT_ID=<str>            appears in 426 body + well-known

#ifndef LLAMA_SERVER_CODEC_VERSION_HPP
#define LLAMA_SERVER_CODEC_VERSION_HPP

#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace codec_version {

inline constexpr const char * CLIENT_VERSION = "0.4";
inline constexpr const char * CLIENT_VERSION_HEADER = "Codec-Client-Version";
inline constexpr const char * MIN_VERSION_HEADER = "Codec-Min-Version";
inline constexpr const char * REQUIRED_FEATURES_HEADER = "Codec-Required-Features";

// Per spec § Version Compatibility Signaling: absent header = treat as
// "0.2" baseline (the oldest published version).
inline constexpr const char * DEFAULT_CLIENT_VERSION = "0.2";

// ── Version comparison ───────────────────────────────────────────────────

// Parse "0.X" or "0.X.Y" or "v0.X" into (major, minor). Patch ignored.
inline std::pair<int, int> parse_(const std::string & s) {
    const char * p = s.c_str();
    while (*p == 'v' || *p == 'V') ++p;
    int major = 0, minor = 0;
    if (sscanf(p, "%d.%d", &major, &minor) >= 1) {
        return {major, minor};
    }
    return {0, 0};
}

inline bool version_ge(const std::string & a, const std::string & b) {
    return parse_(a) >= parse_(b);
}

inline bool version_lt(const std::string & a, const std::string & b) {
    return !version_ge(a, b);
}

// ── Request parsing ──────────────────────────────────────────────────────

// Case-insensitive header lookup against the request's `headers` map.
inline std::string parse_client_version(const std::map<std::string, std::string> & headers) {
    std::string lower_target = "codec-client-version";
    for (const auto & kv : headers) {
        std::string k = kv.first;
        for (auto & c : k) c = static_cast<char>(std::tolower(c));
        if (k == lower_target) {
            std::string v = kv.second;
            // Trim leading "v" or whitespace.
            size_t start = 0;
            while (start < v.size() && (v[start] == ' ' || v[start] == '\t' || v[start] == 'v')) {
                ++start;
            }
            return v.substr(start);
        }
    }
    return DEFAULT_CLIENT_VERSION;
}

// ── Header version-introduced floor (mirrors spec table) ─────────────────

inline std::map<std::string, std::string> header_version_introduced() {
    return {
        {"Codec-Tokenizer-Map",       "0.2"},
        {"Codec-Zstd-Dict",           "0.2"},
        {"Codec-Latent-Map",          "0.3"},
        {"Codec-Map",                 "0.3"},
        {"Codec-Safety-Policy",       "0.4"},
        {"Codec-Safety-Policy-Hash",  "0.4"},
    };
}

// Lower-cased copy of header_version_introduced() for case-insensitive
// lookup. HTTP field names are case-insensitive per RFC 9110 §5.1.
// A caller-supplied header differing only in case from the table's
// canonical spelling must still resolve to the same floor.
// Mirrors the lowercased registry built once in the Python reference,
// vllm/entrypoints/codec_version.py.
inline std::map<std::string, std::string> header_version_introduced_lower() {
    std::map<std::string, std::string> out;
    for (const auto & kv : header_version_introduced()) {
        std::string k = kv.first;
        for (auto & c : k) c = static_cast<char>(std::tolower(c));
        out[k] = kv.second;
    }
    return out;
}

// True iff the server may emit `name` to a client speaking
// `client_version`. A header not in the table defaults to true.
// That covers both the v0.2 baseline headers and non-Codec headers such
// as Content-Type or Vary.
//
// The lookup is case-insensitive. Without this, a header spelled with a
// different case than the table's entry would miss the table lookup and
// fall through to the "always emit" default, leaking a v0.4 header to a
// client that never asked for it.
inline bool should_emit_header(const std::string & name, const std::string & client_version) {
    std::string lower_name = name;
    for (auto & c : lower_name) c = static_cast<char>(std::tolower(c));
    auto floors = header_version_introduced_lower();
    auto it = floors.find(lower_name);
    if (it == floors.end()) return true;
    return version_ge(client_version, it->second);
}

// Strip Codec-* headers whose floor > client_version.
inline std::map<std::string, std::string> filter_codec_headers(
        const std::map<std::string, std::string> & headers,
        const std::string & client_version) {
    std::map<std::string, std::string> out;
    for (const auto & kv : headers) {
        if (should_emit_header(kv.first, client_version)) {
            out[kv.first] = kv.second;
        }
    }
    return out;
}

// ── Capability config (stage-1: enable; stage-2: enforce) ────────────────

inline bool env_truthy_(const char * name) {
    const char * v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s = v;
    return s == "1" || s == "true" || s == "True" || s == "yes";
}

inline std::string env_str_(const char * name) {
    const char * v = std::getenv(name);
    return v ? v : "";
}

inline bool safety_policy_enabled() {
    return !env_str_("CODEC_SAFETY_POLICY").empty();
}

inline bool safety_policy_required() {
    if (!safety_policy_enabled()) return false;
    return env_truthy_("CODEC_SAFETY_POLICY_REQUIRED");
}

// Returns "off" | "advisory" | "strict". Default off.
inline std::string version_policy_mode() {
    std::string m = env_str_("CODEC_VERSION_POLICY");
    if (m == "advisory" || m == "strict") return m;
    return "off";
}

inline bool any_v04_mandatory() {
    return safety_policy_required() || version_policy_mode() == "strict";
}

inline std::vector<std::string> collect_required_features() {
    std::vector<std::string> out;
    if (safety_policy_required()) {
        out.push_back("safety-policy-enforcement");
    }
    return out;
}

inline bool needs_upgrade(const std::string & client_version, const std::string & min_version = "0.4") {
    if (!any_v04_mandatory()) return false;
    return version_lt(client_version, min_version);
}

// ── 426 builder ──────────────────────────────────────────────────────────

// JSON-escape a string for embedding into the response body. Minimal —
// handles the characters that appear in our schema (no embedded
// control chars, no Unicode beyond what env vars allow).
inline std::string json_escape_(const std::string & s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Build the 426 response body. Returns (json_body, headers).
struct upgrade_required_response {
    std::string body;
    std::map<std::string, std::string> headers;
};

inline upgrade_required_response build_426(const std::string & client_version,
                                            const std::string & min_version = "0.4") {
    auto features = collect_required_features();
    std::ostringstream body;
    body << "{\"error\":\"codec_version_required\","
         << "\"minimum_version\":\"" << json_escape_(min_version) << "\","
         << "\"required_features\":[";
    for (size_t i = 0; i < features.size(); ++i) {
        if (i) body << ",";
        body << "\"" << json_escape_(features[i]) << "\"";
    }
    body << "],"
         << "\"client_version\":\"" << json_escape_(client_version) << "\","
         << "\"docs_url\":\"https://codecai.net/docs/version-negotiation/\"";
    std::string deployment_id = env_str_("CODEC_DEPLOYMENT_ID");
    if (!deployment_id.empty()) {
        body << ",\"deployment_id\":\"" << json_escape_(deployment_id) << "\"";
    }
    body << "}";

    upgrade_required_response out;
    out.body = body.str();
    out.headers[MIN_VERSION_HEADER] = min_version;
    if (!features.empty()) {
        std::ostringstream f;
        for (size_t i = 0; i < features.size(); ++i) {
            if (i) f << ", ";
            f << features[i];
        }
        out.headers[REQUIRED_FEATURES_HEADER] = f.str();
    }
    return out;
}

// ── Well-known doc ───────────────────────────────────────────────────────

// Returns the JSON body for `.well-known/codec/version-policy.json`,
// or empty string when the deployment has no mandatory features
// (the route handler returns 404 in that case per the spec).
inline std::string version_policy_document_json() {
    if (!any_v04_mandatory()) return "";
    auto features = collect_required_features();
    std::ostringstream out;
    out << "{\"minimum_version\":\"0.4\","
        << "\"required_features\":[";
    for (size_t i = 0; i < features.size(); ++i) {
        if (i) out << ",";
        out << "\"" << json_escape_(features[i]) << "\"";
    }
    out << "],";
    std::string deployment_id = env_str_("CODEC_DEPLOYMENT_ID");
    if (!deployment_id.empty()) {
        out << "\"deployment_id\":\"" << json_escape_(deployment_id) << "\",";
    }
    out << "\"docs_url\":\"https://codecai.net/docs/version-negotiation/\"}";
    return out.str();
}

} // namespace codec_version

#endif // LLAMA_SERVER_CODEC_VERSION_HPP
