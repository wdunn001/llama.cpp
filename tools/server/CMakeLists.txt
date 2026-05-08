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
    server-tools.cpp
    server-tools.h
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
# server-http.cpp (the llama-server target below) which links zlib. Without
# this define, the negotiation block is compiled out and the server never
# advertises Content-Encoding: gzip — making the gzip middleware a no-op.
if (LLAMA_HAVE_CODEC_GZIP)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_GZIP=1)
endif()


# llama-server executable

set(TARGET llama-server)

set(TARGET_SRCS
    server.cpp
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
)

option(LLAMA_BUILD_WEBUI "Build the embedded Web UI" ON)

if (LLAMA_BUILD_WEBUI)
    set(PUBLIC_ASSETS
        index.html
        bundle.js
        bundle.css
        loading.html
    )

    foreach(asset ${PUBLIC_ASSETS})
        set(input "${CMAKE_CURRENT_SOURCE_DIR}/public/${asset}")
        set(output "${CMAKE_CURRENT_BINARY_DIR}/${asset}.hpp")
        list(APPEND TARGET_SRCS ${output})
        add_custom_command(
            DEPENDS "${input}"
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${input}" "-DOUTPUT=${output}" -P "${PROJECT_SOURCE_DIR}/scripts/xxd.cmake"
        )
        set_source_files_properties(${output} PROPERTIES GENERATED TRUE)
    endforeach()
    add_definitions(-DLLAMA_BUILD_WEBUI)
else()
endif()

add_executable(${TARGET} ${TARGET_SRCS})
install(TARGETS ${TARGET} RUNTIME)

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PRIVATE server-context PUBLIC llama-common cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

if (LLAMA_HAVE_CODEC_GZIP)
    target_compile_definitions(${TARGET} PRIVATE LLAMA_HAVE_CODEC_GZIP=1)
    target_link_libraries(${TARGET} PRIVATE ZLIB::ZLIB)
endif()

target_compile_features(${TARGET} PRIVATE cxx_std_17)
