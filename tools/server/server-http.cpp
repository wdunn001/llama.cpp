#include "common.h"
#include "http.h"
#include "server-http.h"
#include "server-common.h"
#include "ui.h"

#include <cpp-httplib/httplib.h>

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

#if defined(LLAMA_HAVE_CODEC_GZIP)
#include <zlib.h>
#endif

#if defined(LLAMA_HAVE_CODEC_BROTLI)
#include <brotli/encode.h>
#endif

#if defined(LLAMA_HAVE_CODEC_ZSTD)
#include <zstd.h>
#include "codec_zstd_dict_registry.hpp"
#endif

//
// HTTP implementation using cpp-httplib
//

class server_http_context::Impl {
public:
    std::unique_ptr<httplib::Server> srv;
};

server_http_context::server_http_context()
    : pimpl(std::make_unique<Impl>())
{}

server_http_context::~server_http_context() = default;

static void log_server_request(const httplib::Request & req, const httplib::Response & res) {
    // skip logging requests that are regularly sent, to avoid log spam
    if (req.path == "/health"
        || req.path == "/v1/health"
        || req.path == "/models"
        || req.path == "/v1/models"
        || req.path == "/props"
        || req.path == "/metrics"
    ) {
        return;
    }

    // reminder: this function is not covered by httplib's exception handler; if someone does more complicated stuff, think about wrapping it in try-catch

    SRV_TRC("done request: %s %s %s %d\n", req.method.c_str(), req.path.c_str(), req.remote_addr.c_str(), res.status);

    SRV_DBG("request:  %s\n", req.body.c_str());
    SRV_DBG("response: %s\n", res.body.c_str());
}

// returns true if the Origin header value's host is localhost / 127.0.0.1 / ::1 (any port)
static bool origin_is_localhost(const std::string & origin) {
    try {
        const std::string host = common_http_parse_url(origin).host;
        return host == "localhost" || host == "127.0.0.1" || host == "::1";
    } catch (const std::exception &) {
        return false;
    }
}

// For Google Cloud Platform deployment compatibility
struct gcp_params {
    bool enabled;
    std::string path_health;
    std::string path_predict;
    int port;

    // Ref: https://docs.cloud.google.com/vertex-ai/docs/predictions/custom-container-requirements#aip-variables
    gcp_params() {
        enabled = getenv("AIP_MODE", "") == "PREDICTION";
        path_health = getenv("AIP_HEALTH_ROUTE", "", true); // default: using the route defined in server.cpp
        path_predict = getenv("AIP_PREDICT_ROUTE", "/predict", true);
        port = std::stoi(getenv("AIP_HTTP_PORT", "8080"));
    }

    static std::string getenv(const char * name, const std::string & default_value, bool ensure_leading_slash = false) {
        const auto * value = std::getenv(name);
        if (value == nullptr || value[0] == '\0') {
            return default_value;
        }
        std::string val = value;
        if (ensure_leading_slash && !val.empty() && val[0] != '/') {
            val.insert(val.begin(), '/');
        }
        return val;
    }
};

bool server_http_context::init(const common_params & params) {
    const gcp_params gcp;

    path_prefix = params.api_prefix;
    port = params.port;
    hostname = params.hostname;

    if (gcp.enabled) {
        SRV_TRC("Google Cloud Platform compat: health route = %s, predict route = %s, port = %d\n", gcp.path_health.c_str(), gcp.path_predict.c_str(), gcp.port);

        if (port != gcp.port) {
            SRV_WRN("Google Cloud Platform compat: overriding server port %d with AIP_HTTP_PORT %d\n", port, gcp.port);
        }

        port = gcp.port;
    }

    auto & srv = pimpl->srv;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (!params.ssl_file_key.empty() && !params.ssl_file_cert.empty()) {
        SRV_TRC("running with SSL: key = %s, cert = %s\n", params.ssl_file_key.c_str(), params.ssl_file_cert.c_str());
        srv = std::make_unique<httplib::SSLServer>(
            params.ssl_file_cert.c_str(), params.ssl_file_key.c_str()
        );
        is_ssl = true;
    } else {
        SRV_TRC("%s", "running without SSL\n");
        srv = std::make_unique<httplib::Server>();
    }
#else
    if (params.ssl_file_key != "" && params.ssl_file_cert != "") {
        SRV_ERR("%s", "the server is built without SSL support\n");
        return false;
    }
    srv.reset(new httplib::Server());
#endif

    srv->set_default_headers({{"Server", "llama.cpp"}});
    // srv->set_logger(log_server_request); // TODO @ngxson : this is too spamy, no very useful; improve it in the future
    srv->set_exception_handler([](const httplib::Request &, httplib::Response & res, const std::exception_ptr & ep) {
        // this is fail-safe; exceptions should already handled by `ex_wrapper`

        std::string message;
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception & e) {
            message = e.what();
        } catch (...) {
            message = "Unknown Exception";
        }

        res.status = 500;
        res.set_content(message, "text/plain");
        SRV_ERR("got exception: %s\n", message.c_str());
    });

    srv->set_error_handler([](const httplib::Request &, httplib::Response & res) {
        if (res.status == 404) {
            res.set_content(
                safe_json_to_str(json {
                    {"error", {
                        {"message", "File Not Found"},
                        {"type", "not_found_error"},
                        {"code", 404}
                    }}
                }),
                "application/json; charset=utf-8"
            );
        }
        // for other error codes, we skip processing here because it's already done by res->error()
    });

    // set timeouts and change hostname and port
    srv->set_read_timeout (params.timeout_read);
    srv->set_write_timeout(params.timeout_write);
    srv->set_socket_options([reuse_port = params.reuse_port](const socket_t sock) {
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
        if (reuse_port) {
#ifdef SO_REUSEPORT
            httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEPORT, 1);
#else
            SRV_WRN("%s", "SO_REUSEPORT is not supported\n");
#endif
        }
    });

    if (params.api_keys.size() == 1) {
        const auto key = params.api_keys[0];
        const std::string substr = key.substr(std::max(static_cast<int>(key.length() - 4), 0));
        SRV_TRC("api_keys: ****%s\n", substr.c_str());
    } else if (params.api_keys.size() > 1) {
        SRV_TRC("api_keys: %zu keys loaded\n", params.api_keys.size());
    }

    //
    // Middlewares
    //

    // Frontend paths - all embedded UI assets
    static const std::unordered_set<std::string> frontend_paths = []() {
        std::unordered_set<std::string> paths { "/" };
        for (const llama_ui_asset & a : llama_ui_get_assets()) {
            paths.insert("/" + a.name);
        }
        return paths;
    }();

    // Public endpoints - API routes plus all embedded UI assets
    static const std::unordered_set<std::string> get_public_endpoints = []() {
        std::unordered_set<std::string> endpoints {
            "/health",
            "/v1/health",
        };
        endpoints.insert(frontend_paths.begin(), frontend_paths.end());
        return endpoints;
    }();

    auto middleware_validate_api_key = [api_keys = params.api_keys](const httplib::Request & req, httplib::Response & res) {
        // If API key is not set, skip validation
        if (api_keys.empty()) {
            return true;
        }

        // If path is public or a UI asset, skip validation
        if (get_public_endpoints.count(req.path)) {
            return true;
        }

        // Check for API key in the Authorization header
        std::string req_api_key = req.get_header_value("Authorization");
        if (req_api_key.empty()) {
            // retry with anthropic header
            req_api_key = req.get_header_value("X-Api-Key");
        }

        // remove the "Bearer " prefix if needed
        static std::string prefix = "Bearer ";
        if (req_api_key.substr(0, prefix.size()) == prefix) {
            req_api_key = req_api_key.substr(prefix.size());
        }

        // validate the API key
        if (std::find(api_keys.begin(), api_keys.end(), req_api_key) != api_keys.end()) {
            return true; // API key is valid
        }

        // API key is invalid or not provided
        res.status = 401;
        res.set_content(
            safe_json_to_str(json {
                {"error", {
                    {"message", "Invalid API Key"},
                    {"type", "authentication_error"},
                    {"code", 401}
                }}
            }),
            "application/json; charset=utf-8"
        );

        SRV_WRN("%s", "unauthorized: Invalid API Key\n");

        return false;
    };

    auto middleware_server_state = [this](const httplib::Request & req, httplib::Response & res) {
        if (!is_ready.load()) {
            if (frontend_paths.count(req.path)) {
                return true; // frontend asset, allow it to load and show "loading"
            }
            // no endpoints are allowed to be accessed when the server is not ready
            // this is to prevent any data races or inconsistent states
            res.status = 503;
            res.set_content(
                safe_json_to_str(json {
                    {"error", {
                        {"message", "Loading model"},
                        {"type", "unavailable_error"},
                        {"code", 503}
                    }}
                }),
                "application/json; charset=utf-8"
            );
            return false;
        }
        return true;
    };

    // register server middlewares
    srv->set_pre_routing_handler([&params, middleware_validate_api_key, middleware_server_state](const httplib::Request & req, httplib::Response & res) {
        if (params.cors_credentials && params.cors_origins == "*") {
            // special case: echo back the Origin header to allow any origin to access the server with credentials
            res.set_header("Access-Control-Allow-Origin", req.get_header_value("Origin"));
        } else if (params.cors_origins == "localhost") {
            // special case: only reflect the Origin header if it is a localhost origin
            std::string origin = req.get_header_value("Origin");
            if (!origin.empty() && origin_is_localhost(origin)) {
                res.set_header("Access-Control-Allow-Origin", origin);
            } else if (!origin.empty()) {
                SRV_WRN("(CORS) skip non-localhost origin: %s\n", origin.c_str());
            }
        } else {
            res.set_header("Access-Control-Allow-Origin", params.cors_origins);
        }
        // If this is OPTIONS request, skip validation because browsers don't include Authorization header
        if (req.method == "OPTIONS") {
            res.set_header("Access-Control-Allow-Credentials", params.cors_credentials ? "true" : "false");
            res.set_header("Access-Control-Allow-Methods",     params.cors_methods);
            res.set_header("Access-Control-Allow-Headers",     params.cors_headers);
            res.set_content("", "text/html"); // blank response, no data
            return httplib::Server::HandlerResponse::Handled; // skip further processing
        }
        if (!middleware_server_state(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!middleware_validate_api_key(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    auto n_threads_http = params.n_threads_http;
    if (n_threads_http < 1) {
        // +4 threads for monitoring, health and some threads reserved for MCP and other tasks in the future
        n_threads_http = std::max(params.n_parallel + 4, static_cast<int32_t>(std::thread::hardware_concurrency() - 1));
    }
    SRV_TRC("using %d threads for HTTP server\n", n_threads_http);
    srv->new_task_queue = [n_threads_http] {
        // spawn n_threads_http fixed thread (always alive), while allow up to 1024 max possible additional threads
        // when n_threads_http is used, server will create new "dynamic" threads that will be destroyed after processing each request
        // ref: https://github.com/yhirose/cpp-httplib/pull/2368
        const auto max_threads = static_cast<size_t>(n_threads_http + 1024);
        return new httplib::ThreadPool(n_threads_http, max_threads);
    };

    //
    // Web UI setup
    //

    // Use new `params.ui` field (backed by old `params.webui` for compat)
    if (!params.ui) {
        SRV_INF("%s", "The UI is disabled\n");
        SRV_INF("%s", "Use --ui/--no-ui (or deprecated --webui/--no-webui) to enable/disable\n");
    } else {
        // register static assets routes
        if (!params.public_path.empty()) {
            // Set the base directory for serving static files
            if (const auto is_found = srv->set_mount_point(params.api_prefix + "/", params.public_path); !is_found) {
                SRV_ERR("static assets path not found: %s\n", params.public_path.c_str());
                return false;
            }
        } else {
#if defined(LLAMA_UI_HAS_ASSETS)
            static auto handle_gzip_header = [](const httplib::Request & req, httplib::Response & res) {
                if (!llama_ui_use_gzip()) {
                    // no gzip build, skip
                    return true;
                }
                if (req.get_header_value("Accept-Encoding").find("gzip") == std::string::npos) {
                    res.status = 415; // unsupported media type
                    res.set_content("Error: gzip is not supported by this browser", "text/plain");
                    return false;
                } else {
                    res.set_header("Content-Encoding", "gzip");
                }
                return true;
            };

            // Hashed assets never change under a given name, so they can be cached forever.
            // `index.html` is the exception: its name is stable while its contents change on
            // every build, and it is what names the hashed asset versions the UI loads.
            static constexpr auto cache_immutable  = "public, max-age=31536000, immutable";
            static constexpr auto cache_revalidate = "no-cache";

            // Serves an asset with ETag/304 handling, under the given caching policy.
            auto serve_asset_cached = [](const std::string & name, bool isolation, const char * cache_control) {
                return [name, isolation, cache_control](const httplib::Request & req, httplib::Response & res) {
                    if (!handle_gzip_header(req, res)) {
                        return true; // returns error message
                    }
                    const llama_ui_asset * a = llama_ui_find_asset(name);
                    if (!a) { res.status = 404; return false; }
                    res.set_header("ETag", a->etag);
                    if (const std::string & inm = req.get_header_value("If-None-Match");
                        !inm.empty() && (inm == a->etag || inm == std::string("W/") + a->etag)) {
                        res.status = 304;
                        return false;
                    }
                    if (isolation) {
                        res.set_header("Cross-Origin-Embedder-Policy", "require-corp");
                        res.set_header("Cross-Origin-Opener-Policy",   "same-origin");
                    }
                    res.set_header("Cache-Control", cache_control);
                    res.set_content(reinterpret_cast<const char*>(a->data), a->size, a->type.c_str());
                    return false;
                };
            };

            auto serve_asset_nocache = [](const std::string & name) {
                return [name](const httplib::Request & req, httplib::Response & res) {
                    if (!handle_gzip_header(req, res)) {
                        return true; // returns error message
                    }
                    const llama_ui_asset * a = llama_ui_find_asset(name);
                    if (!a) {
                        res.status = 404;
                        return false;
                    }
                    res.set_header("Cache-Control", "no-cache");
                    res.set_content(reinterpret_cast<const char*>(a->data), a->size, a->type.c_str());
                    return false;
                };
            };

            // main index file -- revalidated, so a new build is picked up on the next load
            srv->Get(params.api_prefix + "/",           serve_asset_cached("index.html", true, cache_revalidate));
            srv->Get(params.api_prefix + "/index.html", serve_asset_cached("index.html", true, cache_revalidate));

            // All remaining assets registered directly from the embedded asset table.
            // PWA revalidation files (sw.js, manifest, version.json) use no-cache;
            // everything else is immutable.
            static const std::unordered_set<std::string> no_cache_names = {
                "sw.js",
                "manifest.webmanifest",
                "_app/version.json",
                "build.json"
            };

            for (const auto & a : llama_ui_get_assets()) {
                if (a.name == "index.html") continue;  // served at "/" and "/index.html" above
                if (no_cache_names.count(a.name)) {
                    SRV_DBG("serve nocache for %s\n", a.name.c_str());
                    srv->Get(params.api_prefix + "/" + a.name, serve_asset_nocache(a.name));
                } else {
                    srv->Get(params.api_prefix + "/" + a.name, serve_asset_cached(a.name, false, cache_immutable));
                }
            }

#endif
        }
    }
    return true;
}

bool server_http_context::start() {
    // Bind and listen

    const auto & srv = pimpl->srv;
    auto was_bound = false;
    auto is_sock = false;
    if (string_ends_with(std::string(hostname), ".sock")) {
        is_sock = true;
        SRV_TRC("%s", "setting address family to AF_UNIX\n");
        srv->set_address_family(AF_UNIX);
        // bind_to_port requires a second arg, any value other than 0 should
        // simply get ignored
        was_bound = srv->bind_to_port(hostname, 8080);
    } else {
        SRV_TRC("%s", "binding port with default address family\n");
        // bind HTTP listen port
        if (port == 0) {
            const auto bound_port = srv->bind_to_any_port(hostname);
            was_bound = (bound_port >= 0);
            if (was_bound) {
                port = bound_port;
            }
        } else {
            was_bound = srv->bind_to_port(hostname, port);
        }
    }

    if (!was_bound) {
        SRV_ERR("couldn't bind HTTP server socket, hostname: %s, port: %d\n", hostname.c_str(), port);
        return false;
    }

    // run the HTTP server in a thread
    thread = std::thread([this] { pimpl->srv->listen_after_bind(); });
    srv->wait_until_ready();

    listening_address = is_sock ? string_format("unix://%s", hostname.c_str())
                                : string_format("%s://%s:%d", is_ssl ? "https" : "http", common_http_format_host(hostname).c_str(), port);
    return true;
}

void server_http_context::stop() const {
    if (pimpl->srv) {
        pimpl->srv->stop();
    }
}

static void set_headers(httplib::Response & res, const std::map<std::string, std::string> & headers) {
    for (const auto & [key, value] : headers) {
        res.set_header(key, value);
    }
}

// percent-decode a path component (%XX). path params arrive raw from httplib, unlike query
// params, so a conv id like "conv::model" sent as "conv%3A%3Amodel" must be decoded here to
// match the value the client put in the X-Conversation-Id header
static std::string decode_path_component(const std::string & in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(char((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

static std::map<std::string, std::string> get_params(const httplib::Request & req) {
    std::map<std::string, std::string> params;
    for (const auto & [key, value] : req.params) {
        params[key] = value;
    }
    for (const auto & [key, value] : req.path_params) {
        params[key] = decode_path_component(value);
    }
    return params;
}

static std::map<std::string, std::string> get_headers(const httplib::Request & req) {
    std::map<std::string, std::string> headers;
    for (const auto & [key, value] : req.headers) {
        headers[key] = value;
    }
    return headers;
}

static std::string build_query_string(const httplib::Request & req) {
    std::string qs;
    for (const auto & [key, value] : req.params) {
        if (!qs.empty()) {
            qs += '&';
        }
        qs += httplib::encode_query_component(key) + "=" + httplib::encode_query_component(value);
    }
    return qs;
}

// using unique_ptr for request to allow safe capturing in lambdas
using server_http_req_ptr = std::unique_ptr<server_http_req>;

#if defined(LLAMA_HAVE_CODEC_GZIP)
// Streaming gzip wrapper for chunked responses. Codec frames compress
// extremely well (32-byte msgpack frames → ~3 B/token after deflate) and
// the level-6 default cost is sub-microsecond per frame, so this lifts
// llama-server's Codec wire from ~17–25× to ~700×+ vs JSON-SSE at 2K
// tokens with no measurable TTFT impact (gzip is the only stream-clean
// encoding on the cross-stack matrix). Activated when a handler sets
// Content-Encoding: gzip on the response — the Codec endpoint is
// currently the only such handler. Other streams pass through unchanged.
class codec_gzip_streamer {
public:
    codec_gzip_streamer() : initialized_(false) {
        // 15 = max window, +16 = gzip wrapper (vs zlib wrapper at 15+0).
        // memLevel=8 and Z_DEFAULT_STRATEGY are the standard "balanced"
        // settings; matches what Apache/nginx use for streamed responses.
        zs_ = z_stream{};
        zs_.zalloc = Z_NULL;
        zs_.zfree  = Z_NULL;
        zs_.opaque = Z_NULL;
        if (deflateInit2(&zs_, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
            initialized_ = true;
        }
    }
    ~codec_gzip_streamer() {
        if (initialized_) deflateEnd(&zs_);
    }
    codec_gzip_streamer(const codec_gzip_streamer &) = delete;
    codec_gzip_streamer & operator=(const codec_gzip_streamer &) = delete;

    bool ok() const { return initialized_; }

    // Append `chunk` to the deflate stream; deflated output appended to
    // `out`. `flush` is Z_SYNC_FLUSH for mid-stream chunks (forces zlib
    // to emit deflated bytes for what we just fed in, preserving TTFT)
    // and Z_FINISH on the last call (closes the gzip stream cleanly).
    // Z_NO_FLUSH would let zlib buffer optimally for ratio, which is
    // wrong for streaming — the client wouldn't see anything until a
    // full block accumulated.
    bool deflate_append(const char * data, size_t len, int flush, std::string & out) {
        if (!initialized_) return false;
        zs_.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(data));
        zs_.avail_in = static_cast<uInt>(len);
        unsigned char buf[16 * 1024];
        do {
            zs_.next_out  = buf;
            zs_.avail_out = sizeof(buf);
            int rc = deflate(&zs_, flush);
            if (rc == Z_STREAM_ERROR) return false;
            size_t produced = sizeof(buf) - zs_.avail_out;
            if (produced > 0) {
                out.append(reinterpret_cast<const char *>(buf), produced);
            }
            if (rc == Z_STREAM_END) break;
            // Loop until zlib has consumed all input AND has emitted any
            // pending output for the requested flush mode. Z_FINISH
            // additionally requires Z_STREAM_END to terminate.
            if (zs_.avail_in == 0 && zs_.avail_out > 0 && flush != Z_FINISH) break;
        } while (zs_.avail_in > 0 || flush == Z_FINISH);
        return true;
    }

private:
    z_stream zs_;
    bool     initialized_;
};
#endif // LLAMA_HAVE_CODEC_GZIP

#if defined(LLAMA_HAVE_CODEC_BROTLI)
// Streaming brotli wrapper. Quality 4 matches `_compress_brotli` in
// the sglang/vLLM Python ports (sglang/srt/entrypoints/codec_compression.py)
// and is the operating-point fix from the v0.4.1 bench: BROTLI_DEFAULT_QUALITY
// (11) is 10–50× slower for stream workloads with negligible ratio gain on
// the structured-int Codec payload. Mode GENERIC, lgwin 22 mirror the Python.
//
// IMPORTANT: do NOT flush after every chunk. The v0.4.1 Python regression
// (per-chunk flush bug) showed it inflates small streams (64-token msgpack:
// 1159 B vs 975 B identity) because each flush emits a complete brotli
// block + its own header, forfeiting between-chunk dictionary sharing.
// Mid-stream chunks pass BROTLI_OPERATION_PROCESS; the final chunk gets
// BROTLI_OPERATION_FINISH which flushes once at stream end.
class codec_brotli_streamer {
public:
    codec_brotli_streamer() : enc_(nullptr), initialized_(false) {
        enc_ = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
        if (enc_ == nullptr) return;
        // Order of parameters matches the Python ref.
        if (!BrotliEncoderSetParameter(enc_, BROTLI_PARAM_QUALITY, 4u))           return;
        if (!BrotliEncoderSetParameter(enc_, BROTLI_PARAM_MODE,    BROTLI_MODE_GENERIC)) return;
        if (!BrotliEncoderSetParameter(enc_, BROTLI_PARAM_LGWIN,   22u))          return;
        initialized_ = true;
    }
    ~codec_brotli_streamer() {
        if (enc_) BrotliEncoderDestroyInstance(enc_);
    }
    codec_brotli_streamer(const codec_brotli_streamer &) = delete;
    codec_brotli_streamer & operator=(const codec_brotli_streamer &) = delete;

    bool ok() const { return initialized_; }

    // Append `chunk` to the brotli stream; encoded output appended to
    // `out`. `op` is BROTLI_OPERATION_PROCESS for mid-stream chunks and
    // BROTLI_OPERATION_FINISH on the last call.
    //
    // Per the brotli API contract (encode.h §BrotliEncoderCompressStream),
    // FINISH may need multiple calls before the encoder drains; loop
    // until `available_in == 0` AND, for FINISH, `BrotliEncoderIsFinished`
    // is true.
    bool compress_append(const char * data, size_t len, BrotliEncoderOperation op, std::string & out) {
        if (!initialized_) return false;
        const uint8_t * next_in = reinterpret_cast<const uint8_t *>(data);
        size_t avail_in = len;

        uint8_t buf[16 * 1024];
        // Drive the encoder until input drained. Each call may emit
        // 0..N output bytes — we copy whatever it produced into `out`.
        while (true) {
            uint8_t * next_out = buf;
            size_t    avail_out = sizeof(buf);
            if (!BrotliEncoderCompressStream(enc_, op,
                                             &avail_in, &next_in,
                                             &avail_out, &next_out,
                                             nullptr)) {
                return false;
            }
            size_t produced = sizeof(buf) - avail_out;
            if (produced > 0) {
                out.append(reinterpret_cast<const char *>(buf), produced);
            }

            // FINISH continues until the encoder reports done.
            if (op == BROTLI_OPERATION_FINISH) {
                if (BrotliEncoderIsFinished(enc_)) return true;
                // Need another round to drain residual output.
                continue;
            }

            // PROCESS / FLUSH: stop once input is consumed and the
            // encoder has no more queued output.
            if (avail_in == 0 && !BrotliEncoderHasMoreOutput(enc_)) return true;
        }
    }

private:
    BrotliEncoderState * enc_;
    bool                 initialized_;
};
#endif // LLAMA_HAVE_CODEC_BROTLI

#if defined(LLAMA_HAVE_CODEC_ZSTD)
// Streaming zstd wrapper. Construction takes the pre-trained dict
// bytes; ZSTD_CCtx_loadDictionary copies the dict so the caller's
// std::string can be released after construction.
//
// Per Codec v0.4 spec/PROTOCOL.md §Pre-trained ZSTD dictionaries the
// dict is the precondition for using zstd at all — the negotiator
// gates selection on codec_zstd_dict_has(stream_format), so this class
// is never constructed without a real dict. If loadDictionary fails
// (corrupt dict file, etc.) we set initialized_=false so the dispatch
// can strip Content-Encoding and fall through to identity rather than
// stream a broken zstd frame.
//
// Level 3 matches the Python reference and is the operating-point for
// streaming: higher levels (10+) trade CPU for marginal ratio gains on
// structured Codec payloads with already-loaded dicts. Per-chunk: send
// ZSTD_e_continue. End: ZSTD_e_end (loops until 0 returned per the
// zstd.h §ZSTD_compressStream2 contract).
class codec_zstd_streamer {
public:
    explicit codec_zstd_streamer(const std::string & dict_bytes)
        : cctx_(nullptr), initialized_(false) {
        cctx_ = ZSTD_createCStream();
        if (cctx_ == nullptr) return;
        // Level 3 matches the Python `_compress_zstd` reference.
        size_t rc = ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, 3);
        if (ZSTD_isError(rc)) return;
        rc = ZSTD_CCtx_loadDictionary(cctx_, dict_bytes.data(), dict_bytes.size());
        if (ZSTD_isError(rc)) {
            // Dict bytes were unparseable — leave initialized_=false so
            // the dispatch falls through to identity.
            return;
        }
        initialized_ = true;
    }
    ~codec_zstd_streamer() {
        if (cctx_) ZSTD_freeCStream(cctx_);
    }
    codec_zstd_streamer(const codec_zstd_streamer &) = delete;
    codec_zstd_streamer & operator=(const codec_zstd_streamer &) = delete;

    bool ok() const { return initialized_; }

    // Append `chunk` to the zstd stream; encoded output appended to
    // `out`. `end` mode is ZSTD_e_continue for mid-stream chunks and
    // ZSTD_e_end on the last call.
    //
    // Per the zstd API contract (zstd.h §ZSTD_compressStream2),
    // ZSTD_e_end may need multiple calls before the encoder drains the
    // current frame; loop until the call returns 0 (frame complete) or
    // produces no more output and consumes all input (continue case).
    bool compress_append(const char * data, size_t len, ZSTD_EndDirective end, std::string & out) {
        if (!initialized_) return false;
        ZSTD_inBuffer in{data, len, 0};
        uint8_t buf[16 * 1024];
        while (true) {
            ZSTD_outBuffer outb{buf, sizeof(buf), 0};
            size_t rc = ZSTD_compressStream2(cctx_, &outb, &in, end);
            if (ZSTD_isError(rc)) return false;
            if (outb.pos > 0) {
                out.append(reinterpret_cast<const char *>(buf), outb.pos);
            }
            if (end == ZSTD_e_end) {
                if (rc == 0) return true;        // frame complete
                // else: more output pending, loop to drain.
                continue;
            }
            // ZSTD_e_continue: stop once input is consumed AND the
            // encoder didn't fill our buffer (would have wanted more
            // out-space if there was buffered data).
            if (in.pos == in.size && outb.pos < outb.size) return true;
        }
    }

private:
    ZSTD_CStream * cctx_;
    bool           initialized_;
};
#endif // LLAMA_HAVE_CODEC_ZSTD

static void process_handler_response(server_http_req_ptr && request, server_http_res_ptr & response, httplib::Response & res) {
    if (response->is_stream()) {
        res.status = response->status;
        // Tell Nginx to not buffer any streamed response
        response->headers["X-Accel-Buffering"] = "no";
        set_headers(res, response->headers);
        const std::string content_type = response->content_type;
        // convert to shared_ptr as both chunked_content_provider() and on_complete() need to use it
        std::shared_ptr<server_http_req> q_ptr = std::move(request);
        std::shared_ptr<server_http_res> r_ptr = std::move(response);

        // Which encoding did the handler negotiate? Codec v0.4 preference
        // order is zstd > br > gzip > identity (see spec/versions/v0.4.md
        // §Transport-Compression). Dispatch in the same order; each branch
        // is independently compiled out if the corresponding encoder lib
        // wasn't found at configure time.
        const auto enc_it = r_ptr->headers.find("Content-Encoding");
        const std::string enc_value =
            (enc_it != r_ptr->headers.end()) ? enc_it->second : std::string();

#if defined(LLAMA_HAVE_CODEC_ZSTD)
        if (enc_value == "zstd") {
            // The negotiator has already verified codec_zstd_dict_has(stream_format);
            // pull the dict bytes for the response's content_type-implied format.
            // The stream_format → dict mapping is keyed by "msgpack" / "protobuf"
            // (matches the request's stream_format field). content_type was set by
            // the Codec handler to application/x-{msgpack,protobuf}, so we recover
            // the format from the suffix.
            std::string fmt;
            if (content_type == "application/x-msgpack")       fmt = "msgpack";
            else if (content_type == "application/x-protobuf") fmt = "protobuf";

            std::string dict_bytes_copy;
            if (!fmt.empty() && codec_zstd_dict_has(fmt)) {
                dict_bytes_copy = codec_zstd_dict_bytes(fmt);
            }

            auto zs = !dict_bytes_copy.empty()
                ? std::make_shared<codec_zstd_streamer>(dict_bytes_copy)
                : std::shared_ptr<codec_zstd_streamer>();
            if (!zs || !zs->ok()) {
                // Negotiator promised a dict but the registry now disagrees,
                // or ZSTD_CCtx_loadDictionary rejected it. Strip the header
                // (and the dict-naming companion) and stream identity rather
                // than serving a corrupt zstd response.
                r_ptr->headers.erase("Content-Encoding");
                r_ptr->headers.erase("Codec-Zstd-Dict");
                res.set_header("Content-Encoding", ""); // no-op safety
                res.set_header("Codec-Zstd-Dict",  "");
                // Fall through to identity below.
            } else {
                const auto zstd_provider = [response = r_ptr, zs](size_t, httplib::DataSink & sink) -> bool {
                    std::string raw;
                    bool has_next = response->next(raw);
                    std::string encoded;
                    ZSTD_EndDirective end = has_next ? ZSTD_e_continue : ZSTD_e_end;
                    if (!zs->compress_append(raw.data(), raw.size(), end, encoded)) {
                        return false;
                    }
                    if (!encoded.empty()) {
                        if (!sink.write(encoded.data(), encoded.size())) {
                            return false;
                        }
                    }
                    if (!has_next) {
                        sink.done();
                        SRV_DBG("%s", "http: zstd stream ended\n");
                    }
                    return has_next;
                };
                const auto on_complete = [request = q_ptr, response = r_ptr, zs](bool) mutable {
                    zs.reset();
                    response.reset();
                    request.reset();
                };
                res.set_chunked_content_provider(content_type, zstd_provider, on_complete);
                return;
            }
        }
#endif // LLAMA_HAVE_CODEC_ZSTD

#if defined(LLAMA_HAVE_CODEC_BROTLI)
        if (enc_value == "br") {
            auto br = std::make_shared<codec_brotli_streamer>();
            if (!br->ok()) {
                // BrotliEncoderCreateInstance / setParameter failure — strip the
                // header and stream identity rather than serving a corrupt response.
                r_ptr->headers.erase("Content-Encoding");
                res.set_header("Content-Encoding", "");
            } else {
                const auto br_provider = [response = r_ptr, br](size_t, httplib::DataSink & sink) -> bool {
                    std::string raw;
                    bool has_next = response->next(raw);
                    std::string encoded;
                    // PROCESS per chunk (NO per-chunk flush — see v0.4.1
                    // regression note on codec_brotli_streamer); FINISH on the
                    // last call so the brotli stream terminates cleanly.
                    BrotliEncoderOperation op = has_next ? BROTLI_OPERATION_PROCESS : BROTLI_OPERATION_FINISH;
                    if (!br->compress_append(raw.data(), raw.size(), op, encoded)) {
                        return false;
                    }
                    if (!encoded.empty()) {
                        if (!sink.write(encoded.data(), encoded.size())) {
                            return false;
                        }
                    }
                    if (!has_next) {
                        sink.done();
                        SRV_DBG("%s", "http: brotli stream ended\n");
                    }
                    return has_next;
                };
                const auto on_complete = [request = q_ptr, response = r_ptr, br](bool) mutable {
                    br.reset();
                    response.reset();
                    request.reset();
                };
                res.set_chunked_content_provider(content_type, br_provider, on_complete);
                return;
            }
        }
#endif // LLAMA_HAVE_CODEC_BROTLI

        const bool want_gzip =
#if defined(LLAMA_HAVE_CODEC_GZIP)
            (enc_value == "gzip");
#else
            false;
#endif

        if (want_gzip) {
#if defined(LLAMA_HAVE_CODEC_GZIP)
            auto gz = std::make_shared<codec_gzip_streamer>();
            if (!gz->ok()) {
                // zlib init failure — strip the header and stream identity
                // rather than serving a corrupt response.
                r_ptr->headers.erase("Content-Encoding");
                res.set_header("Content-Encoding", ""); // no-op safety
            } else {
                const auto gzipped_provider = [response = r_ptr, gz](size_t, httplib::DataSink & sink) -> bool {
                    std::string raw;
                    bool has_next = response->next(raw);
                    std::string deflated;
                    // Z_SYNC_FLUSH per chunk so the client sees bytes
                    // promptly (preserves Codec's TTFT property).
                    int flush = has_next ? Z_SYNC_FLUSH : Z_FINISH;
                    if (!gz->deflate_append(raw.data(), raw.size(), flush, deflated)) {
                        return false;
                    }
                    if (!deflated.empty()) {
                        if (!sink.write(deflated.data(), deflated.size())) {
                            return false;
                        }
                    }
                    if (!has_next) {
                        sink.done();
                        SRV_DBG("%s", "http: gzip stream ended\n");
                    }
                    return has_next;
                };
                const auto on_complete = [request = q_ptr, response = r_ptr, gz](bool) mutable {
                    gz.reset();
                    response.reset();
                    request.reset();
                };
                res.set_chunked_content_provider(content_type, gzipped_provider, on_complete);
                return;
            }
#endif
        }

        const auto chunked_content_provider = [response = r_ptr](size_t, httplib::DataSink & sink) -> bool {
            std::string chunk;
            const bool has_next = response->next(chunk);
            if (!chunk.empty()) {
                if (!sink.write(chunk.data(), chunk.size())) {
                    return false;
                }
                SRV_DBG("http: streamed chunk: %s\n", chunk.c_str());
            }
            if (!has_next) {
                sink.done();
                SRV_DBG("%s", "http: stream ended\n");
            }
            return has_next;
        };
        const auto on_complete = [request = q_ptr, response = r_ptr](bool) mutable {
            response->on_complete();
            response.reset();
            request.reset();
        };
        res.set_chunked_content_provider(content_type, chunked_content_provider, on_complete);
    } else {
        res.status = response->status;
        set_headers(res, response->headers);
        res.set_content(response->data, response->content_type);
        response->on_complete();
    }
}

void server_http_context::get(const std::string & path, const server_http_context::handler_t & handler) const {
    handlers.emplace(path, handler);
    pimpl->srv->Get(path_prefix + path, [handler](const httplib::Request & req, httplib::Response & res) {
        server_http_req_ptr request = std::make_unique<server_http_req>(server_http_req{
            get_params(req),
            get_headers(req),
            req.path,
            build_query_string(req),
            req.body,
            {},
            req.is_connection_closed
        });
        server_http_res_ptr response = handler(*request);
        process_handler_response(std::move(request), response, res);
    });
}

void server_http_context::post(const std::string & path, const server_http_context::handler_t & handler) const {
    handlers.emplace(path, handler);
    pimpl->srv->Post(path_prefix + path, [handler](const httplib::Request & req, httplib::Response & res) {
        std::string body = req.body;
        std::map<std::string, uploaded_file> files;

        if (req.is_multipart_form_data()) {
            // translate text fields to a JSON object and use it as the body
            json form_json = json::object();
            for (const auto & [key, field] : req.form.fields) {
                if (form_json.contains(key)) {
                    // if the key already exists, convert it to an array
                    if (!form_json[key].is_array()) {
                        json existing_value = form_json[key];
                        form_json[key] = json::array({existing_value});
                    }
                    form_json[key].push_back(field.content);
                } else {
                    form_json[key] = field.content;
                }
            }
            body = form_json.dump();

            // populate files from multipart form
            for (const auto & [key, file] : req.form.files) {
                files[key] = uploaded_file{
                    raw_buffer(file.content.begin(), file.content.end()),
                    file.filename,
                    file.content_type,
                };
            }
        }

        server_http_req_ptr request = std::make_unique<server_http_req>(server_http_req{
            get_params(req),
            get_headers(req),
            req.path,
            build_query_string(req),
            body,
            std::move(files),
            req.is_connection_closed
        });
        server_http_res_ptr response = handler(*request);
        process_handler_response(std::move(request), response, res);
    });
}

void server_http_context::del(const std::string & path, const server_http_context::handler_t & handler) const {
    handlers.emplace(path, handler);
    pimpl->srv->Delete(path_prefix + path, [handler](const httplib::Request & req, httplib::Response & res) {
        server_http_req_ptr request = std::make_unique<server_http_req>(server_http_req{
            get_params(req),
            get_headers(req),
            req.path,
            build_query_string(req),
            req.body,
            {},
            req.is_connection_closed
        });
        server_http_res_ptr response = handler(*request);
        process_handler_response(std::move(request), response, res);
    });
}

//
// Vertex AI Prediction protocol (AIP_PREDICT_ROUTE)
// https://cloud.google.com/vertex-ai/docs/predictions/custom-container-requirements
//

// Derives the camelCase @requestFormat alias for a registered path.
// e.g. "/v1/chat/completions" -> "chatCompletions", "/apply-template" -> "applyTemplate"
static std::string path_to_gcp_format(const std::string & path) {
    std::string s = path;
    if (s.size() > 3 && s[0] == '/' && s[1] == 'v' && s[2] == '1') {
        s = s.substr(3);
    }
    if (!s.empty() && s[0] == '/') {
        s = s.substr(1);
    }
    std::string result;
    bool cap = false;
    for (unsigned char c : s) {
        if (c == ':') break; // stop before path parameters
        if (c == '/' || c == '-' || c == '_') {
            cap = true;
        } else {
            result += static_cast<char>(cap ? std::toupper(c) : c);
            cap = false;
        }
    }
    return result;
}

static json parse_gcp_predict_response(const server_http_res_ptr & res) {
    if (res == nullptr) {
        throw std::runtime_error("empty response from internal handler");
    }
    if (res->is_stream()) {
        throw std::invalid_argument("predict route does not support streaming responses");
    }
    if (res->data.empty()) {
        return nullptr;
    }
    try {
        return json::parse(res->data);
    } catch (...) {
        return res->data;
    }
}

void server_http_context::register_gcp_compat() const {
    const gcp_params gcp;

    if (!gcp.enabled) {
        // do nothing
        return;
    }

    if (handlers.count(gcp.path_predict)) {
        SRV_ERR("AIP_PREDICT_ROUTE=%s conflicts with an existing llama-server route\n", gcp.path_predict.c_str());
        exit(1);
    }

    // camelCase alias -> canonical path (first registration wins on collision)
    // e.g. "chatCompletions" -> "/v1/chat/completions"
    std::unordered_map<std::string, std::string> alias_to_path;
    for (const auto & [path, _] : handlers) {
        alias_to_path.emplace(path_to_gcp_format(path), path);
    }

    if (!gcp.path_health.empty()) {
        const auto health_handler = handlers.find("/health");
        GGML_ASSERT(health_handler != handlers.end());
        get(gcp.path_health, health_handler->second);
    }

    post(gcp.path_predict, [this, alias_to_path = std::move(alias_to_path)](const server_http_req & req) -> server_http_res_ptr {
        static const auto build_error = [](const std::string & message, error_type type) -> json {
            return json {{"error", format_error_response(message, type)}};
        };

        json data;
        try {
            data = json::parse(req.body);
        } catch (const std::exception & e) {
            auto res = std::make_unique<server_http_res>();
            res->status = 400;
            res->data = safe_json_to_str({{"error", format_error_response(e.what(), ERROR_TYPE_INVALID_REQUEST)}});
            return res;
        }
        if (!data.is_object()) {
            auto res = std::make_unique<server_http_res>();
            res->status = 400;
            res->data = safe_json_to_str({{"error", format_error_response("request body must be a JSON object", ERROR_TYPE_INVALID_REQUEST)}});
            return res;
        }
        if (!data.contains("instances") || !data.at("instances").is_array()) {
            auto res = std::make_unique<server_http_res>();
            res->status = 400;
            res->data = safe_json_to_str({{"error", format_error_response("request body must include an array field named instances", ERROR_TYPE_INVALID_REQUEST)}});
            return res;
        }

        const json & instances = data.at("instances");
        static const size_t MAX_INSTANCES = 128;
        if (instances.size() > MAX_INSTANCES) {
            auto res = std::make_unique<server_http_res>();
            res->status = 400;
            res->data = safe_json_to_str({{"error", format_error_response("instances array exceeds maximum size of " + std::to_string(MAX_INSTANCES), ERROR_TYPE_INVALID_REQUEST)}});
            return res;
        }

        std::vector<std::future<json>> futures;
        futures.reserve(instances.size());

        for (const auto & instance : instances) {
            futures.push_back(std::async(std::launch::async, [this, &req, &alias_to_path, instance]() -> json {
                if (!instance.is_object()) {
                    return build_error("each instance must be a JSON object", ERROR_TYPE_INVALID_REQUEST);
                }
                if (!instance.contains("@requestFormat") || !instance.at("@requestFormat").is_string()) {
                    return build_error("each instance must include a string @requestFormat", ERROR_TYPE_INVALID_REQUEST);
                }

                try {
                    json payload = instance;
                    const std::string format = payload.at("@requestFormat").get<std::string>();
                    payload.erase("@requestFormat");

                    if (payload.contains("stream")) {
                        SRV_WRN("%s", "ignoring client-provided stream field in instance, streaming is not supported in predict route\n");
                        payload["stream"] = false;
                    }

                    // accept both camelCase aliases (e.g. "chatCompletions") and direct paths
                    std::string dispatch_path;
                    auto it_alias = alias_to_path.find(format);
                    if (it_alias != alias_to_path.end()) {
                        dispatch_path = it_alias->second;
                    } else if (handlers.count(format)) {
                        dispatch_path = format;
                    } else {
                        return build_error("no handler registered for @requestFormat: " + format, ERROR_TYPE_INVALID_REQUEST);
                    }

                    const server_http_req internal_req {
                        req.params,
                        req.headers,
                        path_prefix + dispatch_path,
                        req.query_string,
                        payload.dump(),
                        {},
                        req.should_stop,
                    };

                    server_http_res_ptr internal_res = handlers.at(dispatch_path)(internal_req);
                    return parse_gcp_predict_response(internal_res);
                } catch (const std::invalid_argument & e) {
                    return build_error(e.what(), ERROR_TYPE_INVALID_REQUEST);
                } catch (const std::exception & e) {
                    return build_error(e.what(), ERROR_TYPE_SERVER);
                } catch (...) {
                    return build_error("unknown error", ERROR_TYPE_SERVER);
                }
            }));
        }

        json predictions = json::array();
        for (auto & future : futures) {
            predictions.push_back(future.get());
        }

        auto res = std::make_unique<server_http_res>();
        res->data = safe_json_to_str({{"predictions", predictions}});
        return res;
    });
}
