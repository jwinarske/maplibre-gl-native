#pragma once

#include <mbgl/capture/renderable.hpp>
#include <mbgl/capture/texture2d.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>

#include <memory>

namespace mln {
namespace capture {

class Context;

/// The render target behind a heatmap's first pass, as far as a capture needs one.
///
/// A real backend allocates a framebuffer here and the layer's drawables rasterize into it.
/// This backend rasterizes nothing — it records the stream — so what has to exist is the
/// *shape*: a renderable of the right size for `PaintParameters` to scissor against, and a
/// texture of the right size and channel type for the second pass to bind. Both are real
/// objects with real sizes; only the pixels are absent.
///
/// `readStillImage` is the one thing that cannot be answered. It is reachable from the
/// snapshotter and from nothing on the heatmap path, so it returns an empty image rather than
/// a plausible-looking black one — a caller that gets pixels back should not have to guess
/// whether they were rendered.
class OffscreenTexture final : public gfx::OffscreenTexture {
public:
    OffscreenTexture(Context&, Size, gfx::TextureChannelDataType);
    ~OffscreenTexture() override = default;

    bool isRenderable() override { return true; }
    PremultipliedImage readStillImage() override { return {}; }
    const gfx::Texture2DPtr& getTexture() override { return texture; }

private:
    gfx::Texture2DPtr texture;
};

} // namespace capture
} // namespace mln
