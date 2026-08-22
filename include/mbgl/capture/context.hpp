#pragma once

#include <mbgl/capture/frame_diff.hpp>
#include <mbgl/capture/uniform_buffer.hpp>
#include <mbgl/gfx/color_mode.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/depth_mode.hpp>

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace mln {
namespace capture {

class Context;
class Drawable;
class RendererBackend;
class Texture2D;

/// Liveness token for objects that outlive their Context.
///
/// Drawables and textures call back into the Context when they are destroyed, but their
/// lifetimes are not nested inside it: they are owned by layer groups owned by the
/// RenderOrchestrator, which the caller may destroy after the backend. Holding the Context by
/// reference makes that ordering a use-after-free. The token is nulled in ~Context, so a late
/// destructor sees a dead Context and does nothing instead of writing through a dangling
/// pointer.
using ContextRef = std::shared_ptr<Context*>;

/// The capture Context. Every factory hands back an object that retains what mbgl gives it
/// and forwards a description to the frame sink. No GPU resource is created anywhere.
class Context final : public gfx::Context {
public:
    Context(RendererBackend& backend_, MapID mapId_, FrameSink& sink_);
    ~Context() noexcept override;

    RendererBackend& getBackend() noexcept { return backend; }
    FrameSink& getSink() noexcept { return sink; }
    MapID getMapID() const noexcept { return mapId; }
    std::uint64_t getFrameNumber() const noexcept { return frameNo; }

    void beginFrame() override;
    void endFrame() override;
    void performCleanup() override;
    void reduceMemoryUsage() override {}

    std::unique_ptr<gfx::CommandEncoder> createCommandEncoder() override;

    gfx::VertexAttributeArrayPtr createVertexAttributeArray() const override;
    gfx::UniqueDrawableBuilder createDrawableBuilder(std::string name) override;

    gfx::UniformBufferPtr createUniformBuffer(const void* data, std::size_t size, bool persistent, bool ssbo) override;
    gfx::UniqueUniformBufferArray createLayerUniformBufferArray() override;
    bool emplaceOrUpdateUniformBuffer(gfx::UniformBufferPtr&,
                                      const void* data,
                                      std::size_t size,
                                      bool persistent) override;

    gfx::ShaderProgramBasePtr getGenericShader(gfx::ShaderRegistry&, const std::string& name) override;

    TileLayerGroupPtr createTileLayerGroup(int32_t layerIndex, std::size_t initialCapacity, std::string name) override;
    LayerGroupPtr createLayerGroup(int32_t layerIndex, std::size_t initialCapacity, std::string name) override;

    gfx::Texture2DPtr createTexture2D() override;
    gfx::DynamicTexturePtr createDynamicTexture(Size size, gfx::TexturePixelType pixelType) override;
    RenderTargetPtr createRenderTarget(Size size, gfx::TextureChannelDataType type) override;
    std::unique_ptr<gfx::OffscreenTexture> createOffscreenTexture(Size, gfx::TextureChannelDataType) override;

    void resetState(gfx::DepthMode, gfx::ColorMode) override {}
    void setDirtyState() override {}
    void clearStencilBuffer(int32_t) override {}

#ifndef NDEBUG
    void visualizeStencilBuffer() override {}
    void visualizeDepthBuffer(float) override {}
#endif

    const gfx::UniformBufferArray& getGlobalUniformBuffers() const override { return globalUniformBuffers; }
    gfx::UniformBufferArray& mutableGlobalUniformBuffers() override { return globalUniformBuffers; }
    void bindGlobalUniformBuffers(gfx::RenderPass&) const noexcept override {}
    void unbindGlobalUniformBuffers(gfx::RenderPass&) const noexcept override {}

    // -- frame recording, used by the capture resource classes -------------------------

    void recordDrawableAdd(DrawableAdd&&);
    void recordDrawableRemove(const util::SimpleIdentity&);
    void recordDrawableUboUpdate(const util::SimpleIdentity& owner,
                                 std::size_t slot,
                                 const void* data,
                                 std::size_t size);
    void recordLayerUboUpdate(std::int32_t layerIndex, std::size_t slot, const void* data, std::size_t size);
    void recordTextureUpdate(TextureUpdate&&);
    void recordStencilTiles(StencilTiles&&);
    void recordDraw(const DrawOrderEntry&);
    void setFrameDepthInfo(std::uint32_t opaquePassCutoff, float depthRangeSize);

    const ContextRef& ref() const noexcept { return selfRef; }

    /// Textures accumulate dirty regions and are flushed once per frame rather than on every
    /// sub-region upload -- see Texture2D::markDirty.
    void registerDirtyTexture(Texture2D&);
    void unregisterTexture(Texture2D&);

protected:
    std::unique_ptr<gfx::RenderbufferResource> createRenderbufferResource(gfx::RenderbufferPixelType, Size) override;
    std::unique_ptr<gfx::DrawScopeResource> createDrawScopeResource() override;

private:
    RendererBackend& backend;
    MapID mapId;
    FrameSink& sink;

    UniformBufferArray globalUniformBuffers;

    ContextRef selfRef;
    std::unordered_set<Texture2D*> dirtyTextures;

    std::uint64_t frameNo = 0;
    bool inFrame = false;
    FrameOrder pendingOrder;
};

} // namespace capture
} // namespace mln
