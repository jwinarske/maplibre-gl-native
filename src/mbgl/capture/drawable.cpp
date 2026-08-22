#include <mbgl/capture/drawable.hpp>

#include <mbgl/capture/context.hpp>
#include <mbgl/capture/shader_program.hpp>
#include <mbgl/capture/texture2d.hpp>
#include <mbgl/gfx/drawable_impl.hpp>
#include <mbgl/gfx/index_vector.hpp>
#include <mbgl/gfx/vertex_attribute.hpp>
#include <mbgl/gfx/vertex_vector.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/util/hash.hpp>
#include <mbgl/util/monotonic_timer.hpp>

#include <utility>

namespace mln {
namespace capture {

Drawable::Drawable(ContextRef context_, std::string name_)
    : gfx::Drawable(std::move(name_)),
      context(std::move(context_)) {}

Drawable::~Drawable() {
    // Drawables outlive the Context whenever the caller destroys the backend before the
    // Renderer, so this must tolerate a dead Context rather than write through a reference.
    if (context && *context) {
        (*context)->recordDrawableRemove(getID());
    }
}

void Drawable::setVertices(std::vector<uint8_t>&& data, std::size_t count, gfx::AttributeDataType type_) {
    // Two callers, two shapes. Most layers pass an empty vector -- count and type only --
    // and supply the geometry through the vertex attribute array as shared bucket vectors.
    // The background layer instead builds its quad on the CPU and passes the bytes here,
    // naming the attribute they feed via the builder's `vertexAttrId`
    // (render_background_layer.cpp:220-223). Both have to reach the consumer; assuming only
    // the first existed is why background arrived with no position at all.
    //
    // Kept as a shared block so the consumer can retain it without another copy; the real
    // backends instead stuff it into the vertex attribute array at `vertexAttrId`, which is
    // the same information reshaped.
    rawVertices = data.empty() ? nullptr : std::make_shared<std::vector<std::uint8_t>>(std::move(data));
    vertexCount = count;
    vertexType = type_;
}

void Drawable::setIndexData(gfx::IndexVectorBasePtr indexes_, std::vector<UniqueDrawSegment> segments_) {
    indexes = std::move(indexes_);
    segments = std::move(segments_);
}

void Drawable::updateVertexAttributes(gfx::VertexAttributeArrayPtr attrs,
                                      std::size_t vertexCount_,
                                      gfx::DrawMode,
                                      gfx::IndexVectorBasePtr indexes_,
                                      const SegmentBase* segments_,
                                      std::size_t segmentCount) {
    setVertexAttributes(std::move(attrs));
    vertexCount = vertexCount_;

    indexes = std::move(indexes_);

    segments.clear();
    segments.reserve(segmentCount);
    for (std::size_t i = 0; i < segmentCount; ++i) {
        const auto& seg = segments_[i];
        segments.push_back(std::make_unique<gfx::Drawable::DrawSegment>(
            gfx::Triangles(),
            SegmentBase(seg.vertexOffset, seg.indexOffset, seg.vertexLength, seg.indexLength, seg.sortKey)));
    }
}

void Drawable::setDepthModeFor3D(const gfx::DepthMode&) {}
void Drawable::setStencilModeFor3D(const gfx::StencilMode&) {}

std::uint64_t Drawable::contentSignature() const {
    std::size_t seed = 0;
    util::hash_combine(seed, reinterpret_cast<std::uintptr_t>(indexes.get()));
    util::hash_combine(seed, vertexCount);
    util::hash_combine(seed, static_cast<int>(vertexType));
    util::hash_combine(seed, reinterpret_cast<std::uintptr_t>(rawVertices.get()));
    util::hash_combine(seed, reinterpret_cast<std::uintptr_t>(getShader().get()));
    util::hash_combine(seed, segments.size());
    for (const auto& seg : segments) {
        const auto& s = seg->getSegment();
        util::hash_combine(seed, s.vertexOffset);
        util::hash_combine(seed, s.indexOffset);
        util::hash_combine(seed, s.vertexLength);
        util::hash_combine(seed, s.indexLength);
    }

    const auto hashAttrs = [&seed](const gfx::VertexAttributeArrayPtr& array) {
        if (!array) {
            return;
        }
        for (std::size_t id = 0; id < shaders::maxVertexAttributeCountPerShader; ++id) {
            const auto& slot = array->get(id);
            if (!slot) {
                continue;
            }
            util::hash_combine(seed, id);
            util::hash_combine(seed, slot->getIndex());
            util::hash_combine(seed, static_cast<int>(slot->getDataType()));
            // Identity only -- deliberately NOT getLastModified(). That is a mutation stamp,
            // and the layers rewrite data-driven paint values in place on every frame, so
            // folding it in would re-announce every drawable every frame and undo the whole
            // point of the signature. Residual risk accepted: a freed VertexVectorBase can be
            // replaced at the same address, but aliasing would additionally require identical
            // element size, offset, stride, type, vertex count, index vector and segment
            // layout -- at which point the stale descriptor still describes the geometry.
            if (const auto& shared = slot->getSharedRawData()) {
                util::hash_combine(seed, reinterpret_cast<std::uintptr_t>(shared.get()));
                util::hash_combine(seed, shared->getRawSize());
            }
            util::hash_combine(seed, slot->getSharedOffset());
            util::hash_combine(seed, slot->getSharedVertexOffset());
            util::hash_combine(seed, slot->getSharedStride());
            util::hash_combine(seed, slot->getCount());
        }
    };
    hashAttrs(getVertexAttributes());
    hashAttrs(getInstanceAttributes());

    return static_cast<std::uint64_t>(seed);
}

void Drawable::upload(gfx::UploadPass&) {
    // Re-announcement is decided by content, deliberately NOT by whether a setter ran.
    //
    // `RenderFillLayer::update` calls `updateVertexAttributes()` unconditionally on every
    // frame for the mainline Fill/FillOutline/FillPattern variants (render_fill_layer.cpp:
    // 279-297) -- only FillOutlineTriangulated consults `getAttributeUpdateTime()`. Line and
    // circle behave the same way. Treating those calls as "the geometry changed" would emit a
    // full DrawableAdd per drawable per frame and destroy the premise that geometry churn
    // tracks tile churn rather than frame rate.
    const auto signature = contentSignature();

    // In-place value rewrites only matter for attributes that carry their values inline.
    // Everything a real style produces is backed by a shared bucket vector -- we forward the
    // view, not the bytes -- so a "modified" flag on those tells the consumer nothing it does
    // not already have. Checking this is what keeps a static scene from re-announcing every
    // frame (the line layer rewrites its data-driven attributes on every update).
    const bool hasInlineValues = [&] {
        const auto& array = getVertexAttributes();
        if (!array) {
            return false;
        }
        for (std::size_t id = 0; id < shaders::maxVertexAttributeCountPerShader; ++id) {
            const auto& slot = array->get(id);
            if (slot && !slot->getSharedRawData() && slot->getCount() > 0) {
                return true;
            }
        }
        return false;
    }();

    const bool valuesModified = announced && hasInlineValues && attributeUpdateTime &&
                                getVertexAttributes()->isModifiedAfter(*attributeUpdateTime);

    if (!announced) {
        emitAdd(AddReason::Created);
    } else if (signature != announcedSignature) {
        emitAdd(AddReason::AttributesReplaced);
    } else if (valuesModified) {
        // Same buffers, but a data-driven paint value was rewritten in place.
        emitAdd(AddReason::AttributesModified);
    } else {
        attributeUpdateTime = util::MonotonicTimer::now();
        return;
    }

    announced = true;
    announcedSignature = signature;

    // Stamp the upload time as the real backends do (vulkan/drawable.cpp:283); the layers
    // that *do* have an incremental guard rely on it.
    attributeUpdateTime = util::MonotonicTimer::now();
}

void Drawable::emitAdd(AddReason reason) const {
    DrawableAdd add;
    add.reason = reason;
    add.id = getID();
    add.name = getName();
    add.tileID = getTileID();
    add.vertexCount = vertexCount;
    add.vertexType = vertexType;
    add.indexes = indexes;
    add.is3D = getIs3D();
    add.enableStencil = getEnableStencil();
    add.enableDepth = getEnableDepth();
    add.enableColor = getEnableColor();
    add.renderPass = static_cast<std::uint8_t>(mln::underlying_type(getRenderPass()));
    add.subLayerIndex = getSubLayerIndex();
    add.layerIndex = owningLayerIndex;

    if (const auto& shaderBase = getShader()) {
        if (const auto* captured = static_cast<const ShaderProgram*>(shaderBase.get())) {
            add.builtinShader = captured->getShaderID();
            add.permutationKey = captured->getPermutationKey();
        }
    }

    add.segments.reserve(segments.size());
    for (const auto& seg : segments) {
        const auto& s = seg->getSegment();
        add.segments.push_back(SegmentDesc{.vertexOffset = s.vertexOffset,
                                           .indexOffset = s.indexOffset,
                                           .vertexLength = s.vertexLength,
                                           .indexLength = s.indexLength});
    }

    // A drawable's attribute array holds *overrides*; the binding slot the shader declares
    // lives on the shader's own array (populated from the ShaderSource tables). Resolve it
    // here so the consumer gets a usable slot rather than the override's -1.
    const gfx::VertexAttributeArray* shaderAttrs = nullptr;
    const gfx::VertexAttributeArray* shaderInstanceAttrs = nullptr;
    if (const auto& shaderBase = getShader()) {
        shaderAttrs = &shaderBase->getVertexAttributes();
        shaderInstanceAttrs = &shaderBase->getInstanceAttributes();
    }

    const auto collect = [](const gfx::VertexAttributeArrayPtr& array,
                            const gfx::VertexAttributeArray* declared,
                            std::vector<AttributeDesc>& out) {
        if (!array) {
            return;
        }
        // visitAttributes() does not hand back the id, and the id is what names the attribute
        // on the shader side, so walk the fixed-size slot array instead.
        for (std::size_t id = 0; id < shaders::maxVertexAttributeCountPerShader; ++id) {
            const auto& slot = array->get(id);
            if (!slot) {
                continue;
            }
            const auto& attr = *slot;

            AttributeDesc desc;
            desc.attrId = id;
            desc.index = attr.getIndex();
            if (desc.index < 0 && declared) {
                if (const auto& declaredAttr = declared->get(id)) {
                    desc.index = declaredAttr->getIndex();
                }
            }
            desc.dataType = attr.getDataType();
            if (const auto& shared = attr.getSharedRawData()) {
                desc.sharedVector = shared;
                desc.dataType = attr.getSharedType();
                desc.offset = attr.getSharedOffset();
                desc.vertexOffset = attr.getSharedVertexOffset();
                desc.stride = attr.getSharedStride();
            } else {
                desc.rawCount = attr.getCount();
            }
            out.push_back(std::move(desc));
        }
    };
    // The raw-vertex path never reaches the attribute array on this backend, so surface it as
    // the attribute it feeds. Without this the background layer arrives with no position.
    //
    // Guarded against the array also holding that id: emitting two descriptors for one
    // attribute would have the consumer bind the same shader slot twice, which Filament
    // rejects. The real backends merge the two by writing the bytes into the array instead.
    const bool rawIdAlreadyPresent = getVertexAttributes() && getVertexAttributes()->get(vertexAttrId) != nullptr;
    if (rawVertices && !rawVertices->empty() && !rawIdAlreadyPresent) {
        AttributeDesc desc;
        desc.attrId = vertexAttrId;
        desc.dataType = vertexType;
        desc.rawCount = vertexCount;
        desc.rawData = rawVertices;
        if (shaderAttrs) {
            if (const auto& declaredAttr = shaderAttrs->get(vertexAttrId)) {
                desc.index = declaredAttr->getIndex();
            }
        }
        add.attrs.push_back(std::move(desc));
    }

    collect(getVertexAttributes(), shaderAttrs, add.attrs);
    collect(getInstanceAttributes(), shaderInstanceAttrs, add.instanceAttrs);

    for (std::size_t slot = 0; slot < textures.size(); ++slot) {
        if (const auto& tex = textures[slot]) {
            add.textureRefs.emplace_back(slot, static_cast<const Texture2D*>(tex.get())->getID());
        }
    }

    if (context && *context) {
        (*context)->recordDrawableAdd(std::move(add));
    }
}

void Drawable::draw(PaintParameters& parameters) const {
    if (!announced) {
        // Reached when a drawable is created and drawn without an intervening upload pass.
        emitAdd(AddReason::Created);
        announced = true;
        announcedSignature = contentSignature();
    }

    if (!context || !*context) {
        return;
    }
    auto& ctx = **context;

    ctx.recordDraw(DrawOrderEntry{.id = getID(),
                                  .pass = static_cast<std::uint8_t>(mln::underlying_type(parameters.pass)),
                                  .layerIndex = parameters.currentLayer,
                                  .subLayerIndex = getSubLayerIndex(),
                                  .drawPriority = getDrawPriority(),
                                  .uboIndex = getUBOIndex()});
    ctx.setFrameDepthInfo(parameters.opaquePassCutoff, parameters.depthRangeSize);

    // Emit whatever the tweakers just rewrote, dirty-only.
    const auto& ubos = uniformBuffers;
    for (std::size_t slot = 0; slot < ubos.allocatedSize(); ++slot) {
        const auto& buf = ubos.get(slot);
        if (!buf) {
            continue;
        }
        auto& captured = static_cast<UniformBuffer&>(*buf);
        if (!captured.isDirty()) {
            continue;
        }
        ctx.recordDrawableUboUpdate(getID(), slot, captured.getContents().data(), captured.getContents().size());
        captured.clearDirty();
    }
}

} // namespace capture
} // namespace mln
