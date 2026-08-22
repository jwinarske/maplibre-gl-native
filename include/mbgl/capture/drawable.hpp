#pragma once

#include <mbgl/capture/frame_diff.hpp>
#include <mbgl/capture/uniform_buffer.hpp>
#include <mbgl/gfx/draw_mode.hpp>
#include <mbgl/gfx/drawable.hpp>

#include <memory>
#include <vector>

namespace mln {

class SegmentBase;

namespace gfx {
class DepthMode;
class StencilMode;
class UploadPass;
} // namespace gfx

namespace capture {

class Context;
using ContextRef = std::shared_ptr<Context*>;

/// Retains everything a layer hands the backend and hands it on to the frame sink. Nothing
/// is uploaded, converted or interleaved: vertex data stays as the shared vectors mbgl
/// already owns, so the map thread copies nothing.
class Drawable final : public gfx::Drawable {
public:
    Drawable(ContextRef context, std::string name);
    ~Drawable() override;

    /// Called from the layer group's upload pass. Emits `DrawableAdd` the first time the
    /// geometry is complete, and again whenever the attribute set is rebuilt.
    void upload(gfx::UploadPass&);

    /// Records painter order into the current frame. `const` per gfx::Drawable:71, so the
    /// recording goes to the Context's frame sink rather than to mutable member state.
    void draw(PaintParameters&) const override;

    void setIndexData(gfx::IndexVectorBasePtr, std::vector<UniqueDrawSegment>) override;
    void setVertices(std::vector<uint8_t>&&, std::size_t, gfx::AttributeDataType) override;

    void updateVertexAttributes(gfx::VertexAttributeArrayPtr,
                                std::size_t vertexCount,
                                gfx::DrawMode,
                                gfx::IndexVectorBasePtr,
                                const SegmentBase* segments,
                                std::size_t segmentCount) override;

    const gfx::UniformBufferArray& getUniformBuffers() const override { return uniformBuffers; }
    gfx::UniformBufferArray& mutableUniformBuffers() override { return uniformBuffers; }

    const gfx::IndexVectorBasePtr& getIndexes() const noexcept { return indexes; }
    const std::vector<UniqueDrawSegment>& getSegments() const noexcept { return segments; }
    std::size_t getVertexCount() const noexcept { return vertexCount; }
    gfx::AttributeDataType getVertexType() const noexcept { return vertexType; }

    /// Which vertex attribute the raw byte block feeds, from the builder. Mirrors what the
    /// real backends do with `Drawable::Impl::vertexAttrId`.
    void setVertexAttrId(std::size_t value) noexcept { vertexAttrId = value; }

    /// Stamped by the owning layer group before upload, so `DrawableAdd` can name the layer
    /// whose consolidated uniform buffer this drawable indexes into.
    void setOwningLayerIndex(std::int32_t value) noexcept { owningLayerIndex = value; }

    /// 3D drawables get their depth/stencil mode assigned by the layer group before draw,
    /// mirroring the real backends (vulkan/tile_layer_group.cpp:104-113).
    void setDepthModeFor3D(const gfx::DepthMode& value);
    void setStencilModeFor3D(const gfx::StencilMode& value);

private:
    void emitAdd(AddReason) const;
    /// Cheap signature over everything `DrawableAdd` carries about geometry. Re-announcement
    /// is decided by comparing this, never by "was a setter called" -- see upload().
    std::uint64_t contentSignature() const;

    ContextRef context;
    UniformBufferArray uniformBuffers;

    gfx::IndexVectorBasePtr indexes;
    std::vector<UniqueDrawSegment> segments;
    std::size_t vertexCount = 0;
    gfx::AttributeDataType vertexType = gfx::AttributeDataType::Invalid;
    std::shared_ptr<std::vector<std::uint8_t>> rawVertices;
    std::size_t vertexAttrId = 0;

    std::int32_t owningLayerIndex = -1;
    mutable bool announced = false;
    mutable std::uint64_t announcedSignature = 0;
};

} // namespace capture
} // namespace mln
