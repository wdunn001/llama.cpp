include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# ── Codec binary streaming (optional, opt-in at runtime) ─────────────────
#
# The Codec protocol (https://github.com/wdunn001/Codec) lets clients ask
# for a binary token-ID stream instead of JSON SSE — ~14× wire reduction
# uncompressed, ~45× with Content-Encoding: br. Fully backwards compatible:
# clients that don't pass `stream_format` get the existing SSE path.
#
# We try three sources in order, all controlled by LLAMA_CODEC:
#   AUTO (default)  prefer system-installed libcodec (vcpkg / OS package
#                   manager), fall back to FetchContent from GitHub
#   SYSTEM          fail if no system-installed libcodec is found
#   FETCH           always FetchContent
#   OFF             disable the binary streaming endpoint entirely
set(LLAMA_CODEC AUTO CACHE STRING "Codec binary streaming support: AUTO|SYSTEM|FETCH|OFF")
set_property(CACHE LLAMA_CODEC PROPERTY STRINGS AUTO SYSTEM FETCH OFF)

set(LLAMA_CODEC_TARGET "")
if (NOT LLAMA_CODEC STREQUAL "OFF")
    if (LLAMA_CODEC STREQUAL "AUTO" OR LLAMA_CODEC STREQUAL "SYSTEM")
        find_package(codec CONFIG QUIET)
        if (codec_FOUND)
            set(LLAMA_CODEC_TARGET codec::codec)
            message(STATUS "Codec: using system-installed libcodec")
        elseif (LLAMA_CODEC STREQUAL "SYSTEM")
            message(FATAL_ERROR "LLAMA_CODEC=SYSTEM but find_package(codec) failed.\n"
                "Install via vcpkg (`vcpkg install codec`) or set LLAMA_CODEC=FETCH.")
        endif()
    endif()

    if (NOT LLAMA_CODEC_TARGET AND (LLAMA_CODEC STREQUAL "AUTO" OR LLAMA_CODEC STREQUAL "FETCH"))
        include(FetchContent)
        FetchContent_Declare(codec
            GIT_REPOSITORY https://github.com/wdunn001/Codec.git
            GIT_TAG        main
            SOURCE_SUBDIR  packages/c)
        # libcodec respects PROJECT_IS_TOP_LEVEL — it won't drag tests/examples
        # into the parent build, just the library targets.
        set(CODEC_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(CODEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CODEC_INSTALL        OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(codec)
        # libcodec creates `codec::codec` as the universal alias (resolves
        # to whichever of shared/static was built). Prefer the static
        # target when explicitly available so we don't drag a DLL alongside
        # llama-server, but fall back to `codec_static` (no namespace) if
        # the alias didn't propagate through FetchContent's add_subdirectory.
        if (TARGET codec::codec_static)
            set(LLAMA_CODEC_TARGET codec::codec_static)
        elseif (TARGET codec_static)
            set(LLAMA_CODEC_TARGET codec_static)
        elseif (TARGET codec::codec)
            set(LLAMA_CODEC_TARGET codec::codec)
        else()
            message(FATAL_ERROR "Codec: FetchContent succeeded but no usable target found.")
        endif()
        message(STATUS "Codec: vendoring libcodec via FetchContent (target=${LLAMA_CODEC_TARGET})")
    endif()
endif()

# ── Codec response compression (optional) ───────────────────────────────
#
# Streaming gzip wrapper for the chunked content provider. Lifts Codec
# wire reduction from format-only ~17–25× to compressed ~700×+ at 2K
# tokens (matches sglang's gzip cell on the cross-stack matrix in
# Codec/packages/bench/RESULTS.md §1f). Pure HTTP-layer change; does
# nothing for non-Codec endpoints (which never set Content-Encoding).
#
#   AUTO (default)  enable gzip when zlib is available; silently disable
#                   otherwise (server still ships, codec runs identity)
#   ON              fail configure if zlib is missing
#   OFF             never compress, even if zlib is around
set(LLAMA_CODEC_GZIP AUTO CACHE STRING "Codec response gzip compression: AUTO|ON|OFF")
set_property(CACHE LLAMA_CODEC_GZIP PROPERTY STRINGS AUTO ON OFF)

set(LLAMA_HAVE_CODEC_GZIP 0)
if (NOT LLAMA_CODEC_GZIP STREQUAL "OFF")
    if (LLAMA_CODEC_GZIP STREQUAL "ON")
        find_package(ZLIB REQUIRED)
    else()
        find_package(ZLIB QUIET)
    endif()
    if (ZLIB_FOUND)
        set(LLAMA_HAVE_CODEC_GZIP 1)
        message(STATUS "Codec: streaming gzip enabled (zlib ${ZLIB_VERSION_STRING})")
    else()
        message(STATUS "Codec: zlib not found — streaming gzip disabled (Codec frames remain wire-only)")
    endif()
endif()

# Streaming Brotli encoder. Used by the same chunked-content wrapper as
# gzip; selected when the client's Accept-Encoding lists `br` and the
# Codec v0.4 negotiator picks it (preference order: zstd > br > gzip).
# Brotli at quality 4 matches gzip's per-chunk CPU cost while emitting
# ~20% fewer bytes on msgpack Codec streams (RESULTS.md §1f, 2.79 vs
# 3.47 B/token at 1-token chunks). Linked against libbrotlienc only —
# the decoder lives in the client.
#
#   AUTO (default)  pkg-config libbrotlienc, then CMake's BrotliEnc;
#                   silently disable if neither finds the lib
#   ON              FATAL_ERROR if libbrotlienc is missing
#   OFF             never compress with brotli even when available
set(LLAMA_CODEC_BROTLI AUTO CACHE STRING "Codec response brotli compression: AUTO|ON|OFF")
set_property(CACHE LLAMA_CODEC_BROTLI PROPERTY STRINGS AUTO ON OFF)

set(LLAMA_HAVE_CODEC_BROTLI 0)
set(LLAMA_CODEC_BROTLI_TARGET "")
set(LLAMA_CODEC_BROTLI_VERSION "")
if (NOT LLAMA_CODEC_BROTLI STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(BROTLIENC QUIET libbrotlienc)
    endif()
    if (BROTLIENC_FOUND)
        # Wrap the pkg-config result in an INTERFACE target so the
        # downstream `target_link_libraries(... BrotliEnc::ours)` call
        # matches the find_package branch.
        if (NOT TARGET LlamaCodec::BrotliEnc)
            add_library(LlamaCodec::BrotliEnc INTERFACE IMPORTED)
            target_include_directories(LlamaCodec::BrotliEnc INTERFACE ${BROTLIENC_INCLUDE_DIRS})
            target_link_libraries(LlamaCodec::BrotliEnc INTERFACE ${BROTLIENC_LIBRARIES})
            if (BROTLIENC_LIBRARY_DIRS)
                target_link_directories(LlamaCodec::BrotliEnc INTERFACE ${BROTLIENC_LIBRARY_DIRS})
            endif()
        endif()
        set(LLAMA_CODEC_BROTLI_TARGET LlamaCodec::BrotliEnc)
        set(LLAMA_CODEC_BROTLI_VERSION ${BROTLIENC_VERSION})
    else()
        # pkg-config may be unavailable on Windows / minimal containers;
        # fall back to CMake's own BrotliEnc package which ships with
        # google/brotli >= 1.1.
        find_package(BrotliEnc QUIET)
        if (BrotliEnc_FOUND OR TARGET BrotliEnc)
            if (TARGET BrotliEnc)
                set(LLAMA_CODEC_BROTLI_TARGET BrotliEnc)
            elseif (TARGET Brotli::brotlienc)
                set(LLAMA_CODEC_BROTLI_TARGET Brotli::brotlienc)
            endif()
        endif()
    endif()

    if (LLAMA_CODEC_BROTLI_TARGET)
        set(LLAMA_HAVE_CODEC_BROTLI 1)
        if (LLAMA_CODEC_BROTLI_VERSION)
            message(STATUS "Codec: streaming brotli enabled (libbrotlienc ${LLAMA_CODEC_BROTLI_VERSION})")
        else()
            message(STATUS "Codec: streaming brotli enabled (libbrotlienc)")
        endif()
    else()
        if (LLAMA_CODEC_BROTLI STREQUAL "ON")
            message(FATAL_ERROR
                "LLAMA_CODEC_BROTLI=ON but libbrotlienc was not found.\n"
                "Install via: apt install libbrotli-dev (Debian/Ubuntu),\n"
                "brew install brotli (macOS), or vcpkg install brotli (Windows).")
        else()
            message(STATUS "Codec: libbrotlienc not found — streaming brotli disabled")
        endif()
    endif()
endif()

# Streaming Zstandard encoder. Selected by the v0.4 negotiator only
# when (a) the client advertises `zstd`, (b) this build linked libzstd,
# and (c) a pre-trained dict has been loaded for the request's
# `stream_format` via CODEC_ZSTD_DICT_{MSGPACK,PROTOBUF}_PATH env vars.
# Per spec/versions/v0.4.md §Pre-trained ZSTD dictionaries, no-dict
# zstd is the worst-of-both-worlds on Codec streams (same bytes as
# gzip; 334× TTFB regression on buffered middleware), so the dict-gate
# is mandatory.
#
#   AUTO (default)  pkg-config libzstd, then CMake's zstd;
#                   silently disable if neither finds the lib
#   ON              FATAL_ERROR if libzstd is missing
#   OFF             never compress with zstd even when available
set(LLAMA_CODEC_ZSTD AUTO CACHE STRING "Codec response zstd compression: AUTO|ON|OFF")
set_property(CACHE LLAMA_CODEC_ZSTD PROPERTY STRINGS AUTO ON OFF)

set(LLAMA_HAVE_CODEC_ZSTD 0)
set(LLAMA_CODEC_ZSTD_TARGET "")
set(LLAMA_CODEC_ZSTD_VERSION "")
if (NOT LLAMA_CODEC_ZSTD STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(LIBZSTD QUIET libzstd)
    endif()
    if (LIBZSTD_FOUND)
        if (NOT TARGET LlamaCodec::Zstd)
            add_library(LlamaCodec::Zstd INTERFACE IMPORTED)
            target_include_directories(LlamaCodec::Zstd INTERFACE ${LIBZSTD_INCLUDE_DIRS})
            target_link_libraries(LlamaCodec::Zstd INTERFACE ${LIBZSTD_LIBRARIES})
            if (LIBZSTD_LIBRARY_DIRS)
                target_link_directories(LlamaCodec::Zstd INTERFACE ${LIBZSTD_LIBRARY_DIRS})
            endif()
        endif()
        set(LLAMA_CODEC_ZSTD_TARGET LlamaCodec::Zstd)
        set(LLAMA_CODEC_ZSTD_VERSION ${LIBZSTD_VERSION})
    else()
        find_package(zstd QUIET)
        if (zstd_FOUND OR TARGET zstd::libzstd_shared OR TARGET zstd::libzstd_static)
            if (TARGET zstd::libzstd_static)
                set(LLAMA_CODEC_ZSTD_TARGET zstd::libzstd_static)
            elseif (TARGET zstd::libzstd_shared)
                set(LLAMA_CODEC_ZSTD_TARGET zstd::libzstd_shared)
            elseif (TARGET zstd::libzstd)
                set(LLAMA_CODEC_ZSTD_TARGET zstd::libzstd)
            endif()
        endif()
    endif()

    if (LLAMA_CODEC_ZSTD_TARGET)
        set(LLAMA_HAVE_CODEC_ZSTD 1)
        if (LLAMA_CODEC_ZSTD_VERSION)
            message(STATUS "Codec: streaming zstd enabled (libzstd ${LLAMA_CODEC_ZSTD_VERSION})")
        else()
            message(STATUS "Codec: streaming zstd enabled (libzstd)")
        endif()
    else()
        if (LLAMA_CODEC_ZSTD STREQUAL "ON")
            message(FATAL_ERROR
                "LLAMA_CODEC_ZSTD=ON but libzstd was not found.\n"
                "Install via: apt install libzstd-dev (Debian/Ubuntu),\n"
                "brew install zstd (macOS), or vcpkg install zstd (Windows).")
        else()
            message(STATUS "Codec: libzstd not found — streaming zstd disabled")
        endif()
    endif()
endif()

# server-context containing the core server logic, used by llama-server and CLI

set(TARGET server-context)

add_library(${TARGET} STATIC
    server-chat.cpp
    server-chat.h
    server-task.cpp
    server-task.h
    server-queue.cpp
    server-queue.h
    server-common.cpp
    server-common.h
    server-context.cpp
    server-context.h
    server-stream.cpp
    server-stream.h
    server-tools.cpp
    server-tools.h
    server-mcp.cpp
    server-mcp.h
    server-schema.cpp
    server-schema.h
    codec_zstd_dict_registry.cpp
    codec_zstd_dict_registry.hpp
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC llama-common mtmd ${CMAKE_THREAD_LIBS_INIT})

if (LLAMA_CODEC_TARGET)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC=1)
    target_link_libraries(${TARGET} PRIVATE ${LLAMA_CODEC_TARGET})
endif()

# server-context.cpp's Accept-Encoding negotiation block lives in this
# static lib; it needs LLAMA_HAVE_CODEC_GZIP defined too so the
# Content-Encoding response header can land. The actual deflate runs in
# server-http.cpp (part of the llama-server-impl library below) which
# links zlib. Without this define, the negotiation block is compiled out
# and the server never advertises Content-Encoding: gzip — making the
# gzip middleware a no-op.
if (LLAMA_HAVE_CODEC_GZIP)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_GZIP=1)
endif()
# Same propagation rule for brotli + zstd. The negotiator gates each
# encoding behind its respective LLAMA_HAVE_CODEC_* define; the actual
# encoders link into server-http.cpp, part of llama-server-impl below.
if (LLAMA_HAVE_CODEC_BROTLI)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_BROTLI=1)
endif()
if (LLAMA_HAVE_CODEC_ZSTD)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_ZSTD=1)
endif()

# llama-server-impl: server logic, reusable by app

set(TARGET llama-server-impl)

add_library(${TARGET}
    server.cpp
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

# server.cpp / server-http.cpp / server-models.cpp are compiled into this
# library now (upstream split them out of the llama-server executable),
# and server-http.cpp is where the Codec streaming wrapper actually calls
# the gzip/brotli/zstd encoders (see codec_version.hpp and the Codec
# compression negotiation in server-context.cpp for the define gate).
if (LLAMA_HAVE_CODEC_GZIP)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_GZIP=1)
    target_link_libraries(${TARGET} PRIVATE ZLIB::ZLIB)
endif()

# Brotli encoder link + define for the streaming wrapper in
# server-http.cpp. The wrapper is compiled conditionally on the define,
# so the link only happens when the lib was actually found.
if (LLAMA_HAVE_CODEC_BROTLI)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_BROTLI=1)
    target_link_libraries(${TARGET} PRIVATE ${LLAMA_CODEC_BROTLI_TARGET})
endif()

# Zstd encoder link + define. The streaming wrapper loads a pre-trained
# dict per request from the codec_zstd_dict_registry (linked into
# server-context above).
if (LLAMA_HAVE_CODEC_ZSTD)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_ZSTD=1)
    target_link_libraries(${TARGET} PRIVATE ${LLAMA_CODEC_ZSTD_TARGET})
endif()

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
