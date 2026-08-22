#pragma once

#include <mbgl/gfx/drawable_builder.hpp>

namespace mln {
namespace capture {

class Context;

/// Reuses the generic builder machinery in src/mbgl/gfx/drawable_builder_impl.cpp; only the
/// three hooks that name a concrete drawable type are ours.
class DrawableBuilder final : public gfx::DrawableBuilder {
public:
    DrawableBuilder(Context& context_, std::string name_)
        : gfx::DrawableBuilder(std::move(name_)),
          context(context_) {}
    ~DrawableBuilder() override = default;

    // createSegment is public in gfx::DrawableBuilder; keep the visibility it declares.
    std::unique_ptr<gfx::Drawable::DrawSegment> createSegment(gfx::DrawMode, SegmentBase&&) override;

protected:
    gfx::UniqueDrawable createDrawable() const override;
    void init() override;

private:
    Context& context;
};

} // namespace capture
} // namespace mln
