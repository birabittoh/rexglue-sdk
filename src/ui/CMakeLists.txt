# rexui

# Core UI sources
set(REXUI_CORE_SOURCES
    graphics_upload_buffer_pool.cpp
    graphics_util.cpp
    imgui_dialog.cpp
    imgui_drawer.cpp
    immediate_drawer.cpp
    keybinds.cpp
    menu_item.cpp
    presenter.cpp
    renderdoc_api.cpp
    window.cpp
    windowed_app.cpp
    windowed_app_context.cpp
)

# Optional overlay dialogs
set(REXUI_OVERLAY_SOURCES
    overlay/debug_overlay.cpp
    overlay/console_overlay.cpp
    overlay/settings_overlay.cpp
)

# Vulkan UI backend
set(REXUI_VULKAN_SOURCES
    vulkan/linked_type_descriptor_set_allocator.cpp
    vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_tools_context.cpp
    vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_immediate_drawer.cpp
    vulkan/vulkan_instance.cpp
    vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_presenter.cpp
    vulkan/vulkan_provider.cpp
    vulkan/vulkan_submission_tracker.cpp
    vulkan/vulkan_upload_buffer_pool.cpp
    vulkan/vulkan_util.cpp
)

# D3D12 UI backend
if(REXGLUE_USE_D3D12)
    set(REXUI_D3D12_SOURCES
        d3d12/d3d12_cpu_descriptor_pool.cpp
        d3d12/d3d12_descriptor_heap_pool.cpp
        d3d12/d3d12_immediate_drawer.cpp
        d3d12/d3d12_presenter.cpp
        d3d12/d3d12_provider.cpp
        d3d12/d3d12_submission_tracker.cpp
        d3d12/d3d12_upload_buffer_pool.cpp
        d3d12/d3d12_util.cpp
    )
endif()

# Platform-specific sources
if(WIN32)
    set(REXUI_PLATFORM_SOURCES
        surface_win.cpp
        window_win.cpp
        windowed_app_context_win.cpp
    )
else()
    set(REXUI_PLATFORM_SOURCES
        surface_gnulinux.cpp
        window_gtk.cpp
        windowed_app_context_gtk.cpp
    )
endif()

# Combine sources
set(REXUI_SOURCES
    ${REXUI_CORE_SOURCES}
    ${REXUI_OVERLAY_SOURCES}
    ${REXUI_PLATFORM_SOURCES}
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXUI_SOURCES ${REXUI_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXUI_SOURCES ${REXUI_D3D12_SOURCES})
endif()

add_library(rexui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rexui)

target_include_directories(rexui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexui PRIVATE
    ${REXGLUE_ROOT}/src  # For ui/shaders/bytecode includes
)

target_link_libraries(rexui PUBLIC
    rexcore
    renderdoc
    imgui::imgui
)

if(REXGLUE_FIDELITYFX_SOURCE_DIR AND EXISTS "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include")
    target_include_directories(rexui PRIVATE
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include"
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/ffx-api/include"
    )
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_SDK=1)
endif()

set(REXUI_HAS_FIDELITYFX_RUNTIME OFF)
if(TARGET amd_fidelityfx_vk)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_vk)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(TARGET amd_fidelityfx_dx12)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_dx12)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(REXUI_HAS_FIDELITYFX_RUNTIME)
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_RUNTIME=1)
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexui PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
endif()

# Platform-specific dependencies
if(WIN32)
    target_link_libraries(rexui PUBLIC
        dwmapi
        Shcore
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)

    target_include_directories(rexui PRIVATE
        ${GTK3_INCLUDE_DIRS}
        ${X11_XCB_INCLUDE_DIRS}
    )
    target_link_libraries(rexui PUBLIC
        ${GTK3_LIBRARIES}
        ${X11_XCB_LIBRARIES}
    )

    if(REXGLUE_ENABLE_WAYLAND)
        pkg_check_modules(WAYLAND_CLIENT wayland-client)
        pkg_check_modules(GTK3_WAYLAND gtk+-wayland-3.0)
        pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
        find_program(WAYLAND_SCANNER_EXEC NAMES wayland-scanner)
        if(WAYLAND_CLIENT_FOUND AND GTK3_WAYLAND_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER_EXEC)
            message(STATUS "ReXGlue: native Wayland support enabled")
            target_compile_definitions(rexui PUBLIC REX_HAS_WAYLAND=1)
            target_include_directories(rexui PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
            target_link_libraries(rexui PUBLIC ${WAYLAND_CLIENT_LIBRARIES})

            # Generate code for unstable protocols needed for relative pointer
            # motion + cursor lock (required for FPS-style mouse capture on
            # Wayland, where pointer warping is forbidden).
            pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
            set(WAYLAND_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-gen)
            file(MAKE_DIRECTORY ${WAYLAND_GEN_DIR})

            function(_rexglue_wl_protocol name xml_relpath)
                set(xml ${WAYLAND_PROTOCOLS_DIR}/${xml_relpath})
                set(header ${WAYLAND_GEN_DIR}/${name}-client-protocol.h)
                set(source ${WAYLAND_GEN_DIR}/${name}-protocol.c)
                add_custom_command(
                    OUTPUT ${header}
                    COMMAND ${WAYLAND_SCANNER_EXEC} client-header ${xml} ${header}
                    DEPENDS ${xml}
                    VERBATIM)
                add_custom_command(
                    OUTPUT ${source}
                    COMMAND ${WAYLAND_SCANNER_EXEC} private-code ${xml} ${source}
                    DEPENDS ${xml}
                    VERBATIM)
                target_sources(rexui PRIVATE ${header} ${source})
            endfunction()

            _rexglue_wl_protocol(relative-pointer-unstable-v1 unstable/relative-pointer/relative-pointer-unstable-v1.xml)
            _rexglue_wl_protocol(pointer-constraints-unstable-v1 unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)
            target_include_directories(rexui PRIVATE ${WAYLAND_GEN_DIR})
        else()
            set(_rexglue_wl_missing "")
            if(NOT WAYLAND_CLIENT_FOUND)
                list(APPEND _rexglue_wl_missing "wayland-client (pkg: libwayland-dev / wayland)")
            endif()
            if(NOT GTK3_WAYLAND_FOUND)
                list(APPEND _rexglue_wl_missing "gtk+-wayland-3.0 (pkg: libgtk-3-dev)")
            endif()
            if(NOT WAYLAND_PROTOCOLS_FOUND)
                list(APPEND _rexglue_wl_missing "wayland-protocols (pkg: wayland-protocols)")
            endif()
            if(NOT WAYLAND_SCANNER_EXEC)
                list(APPEND _rexglue_wl_missing "wayland-scanner (pkg: libwayland-bin / wayland)")
            endif()
            string(JOIN ", " _rexglue_wl_missing_str ${_rexglue_wl_missing})
            message(FATAL_ERROR
                "REXGLUE_ENABLE_WAYLAND is ON but required dependencies are missing: "
                "${_rexglue_wl_missing_str}. "
                "Install the missing packages or reconfigure with -DREXGLUE_ENABLE_WAYLAND=OFF "
                "to build without native Wayland support.")
        endif()
    endif()
endif()

# D3D12 backend dependencies
if(REXGLUE_USE_D3D12)
    target_link_libraries(rexui PUBLIC
        dxgi
        dxguid
        dxc-headers
    )
endif()


