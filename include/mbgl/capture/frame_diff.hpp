#pragma once

#include <mbgl/gfx/types.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/identity.hpp>
#include <mbgl/util/rect.hpp>
#include <mbgl/util/size.hpp>

#include <array>
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
    /// The type the *buffer* supplies.
    gfx::AttributeDataType dataType = gfx::AttributeDataType::Invalid;

    /// The type the *shader* declares for this slot, which is not always the same thing and
    /// is the one a binding must use.
    ///
    /// A shader always declares the zoom-interpolated width, because it has to handle a
    /// property that varies with zoom: fill's color is `Float4`, a packed min/max pair mixed
    /// by `color_t`. The binder only supplies both halves when the property is actually
    /// interpolated (`PaintPropertyBinder::isInterpolated`); a `match` on a feature property
    /// is per-feature but constant across zoom, so it supplies `Float2` and the tweaker sets
    /// `color_t = 0`, leaving the shader reading `.xy` and never touching `.zw`.
    ///
    /// So the buffer legitimately carries fewer components than the attribute declares. Bind
    /// the declared type with the supplied offset and stride; binding the supplied type gives
    /// the shader a narrower attribute than it reads.
    gfx::AttributeDataType declaredDataType = gfx::AttributeDataType::Invalid;

    /// Shared source buffer. Null when the attribute carries per-vertex values inline
    /// (`rawCount` elements) rather than referencing a bucket vector.
    std::shared_ptr<gfx::VertexVectorBase> sharedVector;
    uint32_t offset = 0;
    uint32_t vertexOffset = 0;
    uint32_t stride = 0;

    /// Element count when `sharedVector` is null.
    std::size_t rawCount = 0;

    /// Owned vertex bytes for the raw-vertex path. The background layer is the one mainline
    /// user: it builds its quad on the CPU and hands the bytes over with `setRawVertices`,
    /// naming the attribute they feed, rather than referencing a bucket vector. Shared so the
    /// consumer can retain it for as long as the GPU needs it.
    std::shared_ptr<const std::vector<std::uint8_t>> rawData;
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

    /// Index of the layer group that owns this drawable. Pairs with `UboUpdate::layerIndex`
    /// so a consumer can find the consolidated UBO array this drawable indexes into.
    std::int32_t layerIndex = -1;

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
    /// True for the frame-wide buffers the renderer maintains rather than any one layer --
    /// `GlobalPaintParamsUBO` and friends. Neither `layerIndex` nor `ownerId` is set for
    /// these. Layers that size geometry in screen space rather than tile space
    /// (circle billboards, line widths) cannot be reconstructed without them.
    bool isGlobal = false;
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

    /// BORROWED, like `UboUpdate::data`: points at the capture texture's own storage and is
    /// valid only for the duration of the `onTextureUpdate` call. A consumer that uploads
    /// asynchronously must copy first -- the next atlas insert rewrites this in place.
    const void* pixels = nullptr;
    std::size_t pixelBytes = 0;
};

/// One tile of a clip set: which tile, and the matrix that puts its mask quad on screen.
///
/// The matrix is `PaintParameters::matrixForTile`, the same call every backend's
/// `renderTileClippingMasks` makes when it fills `shaders::ClipUBO`. It has to travel with the
/// tile because the consumer draws the quad itself and has nothing else to derive it from --
/// in particular *not* a content drawable's own matrix, which carries the layer's translate on
/// top of the tile transform and would put the mask somewhere the content is not.
struct StencilTile {
    OverscaledTileID id;
    /// Column-major tile-to-clip, as every other matrix on this protocol is.
    std::array<float, 16> matrix{};
};

/// The tile set a layer group wants clipped. mbgl never produces clipping-mask *drawables* on
/// any backend we use — `PaintParameters::renderTileClippingMasks` calls into the concrete
/// backend Context directly — so the consumer synthesizes the masks from this. Plan §3.4.1.
///
/// Stencil *reference values* are deliberately not carried. mbgl assigns them from a running
/// counter it resets when the value would overflow the buffer, which is bookkeeping about a
/// stencil buffer this side does not own. The consumer assigns its own and keys them by
/// `DrawableAdd::tileID`, which is the same mapping mbgl makes at draw time
/// (`vulkan/drawable.cpp:331` looks the mode up by `tileID->toUnwrapped()`).
struct StencilTiles {
    MapID mapId = 0;
    std::int32_t layerIndex = 0;
    std::vector<StencilTile> tiles;
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

    /// World -> clip for this frame: mbgl's own `TransformParameters::projMatrix`, which is
    /// view and projection together.
    ///
    /// This is the half of the drawable matrix that does not vary per tile. mbgl builds every
    /// tile's matrix as `projMatrix * matrixFor(tileID)` (paint_parameters.cpp:111-115), where
    /// the second factor is a translate and a scale placing the tile in a world space all
    /// tiles share. Phase A forwards only the product, which is why the mirror's camera has to
    /// contribute nothing; carrying the factors separately is what lets a consumer put the
    /// world on a real camera and place its own 3D content in the same space. See plan §5.
    ///
    /// Column-major and double, as mbgl keeps it -- the world coordinates it multiplies are
    /// large enough at high zoom that the precision matters.
    std::array<double, 16> projMatrix{};
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
