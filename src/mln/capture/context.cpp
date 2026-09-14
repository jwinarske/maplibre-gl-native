#include <mln/capture/context.hpp>

#include <mln/capture/dynamic_texture.hpp>

#include "command_encoder.hpp"
#include <mln/capture/drawable_builder.hpp>
#include <mln/capture/layer_group.hpp>
#include <mln/capture/offscreen_texture.hpp>
#include <mln/capture/renderer_backend.hpp>
#include <mln/capture/texture2d.hpp>
#include <mln/gfx/dynamic_texture.hpp>
#include <mln/gfx/renderbuffer.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/gfx/shader_registry.hpp>
#include <mln/gfx/vertex_attribute.hpp>
#include <mln/shaders/shader_program_base.hpp>
#include <mln/renderer/render_target.hpp>
#include <mln/util/logging.hpp>

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
    // Not gfx::DynamicTexture: its uploadImage is a no-op that discards pixel data, so the
    // shelf packer would allocate an atlas and every glyph written to it would be dropped.
    return std::make_shared<DynamicTexture>(*this, size, pixelType);
}

RenderTargetPtr Context::createRenderTarget(Size size, gfx::TextureChannelDataType type) {
    // RenderTarget is generic; what it wants from us is the offscreen texture below.
    return std::make_shared<RenderTarget>(*this, size, type);
}

std::unique_ptr<gfx::OffscreenTexture> Context::createOffscreenTexture(Size size, gfx::TextureChannelDataType type) {
    auto target = std::make_unique<OffscreenTexture>(*this, size, type);
    // The target uploads no pixels, so it produces no TextureUpdate. Announce it here or the
    // second pass binds a texture id nothing on this protocol ever described.
    sink.onRenderTargetCreate(
        RenderTargetCreate{.mapId = mapId,
                           .textureId = static_cast<Texture2D&>(*target->getTexture()).getID(),
                           .size = size,
                           .channelType = type});
    return target;
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

void Context::recordLayerUboUpdate(
    std::int32_t layerIndex, std::string_view layerName, std::size_t slot, const void* data, std::size_t size) {
    sink.onUboUpdate(UboUpdate{.mapId = mapId,
                               .layerIndex = layerIndex,
                               .layerName = layerName,
                               .ownerId = std::nullopt,
                               .slot = slot,
                               .data = data,
                               .size = size});
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

void Context::setFrameProjection(const std::array<double, 16>& projMatrix) {
    pendingOrder.projMatrix = projMatrix;
}

void Context::setFrameCamera(const std::array<double, 2>& centerZoom0,
                             double bearing,
                             double pitch,
                             double pixelsPerMeter) {
    pendingOrder.centerZoom0 = centerZoom0;
    pendingOrder.bearing = bearing;
    pendingOrder.pitch = pitch;
    pendingOrder.pixelsPerMeter = pixelsPerMeter;
}

void Context::setFrameLight(const FrameOrder::Light& light) {
    pendingOrder.light = light;
}

} // namespace capture
} // namespace mln
