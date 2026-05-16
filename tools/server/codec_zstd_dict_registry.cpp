// codec_zstd_dict_registry.cpp -- see header for rationale.
//
// SHA-256 is vendored inline (small, public-domain RFC 6234 reference
// transform) so this file doesn't pull in OpenSSL or the gguf-hash
// example's `deps/sha256/`. The dict bytes are hashed once at load
// time; the per-request cost on the negotiator hot path is a map
// lookup.

#include "codec_zstd_dict_registry.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ── Tiny SHA-256 (public domain, RFC 6234 reference) ────────────────
//
// One-shot interface only; we hash the whole dict in memory at load
// time. The transform is the textbook 64-round mixer; no SIMD because
// the input is ~16 KiB and runs exactly twice in a server's lifetime.

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buf[64];
    size_t   buf_len;
};

static const uint32_t k_round[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4 + 0]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) <<  8) | (uint32_t(block[i * 4 + 3]) <<  0);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>  3);
        uint32_t s1 = rotr(w[i -  2], 17) ^ rotr(w[i -  2], 19) ^ (w[i -  2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k_round[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_init(sha256_ctx & c) {
    c.state[0] = 0x6a09e667; c.state[1] = 0xbb67ae85; c.state[2] = 0x3c6ef372; c.state[3] = 0xa54ff53a;
    c.state[4] = 0x510e527f; c.state[5] = 0x9b05688c; c.state[6] = 0x1f83d9ab; c.state[7] = 0x5be0cd19;
    c.bit_count = 0;
    c.buf_len = 0;
}

static void sha256_update(sha256_ctx & c, const uint8_t * data, size_t len) {
    c.bit_count += uint64_t(len) * 8;
    while (len > 0) {
        size_t take = 64 - c.buf_len;
        if (take > len) take = len;
        std::memcpy(c.buf + c.buf_len, data, take);
        c.buf_len += take;
        data += take;
        len  -= take;
        if (c.buf_len == 64) {
            sha256_transform(c.state, c.buf);
            c.buf_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx & c, uint8_t out[32]) {
    // Append 0x80, pad zeros to 56 mod 64, then 8-byte big-endian length.
    uint64_t bits = c.bit_count;
    c.buf[c.buf_len++] = 0x80;
    if (c.buf_len > 56) {
        while (c.buf_len < 64) c.buf[c.buf_len++] = 0;
        sha256_transform(c.state, c.buf);
        c.buf_len = 0;
    }
    while (c.buf_len < 56) c.buf[c.buf_len++] = 0;
    for (int i = 7; i >= 0; --i) c.buf[c.buf_len++] = uint8_t((bits >> (i * 8)) & 0xff);
    sha256_transform(c.state, c.buf);
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = uint8_t((c.state[i] >> 24) & 0xff);
        out[i * 4 + 1] = uint8_t((c.state[i] >> 16) & 0xff);
        out[i * 4 + 2] = uint8_t((c.state[i] >>  8) & 0xff);
        out[i * 4 + 3] = uint8_t((c.state[i] >>  0) & 0xff);
    }
}

static std::string hash_to_codec_dict_header(const std::string & data) {
    sha256_ctx c;
    sha256_init(c);
    sha256_update(c, reinterpret_cast<const uint8_t *>(data.data()), data.size());
    uint8_t digest[32];
    sha256_final(c, digest);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(7 + 64);
    out.append("sha256:");
    for (int i = 0; i < 32; ++i) {
        out.push_back(hex[(digest[i] >> 4) & 0xf]);
        out.push_back(hex[(digest[i] >> 0) & 0xf]);
    }
    return out;
}

// ── Registry storage ────────────────────────────────────────────────

struct dict_entry {
    std::string bytes;
    std::string hash;  // "sha256:<lowercase-hex>"
};

static std::mutex                                   g_registry_mu;
static std::unordered_map<std::string, dict_entry>  g_registry;
static const std::string                            g_empty_string;

static bool read_file_all(const char * path, std::string & out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}

// Internal helper: read `env_var`, if it points at a readable non-empty
// file load it into the registry under `format`. Logged via stderr
// since this runs at server-init time before structured logging is up.
static void try_load(const char * format, const char * env_var) {
    const char * path = std::getenv(env_var);
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    std::string bytes;
    if (!read_file_all(path, bytes)) {
        fprintf(stderr,
                "codec: %s=%s — could not read dict file (or file is empty); "
                "zstd will not be advertised for stream_format=%s\n",
                env_var, path, format);
        return;
    }
    std::string h = hash_to_codec_dict_header(bytes);
    {
        std::lock_guard<std::mutex> lk(g_registry_mu);
        g_registry[format] = dict_entry{std::move(bytes), h};
    }
    fprintf(stderr,
            "codec: loaded zstd dict for stream_format=%s from %s "
            "(%zu bytes, %s)\n",
            format, path, g_registry[format].bytes.size(), h.c_str());
}

} // namespace

void codec_zstd_dict_load_from_env() {
    try_load("msgpack",  "CODEC_ZSTD_DICT_MSGPACK_PATH");
    try_load("protobuf", "CODEC_ZSTD_DICT_PROTOBUF_PATH");
}

bool codec_zstd_dict_has(const std::string & stream_format) {
    if (stream_format.empty()) return false;
    std::lock_guard<std::mutex> lk(g_registry_mu);
    return g_registry.find(stream_format) != g_registry.end();
}

const std::string & codec_zstd_dict_bytes(const std::string & stream_format) {
    std::lock_guard<std::mutex> lk(g_registry_mu);
    auto it = g_registry.find(stream_format);
    if (it == g_registry.end()) return g_empty_string;
    return it->second.bytes;
}

const std::string & codec_zstd_dict_hash(const std::string & stream_format) {
    std::lock_guard<std::mutex> lk(g_registry_mu);
    auto it = g_registry.find(stream_format);
    if (it == g_registry.end()) return g_empty_string;
    return it->second.hash;
}
