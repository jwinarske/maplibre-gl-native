#pragma once

#include <mbgl/gfx/types.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/identity.hpp>
#include <mbgl/util/rect.hpp>
#include <mbgl/util/size.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mln {

enum class RenderPass : uint8_t;

namespace gfx {
class IndexVectorBase;
class VertexVectorBase;
} // namespace gfx

namespace capture {

/// Identifies which `Renderer` (map instance) an envelope came from. Drawable ids are
/// `util::SimpleIdentity`, unique per Renderer only, so every envelope must carry this
/// or ids collide when several maps feed one consumer. See plan §3.2 / §3.6.
using MapID = uint32_t;

/// A single vertex attribute as mbgl actually delivers it: a shared, non-owning view into a
/// bucket's vertex vector with an offset/stride, *not* a block of interleaved bytes.
/// Mirrors `gfx::VertexAttribute::setSharedRawData` (gfx/vertex_attribute.hpp:236).
struct AttributeDesc {
    /// Shader-side attribute id (e.g. `shaders::idFillColorVertexAttribute`)
    std::size_t attrId = 0;
    /// Binding slot declared by the shader for this permutation, resolved from the shader's
    /// own attribute table. **-1 means the drawable supplied an override the shader does not
    /// declare** -- the real backends drop these in `buildAttributeBindings`, and so should
    /// the consumer. (Seen in practice: LineShader drawables carry a floor-width override
    /// that only the SDF variant declares.)
    int index = -1;
    gfx::AttributeDataType dataType = gfx::AttributeDataType::Invalid;

    /// Shared source buffer. Null when the attribute carries per-vertex values inline
    /// (`rawCount` elements) rather than referencing a bucket vector.
    std::shared_ptr<gfx::VertexVectorBase> sharedVector;
    uint32_t offset = 0;
    uint32_t vertexOffset = 0;
    uint32_t stride = 0;

    /// Element count when `sharedVector` is null.
    std::size_t rawCount = 0;
};

/// One draw segment: a contiguous index range with its own vertex base.
struct SegmentDesc {
    std::size_t vertexOffset = 0;
    std::size_t indexOffset = 0;
    std::size_t vertexLength = 0;
    std::size_t indexLength = 0;
};

/// Emitted once when a drawable is built. Drawables are immutable after build — mbgl rebuilds
/// them on bucket change and tweakers mutate only UBOs — so this is bounded by tile churn,
/// not by frame rate.
/// Why a drawable was (re-)announced. Distinguishing these matters: the whole diff-stream
/// premise is that geometry churn tracks tile churn rather than frame rate, so a steady
/// stream of `AttributesModified` on a static scene is a bug worth seeing.
enum class AddReason : std::uint8_t {
    Created,            ///< First announcement for this drawable id
    IndexDataReplaced,  ///< setIndexData() ran again
    AttributesReplaced, ///< updateVertexAttributes() ran again
    AttributesModified, ///< same array, but values were rewritten in place
};

struct DrawableAdd {
    MapID mapId = 0;
    AddReason reason = AddReason::Created;
    util::SimpleIdentity id;
    std::string name;

    shaders::BuiltIn builtinShader = shaders::BuiltIn::None;
    /// Permutation key from `ShaderGroup::getOrCreateShader`; distinguishes the
    /// data-driven-attribute variants of one shader family. See plan §3.4.2.
    std::uint64_t permutationKey = 0;

    std::optional<OverscaledTileID> tileID;

    std::vector<AttributeDesc> attrs;
    std::vector<AttributeDesc> instanceAttrs;
    std::size_t vertexCount = 0;
    gfx::AttributeDataType vertexType = gfx::AttributeDataType::Invalid;

    std::shared_ptr<gfx::IndexVectorBase> indexes;
    std::vector<SegmentDesc> segments;

    /// Slot -> texture id, for slots that are bound.
    std::vector<std::pair<std::size_t, util::SimpleIdentity>> textureRefs;

    bool is3D = false;
    bool enableStencil = false;
    bool enableDepth = false;
    bool enableColor = true;
    std::uint8_t renderPass = 0;
    std::int32_t subLayerIndex = 0;
};

struct DrawableRemove {
    MapID mapId = 0;
    util::SimpleIdentity id;
};

/// Per-frame, dirty-only. Tweakers rewrite every drawable UBO every frame
/// (renderer_impl.cpp:279-287); only changed bytes reach here.
///
/// Under `MLN_UBO_CONSOLIDATION` -- which the Vulkan pairing turns on
/// (shaders/layer_ubo.hpp:73) -- the per-drawable UBOs are packed into a single SSBO owned by
/// the *layer group*, and each drawable indexes into it via `DrawOrderEntry::uboIndex`. So
/// most traffic arrives with `layerIndex` set and `ownerId` empty. Drawables that keep their
/// own buffers (and non-consolidating builds) arrive the other way round.
struct UboUpdate {
    MapID mapId = 0;
    /// Set when the buffer belongs to a layer group.
    std::optional<std::int32_t> layerIndex;
    /// Set when the buffer belongs to a single drawable.
    std::optional<util::SimpleIdentity> ownerId;
    std::size_t slot = 0;
    /// BORROWED. Points into the capture UniformBuffer's storage and is valid **only for the
    /// duration of the `onUboUpdate` call**. A sink that queues rather than consumes (the ring
    /// transport in later phases) must copy these bytes before returning.
    const void* data = nullptr;
    std::size_t size = 0;
};

struct TextureUpdate {
    MapID mapId = 0;
    util::SimpleIdentity id;
    Size size;
    gfx::TexturePixelType format = gfx::TexturePixelType::RGBA;
    /// Content hash, so a consumer can share one GPU texture between map instances that
    /// produced byte-identical atlases. See plan §3.6.
    std::uint64_t contentHash = 0;
    /// Sub-region updated, or nullopt for a whole-texture upload.
    std::optional<Rect<uint16_t>> dirtyRect;
};

/// The tile set a layer group wants clipped. mbgl never produces clipping-mask *drawables* on
/// any backend we use — `PaintParameters::renderTileClippingMasks` calls into the concrete
/// backend Context directly — so the consumer synthesizes the masks from this. Plan §3.4.1.
struct StencilTiles {
    MapID mapId = 0;
    std::int32_t layerIndex = 0;
    std::vector<OverscaledTileID> tiles;
};

/// One drawable's position in the frame's painter order, recorded at `draw()` time.
struct DrawOrderEntry {
    util::SimpleIdentity id;
    std::uint8_t pass = 0;
    std::uint32_t layerIndex = 0;
    std::int32_t subLayerIndex = 0;
    std::int64_t drawPriority = 0;
    /// Slot in the layer group's consolidated UBO array. Reassigned by the tweaker on every
    /// pass (fill_layer_tweaker.cpp:245), so it belongs to the frame, not to `DrawableAdd`.
    std::uint32_t uboIndex = 0;
};

struct FrameOrder {
    MapID mapId = 0;
    std::uint64_t frameNo = 0;
    std::vector<DrawOrderEntry> ordered;
    std::uint32_t opaquePassCutoff = 0;
    float depthRangeSize = 0.0f;
};

/// Consumer of the capture stream. Phase 0 ships `LogFrameSink`; later phases swap in the
/// SPSC ring feeding the Filament-side MapSystem. All calls arrive on the map thread.
class FrameSink {
public:
    virtual ~FrameSink() = default;

    virtual void beginFrame(MapID, std::uint64_t /*frameNo*/) {}
    virtual void endFrame(MapID, std::uint64_t /*frameNo*/) {}

    virtual void onDrawableAdd(const DrawableAdd&) {}
    virtual void onDrawableRemove(const DrawableRemove&) {}
    virtual void onUboUpdate(const UboUpdate&) {}
    virtual void onTextureUpdate(const TextureUpdate&) {}
    virtual void onStencilTiles(const StencilTiles&) {}
    virtual void onFrameOrder(const FrameOrder&) {}
};

/// Phase 0 sink: counts and logs. Exists to satisfy the Phase 0 exit criterion — prove mbgl
/// runs a full frame with zero GPU and that background/fill drawables arrive with their
/// attribute descriptors and permutation keys resolved.
class LogFrameSink final : public FrameSink {
public:
    explicit LogFrameSink(bool verbose_ = false)
        : verbose(verbose_) {}

    void beginFrame(MapID, std::uint64_t frameNo) override;
    void endFrame(MapID, std::uint64_t frameNo) override;

    void onDrawableAdd(const DrawableAdd&) override;
    void onDrawableRemove(const DrawableRemove&) override;
    void onUboUpdate(const UboUpdate&) override;
    void onTextureUpdate(const TextureUpdate&) override;
    void onStencilTiles(const StencilTiles&) override;
    void onFrameOrder(const FrameOrder&) override;

    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t drawableAdds = 0;
        std::uint64_t drawableRemoves = 0;
        std::uint64_t uboUpdates = 0;
        std::uint64_t textureUpdates = 0;
        std::uint64_t stencilTileSets = 0;
        std::uint64_t drawsOrdered = 0;
        std::uint64_t liveDrawables = 0;
        std::uint64_t addsCreated = 0;
        std::uint64_t addsIndexDataReplaced = 0;
        std::uint64_t addsAttributesReplaced = 0;
        std::uint64_t addsAttributesModified = 0;
    };
    const Stats& getStats() const { return stats; }

private:
    bool verbose;
    Stats stats;
};

} // namespace capture
} // namespace mln
