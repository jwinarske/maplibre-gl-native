#pragma once

#include <mbgl/gfx/renderable.hpp>
#include <mbgl/util/size.hpp>

#include <memory>

namespace mln {
namespace capture {

/// Nothing to bind: the "surface" exists only so `PaintParameters` has a renderable to
/// describe and so `backend.getDefaultRenderable().getSize()` reports the logical viewport.
class RenderableResource final : public gfx::RenderableResource {
public:
    void bind() override {}
};

class Renderable final : public gfx::Renderable {
public:
    explicit Renderable(Size size_)
        : gfx::Renderable(size_, std::make_unique<RenderableResource>()) {}
    ~Renderable() override = default;

    void setSize(Size size_) { size = size_; }
};

} // namespace capture
} // namespace mln
