/**
 * Narrow standalone definitions needed to link the Xenos microcode -> SPIR-V
 * translation layer (shader.cpp/translator.cpp/spirv_translator*.cpp) into
 * the always-linked rexruntime, without pulling in the generic
 * CommandProcessor/TextureCache/TraceWriter machinery those symbols normally
 * live alongside in the rexgpu-xenos plugin (src/graphics/pipeline/texture/
 * cache.cpp and src/graphics/util/draw.cpp respectively). Each definition
 * below is a verbatim copy of the plugin-side one; if either of those files
 * changes these, mirror the change here.
 */

#include <cstdint>

#include <rex/graphics/flags.h>

// From src/graphics/pipeline/texture/cache.cpp: only used by
// ShaderTranslator::TranslateAnalyzedShader for optional shader-storage
// dumping (translator.cpp), which doesn't otherwise need the texture cache.
REXCVAR_DEFINE_BOOL(shader_dump_enabled, false, "GPU/Shader Storage",
                    "Dump translated shader binaries to disk for mod authoring")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::draw_util {

// From src/graphics/util/draw.cpp: only used by
// SpirvShaderTranslator::FSI_DepthStencilTest (spirv_translator_rb.cpp) for
// MSAA sample positions, which doesn't otherwise need the rest of draw.cpp
// (resolve/render-target-cache helpers tied to TraceWriter).
extern const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
extern const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

}  // namespace rex::graphics::draw_util
