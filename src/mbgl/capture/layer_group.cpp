#include <mbgl/capture/layer_group.hpp>

#include <mbgl/capture/context.hpp>
#include <mbgl/capture/drawable.hpp>
#include <mbgl/gfx/drawable_tweaker.hpp>
#include <mbgl/gfx/upload_pass.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/util/convert.hpp>

namespace mln {
namespace capture {

namespace {

template <typename Group>
void uploadDrawables(Group& group, gfx::UploadPass& uploadPass) {
    const auto layerIndex = group.getLayerIndex();
    group.visitDrawables([&](gfx::Drawable& drawable) {
        if (drawable.getEnabled()) {
            auto& captured = static_cast<Drawable&>(drawable);
            // Stamp before upload: the announcement has to name the layer group whose
            // consolidated UBO array carries this drawable's slice.
            captured.setOwningLayerIndex(layerIndex);
            captured.upload(uploadPass);
        }
    });
}

/// Emit the layer group's own uniform buffers, dirty-only.
///
/// This is where most UBO traffic lives. `MLN_UBO_CONSOLIDATION` (shaders/layer_ubo.hpp:73)
/// is on for Metal/Vulkan/WebGPU, and we pair with Vulkan, so the layer tweakers pack every
/// drawable's UBO into one SSBO held here and stamp each drawable with `setUBOIndex()`
/// (fill_layer_tweaker.cpp:245,266). The consumer pairs this buffer with the per-frame
/// `DrawOrderEntry::uboIndex` to find a given drawable's slice.
void emitLayerUniforms(Context& context, std::int32_t layerIndex, UniformBufferArray& uniforms) {
    for (std::size_t slot = 0; slot < uniforms.allocatedSize(); ++slot) {
        const auto& buf = uniforms.get(slot);
        if (!buf) {
            continue;
        }
        auto& captured = static_cast<UniformBuffer&>(*buf);
        if (!captured.isDirty()) {
            continue;
        }
        context.recordLayerUboUpdate(layerIndex, slot, captured.getContents().data(), captured.getContents().size());
        captured.clearDirty();
    }
}

/// Run tweakers and record painter order. Deliberately does NOT call
/// `parameters.renderTileClippingMasks()` — see the invariant in layer_group.hpp.
template <typename Group>
void renderDrawables(Group& group, PaintParameters& parameters) {
    group.visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled() || !drawable.hasRenderPass(parameters.pass)) {
            return;
        }
        for (const auto& tweaker : drawable.getTweakers()) {
            tweaker->execute(drawable, parameters);
        }
        drawable.draw(parameters);
    });
}

} // namespace

LayerGroup::LayerGroup(Context& context_, int32_t layerIndex_, std::size_t initialCapacity, std::string name_)
    : mln::LayerGroup(layerIndex_, initialCapacity, std::move(name_)),
      context(context_) {}

void LayerGroup::upload(gfx::UploadPass& uploadPass) {
    if (!enabled || !getDrawableCount()) {
        return;
    }
    uploadDrawables(*this, uploadPass);
}

void LayerGroup::render(RenderOrchestrator&, PaintParameters& parameters) {
    if (!enabled || !getDrawableCount() || !parameters.renderPass) {
        return;
    }
    emitLayerUniforms(context, getLayerIndex(), uniformBuffers);
    renderDrawables(*this, parameters);
}

TileLayerGroup::TileLayerGroup(Context& context_, int32_t layerIndex_, std::size_t initialCapacity, std::string name_)
    : mln::TileLayerGroup(layerIndex_, initialCapacity, std::move(name_)),
      context(context_) {}

void TileLayerGroup::upload(gfx::UploadPass& uploadPass) {
    if (!enabled || !getDrawableCount()) {
        return;
    }
    uploadDrawables(*this, uploadPass);
}

void TileLayerGroup::render(RenderOrchestrator&, PaintParameters& parameters) {
    if (!enabled || !getDrawableCount() || !parameters.renderPass) {
        return;
    }

    // Hand the clip set to the consumer instead of rasterizing masks. mbgl's own path here
    // would be `parameters.renderTileClippingMasks(stencilTiles)`, which casts our Context to
    // the concrete backend type. See the invariant in layer_group.hpp and plan §3.4.1.
    if (stencilTiles && !stencilTiles->empty()) {
        StencilTiles st;
        st.layerIndex = getLayerIndex();
        st.tiles.reserve(stencilTiles->size());
        for (const auto& tileRef : *stencilTiles) {
            const auto& tile = tileRef.get();
            // matrixForTile takes the unwrapped id, and the overscaled one identifies the tile
            // to the consumer -- the same pair mbgl itself works in.
            st.tiles.push_back(StencilTile{.id = tile.getOverscaledTileID(),
                                           .matrix = util::cast<float>(parameters.matrixForTile(tile.id))});
        }
        context.recordStencilTiles(std::move(st));
    }

    emitLayerUniforms(context, getLayerIndex(), uniformBuffers);
    renderDrawables(*this, parameters);
}

} // namespace capture
} // namespace mln
