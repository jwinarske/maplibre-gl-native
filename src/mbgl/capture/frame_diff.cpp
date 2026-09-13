#include <mbgl/capture/frame_diff.hpp>

#include <mbgl/gfx/index_vector.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/string.hpp>

#include <sstream>

namespace mln {
namespace capture {

namespace {

const char* attributeTypeName(gfx::AttributeDataType t) {
    switch (t) {
        case gfx::AttributeDataType::Byte:
            return "Byte";
        case gfx::AttributeDataType::Byte2:
            return "Byte2";
        case gfx::AttributeDataType::Byte3:
            return "Byte3";
        case gfx::AttributeDataType::Byte4:
            return "Byte4";
        case gfx::AttributeDataType::UByte:
            return "UByte";
        case gfx::AttributeDataType::UByte2:
            return "UByte2";
        case gfx::AttributeDataType::UByte3:
            return "UByte3";
        case gfx::AttributeDataType::UByte4:
            return "UByte4";
        case gfx::AttributeDataType::Short:
            return "Short";
        case gfx::AttributeDataType::Short2:
            return "Short2";
        case gfx::AttributeDataType::Short3:
            return "Short3";
        case gfx::AttributeDataType::Short4:
            return "Short4";
        case gfx::AttributeDataType::UShort:
            return "UShort";
        case gfx::AttributeDataType::UShort2:
            return "UShort2";
        case gfx::AttributeDataType::UShort3:
            return "UShort3";
        case gfx::AttributeDataType::UShort4:
            return "UShort4";
        case gfx::AttributeDataType::Int:
            return "Int";
        case gfx::AttributeDataType::Int2:
            return "Int2";
        case gfx::AttributeDataType::Int3:
            return "Int3";
        case gfx::AttributeDataType::Int4:
            return "Int4";
        case gfx::AttributeDataType::UInt:
            return "UInt";
        case gfx::AttributeDataType::UInt2:
            return "UInt2";
        case gfx::AttributeDataType::UInt3:
            return "UInt3";
        case gfx::AttributeDataType::UInt4:
            return "UInt4";
        case gfx::AttributeDataType::Float:
            return "Float";
        case gfx::AttributeDataType::Float2:
            return "Float2";
        case gfx::AttributeDataType::Float3:
            return "Float3";
        case gfx::AttributeDataType::Float4:
            return "Float4";
        default:
            return "Invalid";
    }
}

} // namespace

void LogFrameSink::beginFrame(MapID mapId, std::uint64_t frameNo) {
    if (verbose) {
        Log::Info(Event::Render, "capture: map " + util::toString(mapId) + " frame " + util::toString(frameNo) + " >>");
    }
}

void LogFrameSink::endFrame(MapID mapId, std::uint64_t frameNo) {
    ++stats.frames;
    std::ostringstream ss;
    ss << "capture: map " << mapId << " frame " << frameNo << " << drawables=" << stats.liveDrawables << " (+"
       << stats.drawableAdds << "/-" << stats.drawableRemoves << " cumulative)"
       << " ubo=" << stats.uboUpdates << " tex=" << stats.textureUpdates << " stencilSets=" << stats.stencilTileSets
       << " draws=" << stats.drawsOrdered;
    Log::Info(Event::Render, ss.str());
}

void LogFrameSink::onDrawableAdd(const DrawableAdd& add) {
    ++stats.drawableAdds;
    switch (add.reason) {
        case AddReason::Created:
            ++stats.addsCreated;
            ++stats.liveDrawables;
            break;
        case AddReason::IndexDataReplaced:
            ++stats.addsIndexDataReplaced;
            break;
        case AddReason::AttributesReplaced:
            ++stats.addsAttributesReplaced;
            break;
        case AddReason::AttributesModified:
            ++stats.addsAttributesModified;
            break;
    }
    if (!verbose) {
        return;
    }

    std::ostringstream ss;
    ss << "  +drawable " << add.id.id() << " '" << add.name << "'"
       << " shader=" << static_cast<int>(add.builtinShader) << "#" << std::hex << add.permutationKey << std::dec;
    if (add.tileID) {
        ss << " tile=" << util::toString(*add.tileID);
    }
    ss << " verts=" << add.vertexCount << " idx=" << (add.indexes ? add.indexes->elements() : 0)
       << " segs=" << add.segments.size() << " pass=" << static_cast<int>(add.renderPass) << (add.is3D ? " 3D" : "")
       << (add.enableStencil ? " stencil" : "");
    Log::Info(Event::Render, ss.str());

    for (const auto& a : add.attrs) {
        std::ostringstream as;
        as << "      attr id=" << a.attrId << " slot=" << a.index << " type=" << attributeTypeName(a.dataType);
        if (a.sharedVector) {
            as << " shared(off=" << a.offset << " voff=" << a.vertexOffset << " stride=" << a.stride << ")";
        } else {
            as << " inline(count=" << a.rawCount << ")";
        }
        Log::Info(Event::Render, as.str());
    }
}

void LogFrameSink::onDrawableRemove(const DrawableRemove&) {
    ++stats.drawableRemoves;
    if (stats.liveDrawables) {
        --stats.liveDrawables;
    }
}

void LogFrameSink::onUboUpdate(const UboUpdate&) {
    ++stats.uboUpdates;
}

void LogFrameSink::onTextureUpdate(const TextureUpdate& tex) {
    ++stats.textureUpdates;
    if (!verbose) {
        return;
    }
    std::ostringstream ss;
    ss << "  ~texture " << tex.id.id() << " " << tex.size.width << "x" << tex.size.height << " hash=" << std::hex
       << tex.contentHash << std::dec;
    if (tex.dirtyRect) {
        ss << " rect=(" << tex.dirtyRect->x << "," << tex.dirtyRect->y << " " << tex.dirtyRect->w << "x"
           << tex.dirtyRect->h << ")";
    }
    Log::Info(Event::Render, ss.str());
}

void LogFrameSink::onRenderTargetCreate(const RenderTargetCreate& rt) {
    ++stats.renderTargets;
    if (!verbose) {
        return;
    }
    std::ostringstream ss;
    ss << "  +rendertarget " << rt.textureId.id() << " " << rt.size.width << "x" << rt.size.height
       << " ct=" << static_cast<int>(rt.channelType);
    Log::Info(Event::Render, ss.str());
}

void LogFrameSink::onStencilTiles(const StencilTiles& st) {
    ++stats.stencilTileSets;
    if (verbose) {
        Log::Info(
            Event::Render,
            "  stencilTiles layer=" + util::toString(st.layerIndex) + " count=" + util::toString(st.tiles.size()));
    }
}

void LogFrameSink::onFrameOrder(const FrameOrder& order) {
    stats.drawsOrdered = order.ordered.size();
}

} // namespace capture
} // namespace mln
