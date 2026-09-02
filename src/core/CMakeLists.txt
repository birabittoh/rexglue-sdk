# rexcore - Core utilities library
# Foundation types, data structures, and threading primitives

add_library(rexcore OBJECT
    arena.cpp
    bit_map.cpp
    bit_stream.cpp
    byte_stream.cpp
    clock.cpp
    cvar.cpp
    exception_handler.cpp
    filesystem.cpp
    filesystem_wildcard.cpp
    hash.cpp
    logging.cpp
    memory.cpp
    mutex.cpp
    ring_buffer.cpp
    string_buffer.cpp
    threading.cpp
    timer_queue.cpp
    utf8.cpp
    vec128.cpp
    filesystem_zip.cpp
    filesystem_tar.cpp
)
add_library(rex::core ALIAS rexcore)

if(APPLE)
    # Apple's libc++ still gates std::jthread/std::stop_token behind the
    # experimental-library feature flag. Only timer_queue.cpp uses them, and
    # neither type crosses a header boundary.
    set_source_files_properties(timer_queue.cpp PROPERTIES
        COMPILE_OPTIONS "-fexperimental-library")
endif()

# Platform-specific sources
if(WIN32)
    target_sources(rexcore PRIVATE
        atomic_win.cpp
        clock_win.cpp
        dbg_win.cpp
        dynlib_win.cpp
        exception_handler_win.cpp
        filesystem_win.cpp
        fiber_win32.cpp
        mapped_memory_win.cpp
        math_msvc.cpp
        memory_win.cpp
        platform/console_win.cpp
        platform/env_win.cpp
        platform/process_win.cpp
        seh_win.cpp
        socket_win.cpp
        system_win.cpp
        threading_win.cpp
        net/http_win.cpp
    )
    target_compile_definitions(rexcore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(rexcore PUBLIC winhttp)
elseif(UNIX)
    # Linux and macOS/Darwin share the POSIX sources; Darwin-specific behavior
    # is selected inside them with #if defined(__APPLE__).
    target_sources(rexcore PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        dbg_posix.cpp
        dynlib_posix.cpp
        exception_handler_posix.cpp
        filesystem_posix.cpp
        mapped_memory_posix.cpp
        math_gcc.cpp
        memory_posix.cpp
        platform/console_posix.cpp
        platform/env_posix.cpp
        platform/process_posix.cpp
        seh_posix.cpp
        socket_posix.cpp
        system_posix.cpp
        threading_posix.cpp
    )
    if(ANDROID)
        # Android's Bionic lacks getcontext/makecontext/swapcontext.
        target_sources(rexcore PRIVATE fiber_android.cpp)
    else()
        target_sources(rexcore PRIVATE fiber_posix.cpp)
    endif()
    if(ANDROID)
        # Android (Bionic) doesn't ship libcurl. Stub out the HTTP client; the
        # mod catalog won't work on Android until a Java/JNI HTTP backend is
        # added. Android's Bionic bundles librt into libc, so no separate -lrt.
        target_sources(rexcore PRIVATE net/http_stub.cpp)
        target_link_libraries(rexcore PRIVATE log)
    else()
        target_sources(rexcore PRIVATE net/http_curl.cpp)
        # Linked directly (not vendored) per the mod-catalog HTTP client design:
        # this fails configure loudly on a Linux box missing libcurl dev headers
        # rather than silently shipping a build with no catalog support.
        find_package(CURL REQUIRED)
        target_link_libraries(rexcore PUBLIC CURL::libcurl)
    endif()
endif()

# Xenos data-format layer (stateless; headers under include/rex/graphics/).
# The GPU emulation consuming it lives in the rexgpu-xenos plugin.
target_sources(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/xenos.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/info.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/info_formats.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/extent.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/util.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/conversion.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/replacement.cpp
)

target_include_directories(rexcore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include
)

if(APPLE)
    target_link_libraries(rexcore PRIVATE pthread)
elseif(ANDROID)
    # Bionic bundles POSIX real-time functions into libc; only dl is separate.
    target_link_libraries(rexcore PRIVATE dl)
elseif(UNIX)
    target_link_libraries(rexcore PRIVATE pthread rt dl)
endif()

target_link_libraries(rexcore
    PUBLIC
        fmt::fmt
        simde
        tomlplusplus
        spdlog::spdlog
        # rex/hash.h is a public header and pulls in <xxhash.h> itself (XXH_INLINE_ALL),
        # so consumers of that header need xxHash's include dir too, not just rexcore.
        xxHash::xxhash
    PRIVATE
        disruptorplus
        utf8cpp
        stb_image
        stb_image_write
        miniz
)

if(REXGLUE_USE_D3D12)
    target_compile_definitions(rexcore PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_compile_definitions(rexcore PUBLIC REX_HAS_VULKAN=1)
endif()

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexcore PUBLIC
        $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
    target_compile_definitions(rexcore PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_sources(rexcore PRIVATE perf/counter.cpp)
    target_compile_definitions(rexcore PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()
