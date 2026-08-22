#include <mbgl/capture/drawable_builder.hpp>

#include <mbgl/capture/drawable.hpp>
#include <mbgl/capture/context.hpp>
#include <mbgl/gfx/drawable_builder_impl.hpp>
#include <mbgl/gfx/drawable_impl.hpp>

#include <cstring>

namespace mln {
namespace capture {

gfx::UniqueDrawable DrawableBuilder::createDrawable() const {
    return std::make_unique<Drawable>(context.ref(), drawableName.empty() ? name : drawableName);
}

std::unique_ptr<gfx::Drawable::DrawSegment> DrawableBuilder::createSegment(gfx::DrawMode mode, SegmentBase&& seg) {
    return std::make_unique<gfx::Drawable::DrawSegment>(mode, std::move(seg));
}

void DrawableBuilder::init() {
    auto& drawable = static_cast<Drawable&>(*currentDrawable);

    if (impl->rawVerticesCount) {
        auto raw = impl->rawVertices;
        drawable.setVertices(std::move(raw), impl->rawVerticesCount, impl->rawVerticesType);
    } else {
        const auto& verts = impl->vertices.vector();
        constexpr auto vertSize = sizeof(std::remove_reference<decltype(verts)>::type::value_type);
        std::vector<uint8_t> raw(verts.size() * vertSize);
        std::memcpy(raw.data(), verts.data(), raw.size());
        drawable.setVertices(std::move(raw), verts.size(), gfx::AttributeDataType::Short2);
    }

    if (!impl->sharedIndexes && !impl->buildIndexes.empty()) {
        impl->sharedIndexes = std::make_shared<gfx::IndexVectorBase>(std::move(impl->buildIndexes));
    }
    drawable.setIndexData(std::move(impl->sharedIndexes), std::move(impl->segments));

    impl->clear();
    textures.fill(nullptr);
}

} // namespace capture
} // namespace mln
