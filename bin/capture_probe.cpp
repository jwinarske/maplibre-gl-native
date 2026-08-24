// Phase 0 probe for the ECS capture backend.
//
// Runs the complete MapLibre frontend -- Map, Style, sources, tile layout, buckets, layer
// drawable construction, tweakers, the whole RenderOrchestrator pass -- against
// `capture::RendererBackend`, and reports the FrameDiff stream that comes out.
//
// The point is to demonstrate the Phase 0 exit criterion: mbgl completes real frames with no
// GPU of any kind present, and background/fill/line drawables arrive with their vertex
// attribute descriptors and shader permutation keys resolved.
//
// By default it renders a self-contained style (inline GeoJSON, no network) so the run is
// hermetic. Pass a style URL or file path to point it at a real style instead.

#include <mbgl/capture/frame_diff.hpp>
#include <mbgl/capture/renderer_backend.hpp>

#include <mbgl/gfx/backend.hpp>
#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_observer.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/renderer/renderer.hpp>
#include <mbgl/renderer/renderer_frontend.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/style/transition_options.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/run_loop.hpp>

// Buffer contents are hashed into the dump, and the vector types that own them live under
// src/ rather than include/. The probe already links mbgl-core; capture.cmake adds src/ to
// its include path for this.
#include <mbgl/gfx/index_vector.hpp>
#include <mbgl/gfx/vertex_vector.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace mln;

namespace {

constexpr const char* kInlineStyle = R"JSON({
  "version": 8,
  "name": "capture-probe",
  "sources": {
    "probe": {
      "type": "geojson",
      "data": {
        "type": "FeatureCollection",
        "features": [
          {
            "type": "Feature",
            "properties": { "kind": "a" },
            "geometry": {
              "type": "Polygon",
              "coordinates": [[[-0.10,51.49],[-0.10,51.53],[-0.05,51.53],[-0.05,51.49],[-0.10,51.49]]]
            }
          },
          {
            "type": "Feature",
            "properties": { "kind": "b" },
            "geometry": {
              "type": "Polygon",
              "coordinates": [[[-0.16,51.49],[-0.16,51.52],[-0.12,51.52],[-0.12,51.49],[-0.16,51.49]]]
            }
          },
          {
            "type": "Feature",
            "properties": { "kind": "a" },
            "geometry": {
              "type": "LineString",
              "coordinates": [[-0.18,51.48],[-0.12,51.51],[-0.06,51.50],[-0.02,51.53]]
            }
          },
          {
            "type": "Feature",
            "properties": { "kind": "b" },
            "geometry": { "type": "Point", "coordinates": [-0.09,51.505] }
          }
        ]
      }
    }
  },
  "layers": [
    {
      "id": "bg",
      "type": "background",
      "paint": { "background-color": "#101418" }
    },
    {
      "id": "fill-constant",
      "type": "fill",
      "source": "probe",
      "filter": ["==", "$type", "Polygon"],
      "paint": { "fill-color": "#2f6f4f", "fill-opacity": 0.8 }
    },
    {
      "id": "fill-datadriven",
      "type": "fill",
      "source": "probe",
      "filter": ["==", "$type", "Polygon"],
      "paint": {
        "fill-color": ["match", ["get", "kind"], "a", "#c04030", "#3050c0"],
        "fill-opacity": ["match", ["get", "kind"], "a", 0.5, 0.9]
      }
    },
    {
      "id": "line-datadriven",
      "type": "line",
      "source": "probe",
      "filter": ["==", "$type", "LineString"],
      "paint": {
        "line-color": ["match", ["get", "kind"], "a", "#e0d040", "#40e0d0"],
        "line-width": ["match", ["get", "kind"], "a", 4.0, 2.0]
      }
    },
    {
      "id": "circle-constant",
      "type": "circle",
      "source": "probe",
      "filter": ["==", "$type", "Point"],
      "paint": { "circle-color": "#ffffff", "circle-radius": 8 }
    }
  ]
})JSON";

/// Deterministic serialization of the capture stream, for the golden-oracle diff (plan §9.1).
///
/// The Rust frontend runs the same style at the same camera and its normalized dump is
/// compared against this one. What that turns into is a failing text diff, rather than
/// archaeology, when the two disagree about an attribute descriptor, a permutation key, a
/// segment table or a UBO byte.
///
/// # It dumps state, not the event log
///
/// The obvious thing is to serialize every callback in order. That would diff badly for a
/// reason that has nothing to do with correctness: two implementations can reach the same
/// frame through different intermediate steps -- a different number of rebuilds, a different
/// tile arrival order -- and be equally right. So this accumulates the live state and emits a
/// snapshot: which drawables exist now, what the last value of each UBO is, what the current
/// draw order is. The event-log properties that do matter (churn taxonomy, dirty-only UBO
/// traffic) are counter assertions in §9.3 instead, where they belong.
///
/// # Identity is content, not pointers
///
/// `util::SimpleIdentity` is allocation-ordered and means nothing to another implementation.
/// Every drawable is keyed instead by what it structurally is -- layer, sublayer, tile,
/// shader, permutation, vertex count -- with ties broken by a content hash and an ordinal.
/// The structural part leads so a content difference shows up as a changed line rather than
/// as a reordering of the whole file.
///
/// # Floats are bits
///
/// Doubles are emitted as their bit patterns. A decimal rendering invites the two sides to
/// disagree about formatting rather than about values, which is a false diff, and the one
/// regression this file exists to catch (`centerZoom0` scale-freeness) is precisely a case
/// where the last few bits are the whole question.
class DumpFrameSink final : public capture::FrameSink {
public:
    explicit DumpFrameSink(capture::FrameSink& inner_)
        : inner(inner_) {}

    void beginFrame(capture::MapID id, std::uint64_t frameNo) override { inner.beginFrame(id, frameNo); }
    void endFrame(capture::MapID id, std::uint64_t frameNo) override { inner.endFrame(id, frameNo); }

    void onDrawableAdd(const capture::DrawableAdd& add) override {
        inner.onDrawableAdd(add);
        drawables[add.id.id()] = record(add);
    }

    void onDrawableRemove(const capture::DrawableRemove& remove) override {
        inner.onDrawableRemove(remove);
        drawables.erase(remove.id.id());
    }

    void onUboUpdate(const capture::UboUpdate& update) override {
        inner.onUboUpdate(update);
        UboKey key{};
        if (update.isGlobal) {
            key.scope = UboKey::Global;
        } else if (update.layerIndex) {
            key.scope = UboKey::Layer;
            key.owner = *update.layerIndex;
        } else if (update.ownerId) {
            key.scope = UboKey::Owner;
            key.owner = static_cast<std::int64_t>(update.ownerId->id());
        }
        key.slot = update.slot;
        const auto* raw = static_cast<const std::uint8_t*>(update.data);
        ubos[key] = UboValue{std::vector<std::uint8_t>(raw, raw + update.size)};
    }

    void onTextureUpdate(const capture::TextureUpdate& update) override {
        inner.onTextureUpdate(update);
        auto& texture = textures[update.id.id()];
        texture.width = update.size.width;
        texture.height = update.size.height;
        texture.format = static_cast<int>(update.format);
        // Successive sub-region uploads each contribute, so the running hash reflects the
        // whole upload history rather than only the last rect.
        texture.hash = hash(update.pixels, update.pixelBytes, texture.hash);
    }

    void onStencilTiles(const capture::StencilTiles& tiles) override {
        inner.onStencilTiles(tiles);
        auto& set = stencils[tiles.layerIndex];
        set.clear();
        for (const auto& tile : tiles.tiles) {
            set.push_back(StencilRecord{tileKey(tile.id), hash(tile.matrix.data(), sizeof(float) * 16)});
        }
        std::sort(set.begin(), set.end(), [](const StencilRecord& a, const StencilRecord& b) {
            return a.tile < b.tile;
        });
    }

    void onFrameOrder(const capture::FrameOrder& frame) override {
        inner.onFrameOrder(frame);
        order = frame;
        haveOrder = true;
    }

    void dump(std::FILE* out) const;

    /// Prints the actual vertex coordinates, for working out what the frontend should be
    /// producing rather than guessing from a hash.
    ///
    /// Deliberately not part of the dump: a hash is what makes the golden file bounded and
    /// diffable, and inlining every coordinate of a real style would make it neither. This is
    /// a measurement tool, and it is what the tile-coordinate pipeline gets built against.
    void dumpVertices(std::FILE* out) const;

    /// Replaces raw permutation keys with indices that do not depend on the build.
    void assignPermutationIndices() const;

private:
    struct AttrRecord {
        std::size_t attrId = 0;
        int index = -1;
        int dataType = 0;
        int declaredDataType = 0;
        std::uint32_t offset = 0;
        std::uint32_t vertexOffset = 0;
        std::uint32_t stride = 0;
        std::size_t sourceCount = 0;
        std::uint64_t sourceHash = 0;
        /// Raw bytes, kept only for --dump-vertices. Not hashed and not part of the dump.
        std::vector<std::uint8_t> raw;
    };

    struct DrawableRecord {
        std::int32_t layerIndex = -1;
        std::int32_t subLayerIndex = 0;
        std::string tile;
        int shader = 0;
        std::uint64_t permutationKey = 0;
        /// Canonical index of `permutationKey`, assigned in `assignPermutationIndices`.
        int permutationIndex = 0;
        std::size_t vertexCount = 0;
        int vertexType = 0;
        std::uint8_t renderPass = 0;
        bool is3D = false;
        bool enableStencil = false;
        bool enableDepth = false;
        bool enableColor = true;
        std::size_t indexCount = 0;
        std::uint64_t indexHash = 0;
        std::vector<AttrRecord> attrs;
        std::vector<AttrRecord> instanceAttrs;
        std::vector<capture::SegmentDesc> segments;
        std::vector<std::pair<std::size_t, std::int64_t>> textureSlots;

        /// Sorts and diffs by what the drawable structurally is, so a content difference is a
        /// changed line rather than a reshuffle.
        ///
        /// The permutation appears as a canonical index rather than its raw key. The key is
        /// `hash_combine(propertiesAsUniforms, programParameters.getDefinesHash())`, so its
        /// value depends on how the engine was *built* and not on what the map contains: two
        /// builds of the same commit with different CMake options produce different keys for
        /// identical geometry. What is a protocol property is the grouping -- which drawables
        /// need the same shader variant -- and an index preserves that while dropping the part
        /// that is an artifact of the build.
        std::string structuralKey() const {
            char buf[256];
            std::snprintf(buf,
                          sizeof(buf),
                          "L%05d.S%05d.%s.sh%04d.pk%04d.v%08zu",
                          layerIndex,
                          subLayerIndex,
                          tile.c_str(),
                          shader,
                          permutationIndex,
                          vertexCount);
            return buf;
        }

        /// The key this drawable sorts under, ignoring the permutation entirely.
        ///
        /// Used to number permutations deterministically: the numbering cannot be derived from
        /// the raw keys, because their relative order is as arbitrary as their values.
        std::string permutationFreeKey() const {
            char buf[256];
            std::snprintf(buf,
                          sizeof(buf),
                          "L%05d.S%05d.%s.sh%04d.v%08zu",
                          layerIndex,
                          subLayerIndex,
                          tile.c_str(),
                          shader,
                          vertexCount);
            return buf;
        }

        std::uint64_t contentHash() const;
    };

    struct UboKey {
        enum Scope { Global, Layer, Owner } scope = Global;
        std::int64_t owner = 0;
        std::size_t slot = 0;
        bool operator<(const UboKey& other) const {
            if (scope != other.scope) return scope < other.scope;
            if (owner != other.owner) return owner < other.owner;
            return slot < other.slot;
        }
    };

    struct UboValue {
        std::vector<std::uint8_t> bytes;
    };

    struct TextureRecord {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        int format = 0;
        std::uint64_t hash = kFnvOffset;
    };

    struct StencilRecord {
        std::string tile;
        std::uint64_t matrixHash = 0;
    };

    static constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;

    static std::uint64_t hash(const void* data, std::size_t len, std::uint64_t seed = kFnvOffset) {
        auto h = seed;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (bytes) {
            for (std::size_t i = 0; i < len; ++i) {
                h = (h ^ bytes[i]) * 0x100000001b3ull;
            }
        }
        return h;
    }

    /// Hashes an index buffer in a form that does not depend on triangle emission order.
    ///
    /// earcutr and earcut.hpp produce the *same* triangulation — measured, on a square with a
    /// square hole: the same eight triangles, the same total area, and every one with the same
    /// winding — but they emit those triangles in a different order once holes are involved.
    /// Simple polygons, including concave ones, agree index-for-index.
    ///
    /// Emission order is not a property of the map. The triangles are independent, the
    /// rendered result is identical, and nothing downstream depends on the sequence. Hashing
    /// the raw buffer would therefore fail the diff on every polygon with a hole, for a
    /// difference that means nothing, and it would look like a tessellation bug.
    ///
    /// So each triangle is rotated to start at its lowest index and the triangles are sorted.
    /// Rotation preserves winding, which *is* a real property — a reversed triangle is
    /// backface-culled — so a winding difference still fails, as it should. What is discarded
    /// is exactly what carries no meaning.
    static std::uint64_t canonicalIndexHash(const std::uint16_t* indices, std::size_t count) {
        if (!indices || count == 0) {
            return kFnvOffset;
        }
        // Not a triangle list. Nothing to canonicalize, so hash it as it stands.
        if (count % 3 != 0) {
            return hash(indices, count * sizeof(std::uint16_t));
        }

        std::vector<std::array<std::uint16_t, 3>> triangles;
        triangles.reserve(count / 3);
        for (std::size_t i = 0; i < count; i += 3) {
            std::array<std::uint16_t, 3> tri{indices[i], indices[i + 1], indices[i + 2]};
            // Rotate to lowest-first: removes the rotation ambiguity, keeps the winding.
            const auto lowest = std::distance(tri.begin(), std::min_element(tri.begin(), tri.end()));
            std::rotate(tri.begin(), tri.begin() + lowest, tri.end());
            triangles.push_back(tri);
        }
        std::sort(triangles.begin(), triangles.end());

        std::uint64_t h = kFnvOffset;
        for (const auto& tri : triangles) {
            h = hash(tri.data(), sizeof(tri), h);
        }
        return h;
    }

    static std::string tileKey(const OverscaledTileID& id) {
        char buf[64];
        std::snprintf(buf,
                      sizeof(buf),
                      "t%02d_%08u_%08u_o%02d_w%+04d",
                      static_cast<int>(id.canonical.z),
                      id.canonical.x,
                      id.canonical.y,
                      static_cast<int>(id.overscaledZ),
                      static_cast<int>(id.wrap));
        return buf;
    }

    static AttrRecord attrRecord(const capture::AttributeDesc& desc);
    static DrawableRecord record(const capture::DrawableAdd& add);

    /// Assigns every drawable a stable key, and returns them in dump order.
    std::vector<std::pair<std::string, const DrawableRecord*>> keyed() const;

    capture::FrameSink& inner;
    std::map<std::int64_t, DrawableRecord> drawables;
    std::map<UboKey, UboValue> ubos;
    std::map<std::int64_t, TextureRecord> textures;
    std::map<std::int32_t, std::vector<StencilRecord>> stencils;
    capture::FrameOrder order;
    bool haveOrder = false;
};

DumpFrameSink::AttrRecord DumpFrameSink::attrRecord(const capture::AttributeDesc& desc) {
    AttrRecord out;
    out.attrId = desc.attrId;
    out.index = desc.index;
    out.dataType = static_cast<int>(desc.dataType);
    out.declaredDataType = static_cast<int>(desc.declaredDataType);
    out.offset = desc.offset;
    out.vertexOffset = desc.vertexOffset;
    out.stride = desc.stride;
    if (desc.sharedVector) {
        // getRawSize() is sizeof(Vertex) -- the stride, not the total. Hashing it directly
        // covered only the first vertex, which made every attribute hash in the dump far
        // weaker than it looked: two buffers agreeing on their first vertex and differing
        // everywhere after it hashed identically.
        out.sourceCount = desc.sharedVector->getRawCount();
        const std::size_t bytes = desc.sharedVector->getRawSize() * desc.sharedVector->getRawCount();
        out.sourceHash = hash(desc.sharedVector->getRawData(), bytes);
        const auto* raw = static_cast<const std::uint8_t*>(desc.sharedVector->getRawData());
        if (raw) {
            out.raw.assign(raw, raw + bytes);
        }
    } else if (desc.rawData) {
        // The background layer's owned-bytes path. Rev 2 folds both into one slab reference,
        // so the two are deliberately indistinguishable here.
        out.sourceCount = desc.rawCount;
        out.sourceHash = hash(desc.rawData->data(), desc.rawData->size());
        out.raw = *desc.rawData;
    }
    return out;
}

DumpFrameSink::DrawableRecord DumpFrameSink::record(const capture::DrawableAdd& add) {
    DrawableRecord out;
    out.layerIndex = add.layerIndex;
    out.subLayerIndex = add.subLayerIndex;
    out.tile = add.tileID ? tileKey(*add.tileID) : std::string{"tnone"};
    out.shader = static_cast<int>(add.builtinShader);
    out.permutationKey = add.permutationKey;
    out.vertexCount = add.vertexCount;
    out.vertexType = static_cast<int>(add.vertexType);
    out.renderPass = add.renderPass;
    out.is3D = add.is3D;
    out.enableStencil = add.enableStencil;
    out.enableDepth = add.enableDepth;
    out.enableColor = add.enableColor;
    if (add.indexes) {
        out.indexCount = add.indexes->elements();
        out.indexHash = canonicalIndexHash(add.indexes->data(), add.indexes->elements());
    }
    for (const auto& attr : add.attrs) {
        out.attrs.push_back(attrRecord(attr));
    }
    for (const auto& attr : add.instanceAttrs) {
        out.instanceAttrs.push_back(attrRecord(attr));
    }
    // Attribute order is a shader-table detail rather than protocol, so sort by the id the
    // binding actually resolves through.
    const auto byAttrId = [](const AttrRecord& a, const AttrRecord& b) { return a.attrId < b.attrId; };
    std::sort(out.attrs.begin(), out.attrs.end(), byAttrId);
    std::sort(out.instanceAttrs.begin(), out.instanceAttrs.end(), byAttrId);
    out.segments = add.segments;
    for (const auto& [slot, id] : add.textureRefs) {
        out.textureSlots.emplace_back(slot, id.id());
    }
    std::sort(out.textureSlots.begin(), out.textureSlots.end());
    return out;
}

std::uint64_t DumpFrameSink::DrawableRecord::contentHash() const {
    auto h = hash(&renderPass, sizeof(renderPass));
    h = hash(&indexHash, sizeof(indexHash), h);
    h = hash(&vertexType, sizeof(vertexType), h);
    for (const auto& attr : attrs) {
        h = hash(&attr, sizeof(attr), h);
    }
    for (const auto& attr : instanceAttrs) {
        h = hash(&attr, sizeof(attr), h);
    }
    for (const auto& seg : segments) {
        h = hash(&seg, sizeof(seg), h);
    }
    return h;
}

std::vector<std::pair<std::string, const DumpFrameSink::DrawableRecord*>> DumpFrameSink::keyed() const {
    std::vector<std::pair<std::string, const DrawableRecord*>> out;
    assignPermutationIndices();

    // Group by structure, order within a group by content, then number them. Two drawables
    // that are structurally identical still need distinct stable names.
    std::map<std::string, std::vector<const DrawableRecord*>> groups;
    for (const auto& [id, record] : drawables) {
        groups[record.structuralKey()].push_back(&record);
    }
    for (auto& [key, group] : groups) {
        std::sort(group.begin(), group.end(), [](const DrawableRecord* a, const DrawableRecord* b) {
            return a->contentHash() < b->contentHash();
        });
        for (std::size_t i = 0; i < group.size(); ++i) {
            char buf[288];
            std::snprintf(buf, sizeof(buf), "%s#%02zu", key.c_str(), i);
            out.emplace_back(buf, group[i]);
        }
    }
    return out;
}

/// Numbers the permutations so the dump does not carry a build artifact.
///
/// The raw key is a hash over the shader's uniform-property set and the engine's compiled-in
/// defines, so it changes when CMake options change and stays the same when the map does. The
/// grouping it induces is what a consumer actually needs -- which drawables want the same shader
/// variant -- and that survives renumbering.
///
/// Indices are assigned in order of the drawables' *permutation-free* key, because the raw keys
/// cannot order themselves: their relative order is as arbitrary as their values, so numbering
/// by sorted raw key would be stable within one build and meaningless across two.
void DumpFrameSink::assignPermutationIndices() const {
    std::map<std::string, std::uint64_t> firstByPosition;
    for (const auto& [id, record] : drawables) {
        firstByPosition.emplace(record.permutationFreeKey(), record.permutationKey);
    }

    std::map<std::uint64_t, int> indices;
    int next = 0;
    for (const auto& [position, key] : firstByPosition) {
        if (indices.emplace(key, next).second) {
            ++next;
        }
    }

    for (auto& [id, record] : const_cast<std::map<std::int64_t, DrawableRecord>&>(drawables)) {
        const auto found = indices.find(record.permutationKey);
        record.permutationIndex = found == indices.end() ? -1 : found->second;
    }
}

void DumpFrameSink::dumpVertices(std::FILE* out) const {
    for (const auto& [key, d] : keyed()) {
        std::fprintf(out, "%s vertices=%zu\n", key.c_str(), d->vertexCount);
        for (const auto& a : d->attrs) {
            // Short2 is the position attribute: two int16 per vertex, stride 4.
            const bool isShort2 = a.dataType == 9;
            std::fprintf(out,
                         "  attr id=%zu dt=%d count=%zu bytes=%zu%s\n",
                         a.attrId,
                         a.dataType,
                         a.sourceCount,
                         a.raw.size(),
                         isShort2 ? " (position)" : "");
            if (!isShort2 || a.raw.size() < 4) {
                continue;
            }
            const auto* values = reinterpret_cast<const std::int16_t*>(a.raw.data());
            const std::size_t pairs = a.raw.size() / 4;
            std::fprintf(out, "   ");
            for (std::size_t i = 0; i < pairs; ++i) {
                std::fprintf(out, " (%d,%d)", values[i * 2], values[i * 2 + 1]);
            }
            std::fprintf(out, "\n");
        }
    }
}

void DumpFrameSink::dump(std::FILE* out) const {
    const auto bits = [](double value) {
        std::uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        return raw;
    };
    const auto bits32 = [](float value) {
        std::uint32_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        return raw;
    };

    std::fprintf(out, "tessella-capture-dump 1\n");

    const auto entries = keyed();
    std::map<std::int64_t, std::string> idToKey;
    for (const auto& [id, record] : drawables) {
        (void)record;
    }
    // Rebuild id -> key by matching pointers, so the draw order can name drawables the same
    // way the drawable section does.
    for (const auto& [key, record] : entries) {
        for (const auto& [id, candidate] : drawables) {
            if (&candidate == record) {
                idToKey[id] = key;
                break;
            }
        }
    }

    std::fprintf(out, "drawables %zu\n", entries.size());
    for (const auto& [key, d] : entries) {
        std::fprintf(out,
                     "drawable %s pass=%u vtype=%d flags=%d%d%d%d idx=%zu:%016" PRIx64 " segs=%zu\n",
                     key.c_str(),
                     static_cast<unsigned>(d->renderPass),
                     d->vertexType,
                     d->is3D ? 1 : 0,
                     d->enableStencil ? 1 : 0,
                     d->enableDepth ? 1 : 0,
                     d->enableColor ? 1 : 0,
                     d->indexCount,
                     d->indexHash,
                     d->segments.size());
        for (const auto& a : d->attrs) {
            std::fprintf(out,
                         "  attr %s id=%zu bind=%d dt=%d ddt=%d off=%u voff=%u stride=%u src=%zu:%016" PRIx64 "\n",
                         key.c_str(),
                         a.attrId,
                         a.index,
                         a.dataType,
                         a.declaredDataType,
                         a.offset,
                         a.vertexOffset,
                         a.stride,
                         a.sourceCount,
                         a.sourceHash);
        }
        for (const auto& a : d->instanceAttrs) {
            std::fprintf(out,
                         "  iattr %s id=%zu bind=%d dt=%d ddt=%d off=%u voff=%u stride=%u src=%zu:%016" PRIx64 "\n",
                         key.c_str(),
                         a.attrId,
                         a.index,
                         a.dataType,
                         a.declaredDataType,
                         a.offset,
                         a.vertexOffset,
                         a.stride,
                         a.sourceCount,
                         a.sourceHash);
        }
        for (const auto& s : d->segments) {
            std::fprintf(out,
                         "  seg %s v=%zu i=%zu vlen=%zu ilen=%zu\n",
                         key.c_str(),
                         s.vertexOffset,
                         s.indexOffset,
                         s.vertexLength,
                         s.indexLength);
        }
        for (const auto& [slot, id] : d->textureSlots) {
            std::fprintf(out, "  tex %s slot=%zu tex=%" PRId64 "\n", key.c_str(), slot, id);
        }
    }

    std::fprintf(out, "ubos %zu\n", ubos.size());
    for (const auto& [key, value] : ubos) {
        const char* scope = key.scope == UboKey::Global ? "global" : (key.scope == UboKey::Layer ? "layer" : "owner");
        std::string owner = std::to_string(key.owner);
        if (key.scope == UboKey::Owner) {
            const auto it = idToKey.find(key.owner);
            owner = (it != idToKey.end()) ? it->second : std::string{"?"};
        }
        // A consolidated buffer is an array indexed by uboIndex, and uboIndex comes from the
        // same arbitrary tile iteration the draw order does, so the array arrives permuted
        // between runs. Its 16-byte blocks are sorted here -- 16 being the std140/std430
        // alignment unit, so a block boundary never falls inside a member. That deliberately
        // discards intra-array order, which is exactly the part that carries no meaning;
        // attributing each slice back to its drawable needs a stride the stream does not
        // carry, and is worth doing once the Rust side exists to compare against.
        //
        // The global paint params buffer is a single struct rather than an array, so it is
        // left in order. Its symbol_fade_change field at offset 28 is a wall-clock fade
        // ratio, which two implementations have no reason to agree on and which varied on
        // every run here; it is elided rather than compared.
        const bool isGlobal = key.scope == UboKey::Global;
        std::fprintf(out, "ubo %s:%s slot=%zu size=%zu bytes=", scope, owner.c_str(), key.slot, value.bytes.size());

        std::vector<std::string> blocks;
        const std::size_t blockSize = 16;
        if (!isGlobal && value.bytes.size() > blockSize && value.bytes.size() % blockSize == 0) {
            for (std::size_t off = 0; off < value.bytes.size(); off += blockSize) {
                std::string block;
                for (std::size_t i = 0; i < blockSize; ++i) {
                    char hex[3];
                    std::snprintf(hex, sizeof(hex), "%02x", value.bytes[off + i]);
                    block += hex;
                }
                blocks.push_back(std::move(block));
            }
            std::sort(blocks.begin(), blocks.end());
            for (const auto& block : blocks) {
                std::fprintf(out, "%s", block.c_str());
            }
            std::fprintf(out, " (16-byte blocks sorted)");
        } else {
            for (std::size_t i = 0; i < value.bytes.size(); ++i) {
                const bool elide = isGlobal && key.slot == 0 && i >= 28 && i < 32;
                if (elide) {
                    std::fprintf(out, "--");
                } else {
                    std::fprintf(out, "%02x", value.bytes[i]);
                }
            }
            if (isGlobal && key.slot == 0 && value.bytes.size() >= 32) {
                std::fprintf(out, " (symbol_fade_change elided)");
            }
        }
        std::fprintf(out, "\n");
    }

    std::fprintf(out, "textures %zu\n", textures.size());
    {
        std::vector<const TextureRecord*> sorted;
        for (const auto& [id, texture] : textures) {
            sorted.push_back(&texture);
        }
        std::sort(sorted.begin(), sorted.end(), [](const TextureRecord* a, const TextureRecord* b) {
            if (a->width != b->width) return a->width < b->width;
            if (a->height != b->height) return a->height < b->height;
            if (a->format != b->format) return a->format < b->format;
            return a->hash < b->hash;
        });
        for (const auto* t : sorted) {
            std::fprintf(out,
                         "texture %ux%u fmt=%d hash=%016" PRIx64 "\n",
                         t->width,
                         t->height,
                         t->format,
                         t->hash);
        }
    }

    for (const auto& [layer, set] : stencils) {
        std::fprintf(out, "stencil layer=%d tiles=%zu\n", layer, set.size());
        for (const auto& s : set) {
            std::fprintf(out, "  stencil-tile layer=%d %s m=%016" PRIx64 "\n", layer, s.tile.c_str(), s.matrixHash);
        }
    }

    if (!haveOrder) {
        std::fprintf(out, "order none\n");
        return;
    }

    // Draw order is canonicalized within each (pass, layer, sublayer, priority) group.
    //
    // mbgl's iteration over a layer's tiles is not deterministic: across three runs of the
    // same style at the same camera, thirty draw entries permuted and the consolidated SSBO
    // permuted with them, because uboIndex is assigned from that same iteration. Rendering is
    // unaffected -- within a layer the cover tiles do not overlap, and the plan resolves their
    // relative order with the stencil rather than the painter's algorithm (§11.2). So the
    // sequence within a group is not a protocol property and must not be diffed as one. What
    // is a protocol property, the grouping and the relative order of groups, is preserved
    // exactly, because that is what painter order means.
    //
    // uboIndex is deliberately not emitted: it is an artifact of the same arbitrary
    // iteration. What it points at is emitted with the drawable instead.
    std::vector<const capture::DrawOrderEntry*> ordered;
    ordered.reserve(order.ordered.size());
    for (const auto& e : order.ordered) {
        ordered.push_back(&e);
    }
    const auto keyOf = [&](const capture::DrawOrderEntry* e) {
        const auto it = idToKey.find(e->id.id());
        return it != idToKey.end() ? it->second : std::string{"?"};
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const capture::DrawOrderEntry* a, const capture::DrawOrderEntry* b) {
        if (a->pass != b->pass) return a->pass < b->pass;
        if (a->layerIndex != b->layerIndex) return a->layerIndex < b->layerIndex;
        if (a->subLayerIndex != b->subLayerIndex) return a->subLayerIndex < b->subLayerIndex;
        if (a->drawPriority != b->drawPriority) return a->drawPriority < b->drawPriority;
        return keyOf(a) < keyOf(b);
    });

    std::fprintf(out, "order %zu\n", ordered.size());
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const auto& e = *ordered[i];
        std::fprintf(out,
                     "draw %04zu %s pass=%u layer=%u sub=%d prio=%" PRId64 "\n",
                     i,
                     keyOf(ordered[i]).c_str(),
                     static_cast<unsigned>(e.pass),
                     e.layerIndex,
                     e.subLayerIndex,
                     static_cast<std::int64_t>(e.drawPriority));
    }

    std::fprintf(out, "camera cutoff=%u depthRange=%08" PRIx32 "\n", order.opaquePassCutoff, bits32(order.depthRangeSize));
    for (int i = 0; i < 16; ++i) {
        std::fprintf(out, "  proj %02d %016" PRIx64 "\n", i, bits(order.projMatrix[i]));
    }
    std::fprintf(out, "  centerZoom0 %016" PRIx64 " %016" PRIx64 "\n", bits(order.centerZoom0[0]), bits(order.centerZoom0[1]));
    std::fprintf(out, "  bearing %016" PRIx64 "\n", bits(order.bearing));
    std::fprintf(out, "  pitch %016" PRIx64 "\n", bits(order.pitch));
    std::fprintf(out, "  pixelsPerMeter %016" PRIx64 "\n", bits(order.pixelsPerMeter));
    std::fprintf(out,
                 "  light dir %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                 bits(order.light.direction[0]),
                 bits(order.light.direction[1]),
                 bits(order.light.direction[2]));
    std::fprintf(out,
                 "  light color %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                 bits(order.light.color[0]),
                 bits(order.light.color[1]),
                 bits(order.light.color[2]),
                 bits(order.light.color[3]));
    std::fprintf(out, "  light intensity %016" PRIx64 "\n", bits(order.light.intensity));
    std::fprintf(out, "  light anchoredToMap %d\n", order.light.anchoredToMap ? 1 : 0);
}

/// Minimal `RendererFrontend`: owns the capture backend and a `Renderer`, exactly as
/// `MapLibreFrontend` in maplibre_view owns a real one. Pull model -- `update()` stores the
/// parameters and the caller decides when to render, which is the dirty-gating discipline
/// the plan calls for (§8.3).
class CaptureFrontend final : public RendererFrontend {
public:
    CaptureFrontend(Size size, float pixelRatio, capture::FrameSink& sink)
        : backend(std::make_unique<capture::RendererBackend>(size, sink)),
          renderer(std::make_unique<Renderer>(*backend, pixelRatio)) {}

    ~CaptureFrontend() override = default;

    void reset() override { renderer.reset(); }

    void setObserver(RendererObserver& observer) override {
        if (renderer) {
            renderer->setObserver(&observer);
        }
    }

    void update(std::shared_ptr<UpdateParameters> params) override {
        updateParameters = std::move(params);
        dirty = true;
    }

    const TaggedScheduler& getThreadPool() const override { return backend->getThreadPool(); }

    /// Returns true if a frame was actually produced.
    bool renderFrameIfDirty() {
        if (!renderer || !updateParameters || !dirty) {
            return false;
        }
        dirty = false;
        gfx::BackendScope guard{*backend};
        auto params = updateParameters;
        renderer->render(params);
        return true;
    }

private:
    std::unique_ptr<capture::RendererBackend> backend;
    std::unique_ptr<Renderer> renderer;
    std::shared_ptr<UpdateParameters> updateParameters;
    bool dirty = false;
};

class ProbeObserver final : public MapObserver {
public:
    void onDidFinishLoadingStyle() override {
        styleLoaded = true;
        Log::Info(Event::General, "probe: style loaded");
    }
    void onDidFailLoadingMap(MapLoadError, const std::string& what) override { failure = "map load failed: " + what; }

    bool styleLoaded = false;
    std::string failure;
};

} // namespace

int main(int argc, char* argv[]) {
    const std::string styleArg = (argc > 1) ? argv[1] : std::string{};
    const bool verbose = [&] {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-v") == 0) {
                return true;
            }
        }
        return false;
    }();
    // --dump writes the canonical serialization to stdout, --dump=<path> to a file. The
    // oracle diff (§9.1) compares that against the Rust frontend's dump of the same style at
    // the same camera.
    const std::string dumpPath = [&]() -> std::string {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--dump") == 0 || std::strcmp(argv[i], "--dump-vertices") == 0) {
                return "-";
            }
            if (std::strncmp(argv[i], "--dump=", 7) == 0) {
                return argv[i] + 7;
            }
        }
        return {};
    }();
    const bool wantDump = !dumpPath.empty();
    const bool wantVertices = [&] {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--dump-vertices") == 0) {
                return true;
            }
        }
        return false;
    }();
    constexpr int kFrames = 8;

    // Data-driven paint properties should always materialize as vertex attributes or UBO
    // fields rather than GPU-evaluated expression trees (plan §3.1). Only the line layer
    // consults this today, which is exactly the layer in the probe style that needs it.
    gfx::Backend::setEnableGPUExpressionEval(false);

    util::RunLoop runLoop;

    capture::LogFrameSink sink{verbose};
    DumpFrameSink dumpSink{sink};
    // The dump sink forwards everything, so the run is byte-identical whether or not --dump
    // was asked for; only whether a snapshot gets printed at the end differs.
    CaptureFrontend frontend{Size{1024, 768}, 1.0f, dumpSink};
    ProbeObserver observer;

    Map map{frontend,
            observer,
            MapOptions().withMapMode(MapMode::Continuous).withSize(Size{1024, 768}),
            ResourceOptions().withCachePath(":memory:")};

    if (styleArg.empty() || styleArg[0] == '-') {
        map.getStyle().loadJSON(kInlineStyle);
        Log::Info(Event::General, "probe: using built-in offline style");
    } else {
        map.getStyle().loadURL(styleArg);
        Log::Info(Event::General, "probe: loading style " + styleArg);
    }

    // --zoom=<f> moves the camera off the default. A zoom-varying paint property is stored as
    // its value at each end of the tile's zoom range and mixed by a `_t` uniform, and at an
    // exactly integer zoom that mix factor is zero -- so every such uniform reads the same as a
    // style with no zoom dependence at all. Capturing the mix factor at all requires a
    // fractional zoom, which is what this exists for. The default is unchanged, so a capture
    // without it is byte-identical to one from before the flag.
    const double zoom = [&] {
        for (int i = 1; i < argc; ++i) {
            if (std::strncmp(argv[i], "--zoom=", 7) == 0) {
                return std::strtod(argv[i] + 7, nullptr);
            }
        }
        return 13.0;
    }();
    map.jumpTo(CameraOptions().withCenter(LatLng{51.505, -0.11}).withZoom(zoom));

    int framesRendered = 0;
    int framesWithContent = 0;
    int spins = 0;
    constexpr int kMaxSpins = 200000;
    // A dump is only an oracle if it describes a settled state. Tile parse and layout run on
    // the background pool, so stopping after a fixed number of frames stops at whatever
    // happened to have landed -- which varied between six and thirty-seven drawables run to
    // run. Instead, keep going until the live drawable set and the draw order have both been
    // unchanged for several consecutive frames.
    constexpr int kSettleFrames = 16;
    // Transitions already in flight when the style finished loading keep their original
    // duration; pinning the options to zero only affects ones started afterwards. So a paint
    // color was still interpolating at t just short of one, and drifted by a single ULP
    // between runs. The settle therefore also has to outlast mbgl's default 300 ms
    // transition, after which every t saturates at exactly one.
    constexpr auto kSettleAfterLoad = std::chrono::milliseconds{800};
    std::chrono::steady_clock::time_point loadedAt{};
    std::uint64_t lastLive = 0;
    std::uint64_t lastDraws = 0;
    int stableFrames = 0;

    // Tile parse and layout run on the background pool, so the first frames legitimately
    // carry nothing. Keep pumping until drawables actually arrive, then render a few more so
    // the steady-state diff (which should be UBO-only, not geometry) is observable.
    const auto settled = [&] {
        if (!wantDump) {
            return framesWithContent >= kFrames;
        }
        if (stableFrames < kSettleFrames || loadedAt.time_since_epoch().count() == 0) {
            return false;
        }
        return std::chrono::steady_clock::now() - loadedAt > kSettleAfterLoad;
    };

    bool transitionsPinned = false;

    while (!settled() && spins++ < kMaxSpins) {
        runLoop.runOnce();
        if (!observer.failure.empty()) {
            break;
        }
        // Transitions are evaluated against the wall clock, so an in-flight one makes the
        // dump depend on when it was taken: the style light's direction and the fill colors
        // differed in their low bits between runs. Zero duration settles every transitioned
        // property at its target immediately, which is the state an oracle should compare.
        //
        // This has to happen after the style loads, not before. Loading a style replaces the
        // transition options with the style's own, so setting them earlier is silently undone
        // -- which is exactly what happened on the first attempt.
        if (wantDump && !transitionsPinned && observer.styleLoaded) {
            map.getStyle().setTransitionOptions(style::TransitionOptions{Duration::zero(), Duration::zero()});
            transitionsPinned = true;
            loadedAt = std::chrono::steady_clock::now();
            stableFrames = 0;
        }
        if (frontend.renderFrameIfDirty()) {
            ++framesRendered;
            const auto& s = sink.getStats();
            if (s.drawsOrdered > 0) {
                ++framesWithContent;
            }
            if (s.drawsOrdered > 0 && s.liveDrawables == lastLive && s.drawsOrdered == lastDraws) {
                ++stableFrames;
            } else {
                stableFrames = 0;
            }
            lastLive = s.liveDrawables;
            lastDraws = s.drawsOrdered;
            // Keep the map busy so tiles keep arriving and drawables keep being rebuilt.
            map.triggerRepaint();
        }
    }

    const auto& stats = sink.getStats();

    if (wantVertices && observer.failure.empty()) {
        dumpSink.dumpVertices(stdout);
    }

    if (wantDump && observer.failure.empty()) {
        std::FILE* out = (dumpPath == "-") ? stdout : std::fopen(dumpPath.c_str(), "w");
        if (!out) {
            std::fprintf(stderr, "probe: cannot open %s for writing\n", dumpPath.c_str());
            return 1;
        }
        dumpSink.dump(out);
        if (out != stdout) {
            std::fclose(out);
            Log::Info(Event::General, "probe: wrote dump to " + dumpPath);
        }
    }

    std::printf("\n=== capture probe result ===\n");
    if (!observer.failure.empty()) {
        std::printf("FAILED: %s\n", observer.failure.c_str());
        return 1;
    }
    std::printf("style loaded      : %s\n", observer.styleLoaded ? "yes" : "no");
    std::printf("frames rendered   : %d (%d with content, loop spins %d)\n", framesRendered, framesWithContent, spins);
    std::printf(
        "drawable adds     : %llu (created %llu, idx-replaced %llu, attrs-replaced %llu, attrs-modified %llu)\n",
        static_cast<unsigned long long>(stats.drawableAdds),
        static_cast<unsigned long long>(stats.addsCreated),
        static_cast<unsigned long long>(stats.addsIndexDataReplaced),
        static_cast<unsigned long long>(stats.addsAttributesReplaced),
        static_cast<unsigned long long>(stats.addsAttributesModified));
    std::printf("drawable removes  : %llu\n", static_cast<unsigned long long>(stats.drawableRemoves));
    std::printf("live drawables    : %llu\n", static_cast<unsigned long long>(stats.liveDrawables));
    std::printf("ubo updates       : %llu\n", static_cast<unsigned long long>(stats.uboUpdates));
    std::printf("texture updates   : %llu\n", static_cast<unsigned long long>(stats.textureUpdates));
    std::printf("stencil tile sets : %llu\n", static_cast<unsigned long long>(stats.stencilTileSets));
    std::printf("draws in last order: %llu\n", static_cast<unsigned long long>(stats.drawsOrdered));

    const bool ok = observer.styleLoaded && framesWithContent >= kFrames && stats.drawableAdds > 0 &&
                    stats.drawsOrdered > 0;
    std::printf(
        "\n%s\n",
        ok ? "PASS: frontend ran headless with zero GPU and produced a diff stream" : "FAIL: no drawables captured");
    return ok ? 0 : 1;
}
