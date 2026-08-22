#include <mbgl/capture/context.hpp>

#include "command_encoder.hpp"
#include <mbgl/capture/drawable_builder.hpp>
#include <mbgl/capture/layer_group.hpp>
#include <mbgl/capture/renderer_backend.hpp>
#include <mbgl/capture/texture2d.hpp>
#include <mbgl/gfx/dynamic_texture.hpp>
#include <mbgl/gfx/renderbuffer.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/gfx/vertex_attribute.hpp>
#include <mbgl/shaders/shader_program_base.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/util/logging.hpp>

#include <algorithm>

namespace mln {
namespace capture {

namespace {

/// Renderbuffers and draw scopes are GL-era concepts the drawable path never consults; the
/// base class still requires a factory, so hand back inert objects.
class RenderbufferResource final : public gfx::RenderbufferResource {};
class DrawScopeResource final : public gfx::DrawScopeResource {};

} // namespace

Context::Context(RendererBackend& backend_, MapID mapId_, FrameSink& sink_)
    : gfx::Context(gfx::Context::minimumRequiredVertexBindingCount),
      backend(backend_),
      mapId(mapId_),
      sink(sink_),
      selfRef(std::make_shared<Context*>(this)) {}

Context::~Context() noexcept {
    // Anything still holding a ContextRef -- drawables and textures owned by layer groups the
    // caller may destroy after us -- now sees a dead Context and skips its callback.
    *selfRef = nullptr;
    dirtyTextures.clear();
}

void Context::beginFrame() {
    ++frameNo;
    inFrame = true;
    pendingOrder = FrameOrder{};
    pendingOrder.mapId = mapId;
    pendingOrder.frameNo = frameNo;
    sink.beginFrame(mapId, frameNo);
}

void Context::endFrame() {
    if (!inFrame) {
        return;
    }

    // The renderer writes the frame-wide parameters partway through the frame
    // (renderer_impl.cpp:301-313), so they are picked up here rather than at beginFrame.
    emitGlobalUniforms();

    // One texture envelope per frame, not one per sub-region upload. The glyph and icon
    // atlases insert many small regions per frame and each flush hashes the whole texture,
    // so flushing eagerly would be O(atlas bytes) per glyph.
    for (auto* texture : dirtyTextures) {
        texture->flush();
    }
    dirtyTextures.clear();

    sink.onFrameOrder(pendingOrder);
    sink.endFrame(mapId, frameNo);
    inFrame = false;
}

void Context::performCleanup() {}

std::unique_ptr<gfx::CommandEncoder> Context::createCommandEncoder() {
    return std::make_unique<CommandEncoder>(*this);
}

gfx::VertexAttributeArrayPtr Context::createVertexAttributeArray() const {
    // gfx::VertexAttributeArray is concrete; there is no backend-specific attribute state to
    // add, because we never build GPU bindings.
    return std::make_shared<gfx::VertexAttributeArray>();
}

gfx::UniqueDrawableBuilder Context::createDrawableBuilder(std::string name) {
    return std::make_unique<DrawableBuilder>(*this, std::move(name));
}

gfx::UniformBufferPtr Context::createUniformBuffer(const void* data,
                                                   std::size_t size,
                                                   bool /*persistent*/,
                                                   bool /*ssbo*/) {
    return std::make_shared<UniformBuffer>(data, size);
}

gfx::UniqueUniformBufferArray Context::createLayerUniformBufferArray() {
    return std::make_unique<UniformBufferArray>();
}

bool Context::emplaceOrUpdateUniformBuffer(gfx::UniformBufferPtr& ptr,
                                           const void* data,
                                           std::size_t size,
                                           bool persistent) {
    if (ptr && ptr->getSize() == size) {
        ptr->update(data, size);
        return false;
    }
    ptr = createUniformBuffer(data, size, persistent, false);
    return true;
}

gfx::ShaderProgramBasePtr Context::getGenericShader(gfx::ShaderRegistry& shaders, const std::string& name) {
    // Background, hillshade, color-relief and heatmap-texture resolve by bare name rather
    // than through a ShaderGroup permutation. The group we registered in initShaders answers
    // both, so ask it for the empty (no data-driven properties) permutation.
    const auto group = shaders.getShaderGroup(name);
    if (!group) {
        return {};
    }
    auto shader = group->getOrCreateShader(*this, {});
    return std::static_pointer_cast<gfx::ShaderProgramBase>(shader);
}

TileLayerGroupPtr Context::createTileLayerGroup(int32_t layerIndex, std::size_t initialCapacity, std::string name) {
    return std::make_shared<TileLayerGroup>(*this, layerIndex, initialCapacity, std::move(name));
}

LayerGroupPtr Context::createLayerGroup(int32_t layerIndex, std::size_t initialCapacity, std::string name) {
    return std::make_shared<LayerGroup>(*this, layerIndex, initialCapacity, std::move(name));
}

gfx::Texture2DPtr Context::createTexture2D() {
    return std::make_shared<Texture2D>(selfRef);
}

gfx::DynamicTexturePtr Context::createDynamicTexture(Size size, gfx::TexturePixelType pixelType) {
    // gfx::DynamicTexture is generic; it drives our Texture2D through the shelf packer.
    return std::make_shared<gfx::DynamicTexture>(*this, size, pixelType);
}

RenderTargetPtr Context::createRenderTarget(Size size, gfx::TextureChannelDataType type) {
    // Reachable only from heatmap / hillshade-prepare, which are out of scope until Phase 5.
    // RenderTarget is generic, so this works; the offscreen texture behind it is inert.
    return std::make_shared<RenderTarget>(*this, size, type);
}

std::unique_ptr<gfx::OffscreenTexture> Context::createOffscreenTexture(Size, gfx::TextureChannelDataType) {
    // No shared (non-backend) code reaches this today -- only the heatmap / hillshade-prepare
    // passes would, and they are out of scope until Phase 5. Returning null would surface as a
    // null dereference somewhere else entirely after a rebase, so say so here instead.
    Log::Error(Event::General, "capture: createOffscreenTexture is not implemented (Phase 5)");
    assert(false);
    return {};
}

std::unique_ptr<gfx::RenderbufferResource> Context::createRenderbufferResource(gfx::RenderbufferPixelType, Size) {
    return std::make_unique<RenderbufferResource>();
}

std::unique_ptr<gfx::DrawScopeResource> Context::createDrawScopeResource() {
    return std::make_unique<DrawScopeResource>();
}

void Context::emitGlobalUniforms() {
    for (std::size_t slot = 0; slot < globalUniformBuffers.allocatedSize(); ++slot) {
        const auto& buffer = globalUniformBuffers.get(slot);
        if (!buffer) {
            continue;
        }
        auto& captured = static_cast<UniformBuffer&>(*buffer);
        if (!captured.isDirty()) {
            continue;
        }
        sink.onUboUpdate(UboUpdate{.mapId = mapId,
                                   .isGlobal = true,
                                   .layerIndex = std::nullopt,
                                   .ownerId = std::nullopt,
                                   .slot = slot,
                                   .data = captured.getContents().data(),
                                   .size = captured.getContents().size()});
        captured.clearDirty();
    }
}

void Context::registerDirtyTexture(Texture2D& texture) {
    dirtyTextures.insert(&texture);
}

void Context::unregisterTexture(Texture2D& texture) {
    dirtyTextures.erase(&texture);
}

// -- frame recording ------------------------------------------------------------------

void Context::recordDrawableAdd(DrawableAdd&& add) {
    add.mapId = mapId;
    sink.onDrawableAdd(add);
}

void Context::recordDrawableRemove(const util::SimpleIdentity& id) {
    sink.onDrawableRemove(DrawableRemove{.mapId = mapId, .id = id});
}

void Context::recordDrawableUboUpdate(const util::SimpleIdentity& owner,
                                      std::size_t slot,
                                      const void* data,
                                      std::size_t size) {
    sink.onUboUpdate(UboUpdate{.mapId = mapId,
                               .isGlobal = false,
                               .layerIndex = std::nullopt,
                               .ownerId = owner,
                               .slot = slot,
                               .data = data,
                               .size = size});
}

void Context::recordLayerUboUpdate(std::int32_t layerIndex, std::size_t slot, const void* data, std::size_t size) {
    sink.onUboUpdate(UboUpdate{
        .mapId = mapId, .layerIndex = layerIndex, .ownerId = std::nullopt, .slot = slot, .data = data, .size = size});
}

void Context::recordTextureUpdate(TextureUpdate&& tex) {
    tex.mapId = mapId;
    sink.onTextureUpdate(tex);
}

void Context::recordStencilTiles(StencilTiles&& st) {
    st.mapId = mapId;
    sink.onStencilTiles(st);
}

void Context::recordDraw(const DrawOrderEntry& entry) {
    pendingOrder.ordered.push_back(entry);
}

void Context::setFrameDepthInfo(std::uint32_t opaquePassCutoff, float depthRangeSize) {
    pendingOrder.opaquePassCutoff = opaquePassCutoff;
    pendingOrder.depthRangeSize = depthRangeSize;
}

} // namespace capture
} // namespace mln
