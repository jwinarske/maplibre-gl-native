#include <mln/capture/renderer_backend.hpp>

#include <mln/capture/context.hpp>
#include <mln/capture/shader_program.hpp>
#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/shader_registry.hpp>
#include <mln/shaders/program_parameters.hpp>
// The per-shader attribute/texture metadata tables we reuse. These headers are mostly GLSL
// string literals, but the `attributes` / `instanceAttributes` / `textures` arrays alongside
// them are the only part we touch.
#include <mln/shaders/vulkan/background.hpp>
#include <mln/shaders/vulkan/circle.hpp>
#include <mln/shaders/vulkan/clipping_mask.hpp>
#include <mln/shaders/vulkan/collision.hpp>
#include <mln/shaders/vulkan/color_relief.hpp>
#include <mln/shaders/vulkan/debug.hpp>
#include <mln/shaders/vulkan/fill.hpp>
#include <mln/shaders/vulkan/fill_extrusion.hpp>
#include <mln/shaders/vulkan/heatmap.hpp>
#include <mln/shaders/vulkan/heatmap_texture.hpp>
#include <mln/shaders/vulkan/hillshade.hpp>
#include <mln/shaders/vulkan/hillshade_prepare.hpp>
#include <mln/shaders/vulkan/line.hpp>
#include <mln/shaders/vulkan/location_indicator.hpp>
#include <mln/shaders/vulkan/raster.hpp>
#include <mln/shaders/vulkan/symbol.hpp>
#include <mln/shaders/vulkan/widevector.hpp>
#include <mln/util/logging.hpp>

namespace mln {
namespace capture {

namespace {

template <shaders::BuiltIn ShaderID>
void registerShaderGroup(gfx::ShaderRegistry& registry, const ProgramParameters& programParameters) {
    using ShaderClass = shaders::ShaderSource<ShaderID, gfx::Backend::Type::Vulkan>;
    auto group = std::make_shared<ShaderGroup<ShaderID>>(programParameters);
    if (!registry.registerShaderGroup(std::move(group), ShaderClass::name)) {
        Log::Warning(Event::General, std::string("Failed to register capture shader group ") + ShaderClass::name);
        assert(false);
    }
}

template <shaders::BuiltIn... ShaderIDs>
void registerTypes(gfx::ShaderRegistry& registry, const ProgramParameters& programParameters) {
    (registerShaderGroup<ShaderIDs>(registry, programParameters), ...);
}

} // namespace

// ContextMode::Shared, deliberately.
//
// `Renderer::Impl` passes `!backend.contextIsShared()` as RenderOrchestrator's
// `backgroundLayerAsColor` (renderer_impl.cpp:57). Under ContextMode::Unique the background
// layer is folded into the render pass clear color and never becomes a drawable at all --
// which is useless to us, because the consumer composites the map into a Filament scene that
// has no equivalent clear step under our control. Shared mode makes the background layer
// emit real geometry through BackgroundShader, like every other layer.
RendererBackend::RendererBackend(Size size, FrameSink& sink_, MapID mapId_)
    : gfx::RendererBackend(gfx::ContextMode::Shared),
      sink(sink_),
      mapId(mapId_),
      renderable(size) {}

RendererBackend::RendererBackend(Size size, FrameSink& sink_, MapID mapId_, const TaggedScheduler& threadPool_)
    : gfx::RendererBackend(gfx::ContextMode::Shared, threadPool_),
      sink(sink_),
      mapId(mapId_),
      renderable(size) {}

RendererBackend::~RendererBackend() {
    // The Context holds capture resources whose destructors emit removals; tear it down
    // inside a backend scope so the same assertions the real backends rely on hold.
    gfx::BackendScope guard{*this};
    context.reset();
}

std::unique_ptr<gfx::Context> RendererBackend::createContext() {
    return std::make_unique<Context>(*this, mapId, sink);
}

void RendererBackend::initShaders(gfx::ShaderRegistry& registry, const ProgramParameters& programParameters) {
    // Same list the Vulkan backend registers (vulkan/renderer_backend.cpp:694-726), minus the
    // shaders whose drawables we do not consume in v1. Registering a group costs nothing but
    // a map entry — no source, no compilation.
    registerTypes<shaders::BuiltIn::BackgroundShader,
                  shaders::BuiltIn::BackgroundPatternShader,
                  shaders::BuiltIn::CircleShader,
                  shaders::BuiltIn::ClippingMaskProgram,
                  shaders::BuiltIn::CollisionBoxShader,
                  shaders::BuiltIn::CollisionCircleShader,
                  shaders::BuiltIn::ColorReliefShader,
                  shaders::BuiltIn::DebugShader,
                  shaders::BuiltIn::FillShader,
                  shaders::BuiltIn::FillOutlineShader,
                  shaders::BuiltIn::FillPatternShader,
                  shaders::BuiltIn::FillOutlinePatternShader,
                  shaders::BuiltIn::FillOutlineTriangulatedShader,
                  shaders::BuiltIn::FillExtrusionShader,
                  shaders::BuiltIn::FillExtrusionInstancedShader,
                  shaders::BuiltIn::FillExtrusionPatternShader,
                  shaders::BuiltIn::FillExtrusionPatternInstancedShader,
                  shaders::BuiltIn::HeatmapShader,
                  shaders::BuiltIn::HeatmapTextureShader,
                  shaders::BuiltIn::HillshadeShader,
                  shaders::BuiltIn::HillshadePrepareShader,
                  shaders::BuiltIn::LineShader,
                  shaders::BuiltIn::LineGradientShader,
                  shaders::BuiltIn::LineSDFShader,
                  shaders::BuiltIn::LinePatternShader,
                  shaders::BuiltIn::LocationIndicatorShader,
                  shaders::BuiltIn::LocationIndicatorTexturedShader,
                  shaders::BuiltIn::RasterShader,
                  shaders::BuiltIn::SymbolIconShader,
                  shaders::BuiltIn::SymbolSDFShader,
                  shaders::BuiltIn::SymbolTextAndIconShader,
                  shaders::BuiltIn::WideVectorShader>(registry, programParameters);
}

} // namespace capture
} // namespace mln
