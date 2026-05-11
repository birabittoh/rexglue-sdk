set(REXSYSTEM_SOURCES
    # Small, self-contained PM4/register format helpers reused by the
    # headless ring-buffer decode instrumentation in kernel_state.cpp, plus
    # (below) the Xenos microcode -> SPIR-V translation layer reused by
    # the native (non-xenos) command processor. NOTE: the rest
    # of the Xenos GPU stack (CommandProcessor, caches, GraphicsSystem)
    # lives in the rexgpu-xenos plugin, which is runtime-loaded and never
    # linked here -- only translation units with no dependency on that
    # plugin-only code are safe to duplicate into the always-linked runtime.
    ../graphics/register_file.cpp
    ../graphics/packet_disassembler.cpp
    ../graphics/flags.cpp
    ../graphics/format/ucode.cpp
    ../graphics/pipeline/shader/shader.cpp
    ../graphics/pipeline/shader/translator.cpp
    ../graphics/pipeline/shader/translator_disasm.cpp
    ../graphics/shader_translation_support.cpp
    ../graphics/pipeline/shader/spirv_builder.cpp
    ../graphics/pipeline/shader/spirv.cpp
    ../graphics/pipeline/shader/spirv_translator.cpp
    ../graphics/pipeline/shader/spirv_translator_alu.cpp
    ../graphics/pipeline/shader/spirv_translator_fetch.cpp
    ../graphics/pipeline/shader/spirv_translator_memexport.cpp
    ../graphics/pipeline/shader/spirv_translator_rb.cpp

    runtime.cpp
    embedded_metadata.cpp
    achievement_manager.cpp
    achievements.cpp
    leaderboard_manager.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    gpu_plugin_loader.cpp
    mod_attribution.cpp
    mod_conflict_tracker.cpp
    mod_plugin_loader.cpp
    mod_registry.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
        xxHash::xxhash
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexruntime PUBLIC TracyClient)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE rexdiscord ws2_32)
    target_compile_definitions(rexruntime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(LINUX)
    target_link_libraries(rexruntime PRIVATE rexdiscord)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
    # For the Xenos microcode -> SPIR-V translator sources above
    # (spirv_builder.cpp/spirv_translator*.cpp), mirroring rexgpu-xenos's own
    # link libraries for the same translation units.
    target_link_libraries(rexruntime PRIVATE
        glslang::SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()
