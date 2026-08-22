#pragma once

#include <mbgl/capture/uniform_buffer.hpp>
#include <mbgl/renderer/layer_group.hpp>

namespace mln {
namespace capture {

class Context;

// ---------------------------------------------------------------------------------------
//  INVARIANT — do not remove.
//
//  Neither class below may call `PaintParameters::renderTileClippingMasks()` or
//  `PaintParameters::clearStencil()`.
//
//  Those functions are compile-time dispatched per backend (paint_parameters.cpp:156-330)
//  and every non-GL branch performs an unconditional
//  `static_cast<vulkan::Context&>(context)` / `static_cast<vulkan::RenderPass&>(*renderPass)`
//  on our objects. We build with MLN_RENDER_BACKEND_VULKAN so mbgl-core compiles and links,
//  which means those casts exist in the binary; they are reachable ONLY from a backend's own
//  layer group, so they stay dead as long as we never call them. Calling one compiles clean
//  and corrupts memory at runtime.
//
//  Tile clipping is instead handed to the consumer as a `StencilTiles` envelope, which is
//  built from `stencilTiles` (tile_layer_group.cpp:73). See plan §3.4.1 and §4.
// ---------------------------------------------------------------------------------------

class LayerGroup final : public mln::LayerGroup {
public:
    LayerGroup(Context& context_, int32_t layerIndex_, std::size_t initialCapacity, std::string name_);
    ~LayerGroup() override = default;

    void upload(gfx::UploadPass&) override;
    void render(RenderOrchestrator&, PaintParameters&) override;

    const gfx::UniformBufferArray& getUniformBuffers() const override { return uniformBuffers; }
    gfx::UniformBufferArray& mutableUniformBuffers() override { return uniformBuffers; }

private:
    Context& context;
    UniformBufferArray uniformBuffers;
};

class TileLayerGroup final : public mln::TileLayerGroup {
public:
    TileLayerGroup(Context& context_, int32_t layerIndex_, std::size_t initialCapacity, std::string name_);
    ~TileLayerGroup() override = default;

    void upload(gfx::UploadPass&) override;
    void render(RenderOrchestrator&, PaintParameters&) override;

    const gfx::UniformBufferArray& getUniformBuffers() const override { return uniformBuffers; }
    gfx::UniformBufferArray& mutableUniformBuffers() override { return uniformBuffers; }

private:
    Context& context;
    UniformBufferArray uniformBuffers;
};

} // namespace capture
} // namespace mln
