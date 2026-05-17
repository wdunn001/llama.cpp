// codec_zstd_dict_registry.hpp -- per-stream-format pre-trained ZSTD dict
// registry for the Codec transport-compression negotiator.
//
// Per the Codec v0.4 spec (spec/versions/v0.4.md, §Pre-trained ZSTD
// dictionaries / §Codec-Zstd-Dict response header), a server MUST NOT
// respond with `Content-Encoding: zstd` unless it has loaded a matching
// pre-trained dict for the request's `stream_format` (msgpack vs protobuf
// are NOT interchangeable — they train against different byte
// distributions). And every zstd response MUST carry a
// `Codec-Zstd-Dict: sha256:<hex>` header naming the dict in use so the
// client can pick the matching dict to decompress.
//
// Without a dict, no-dict zstd is the worst of both worlds on Codec
// streams (RESULTS.md §1f: same wire bytes as gzip; §1d: 334× TTFB
// regression on buffered middleware). Hence the dict-gate.
//
// The registry is keyed by stream_format string ("msgpack" / "protobuf"
// — matches the values of the request's `stream_format` field).
//
// Default state is empty → the negotiator never picks zstd, falls
// through to br/gzip/identity per the spec preference order.
//
// Bootstrap is via env vars at server start:
//   CODEC_ZSTD_DICT_MSGPACK_PATH   path to msgpack-format dict file
//   CODEC_ZSTD_DICT_PROTOBUF_PATH  path to protobuf-format dict file
//
// `codec_zstd_dict_load_from_env()` is idempotent — safe to call twice.

#pragma once

#include <string>

// Read CODEC_ZSTD_DICT_{MSGPACK,PROTOBUF}_PATH env vars and, for each
// that points at a readable file, load its bytes and compute its
// sha256. Idempotent — repeated calls re-read the env (so an operator
// rotating a dict at runtime by re-pointing the env + sending SIGHUP
// would Just Work, though no such SIGHUP plumbing exists yet).
//
// Silently no-ops on missing env vars (default state: gzip / br /
// identity only). Logs a warning on a present env var that names an
// unreadable / empty path — the operator probably wants to know.
void codec_zstd_dict_load_from_env();

// True iff the registry has a dict for `stream_format`. The negotiator
// gates zstd on this — empty registry → never select zstd.
bool codec_zstd_dict_has(const std::string & stream_format);

// Raw dict bytes for `stream_format`. Returned by const-reference to a
// long-lived registry slot; callers should not assume the reference
// outlives a subsequent `codec_zstd_dict_load_from_env()` (it does in
// practice today, but the contract doesn't require it).
//
// Returns a reference to an empty string when the format has no dict
// registered; callers should consult `codec_zstd_dict_has()` first.
const std::string & codec_zstd_dict_bytes(const std::string & stream_format);

// `sha256:<lowercase-64-hex>` of the registered dict for
// `stream_format`. Goes verbatim onto the `Codec-Zstd-Dict` response
// header. Returns a reference to an empty string when no dict is
// registered.
const std::string & codec_zstd_dict_hash(const std::string & stream_format);
