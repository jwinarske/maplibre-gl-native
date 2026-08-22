#pragma once

#include <mbgl/gfx/dynamic_texture.hpp>

namespace mln {
namespace capture {

class Context;

/// Atlas-backed texture for the capture backend.
///
/// `gfx::DynamicTexture::uploadImage` is a no-op that discards its pixel data -- every real
/// backend overrides it, and a backend that does not silently keeps an atlas full of zeroes.
/// For symbols that is invisible until the very end of the pipeline: the glyph quads are
/// built, placed and drawn correctly against an atlas that never received a glyph, so nothing
/// appears and nothing reports an error.
///
/// The capture Texture2D already keeps its pixels on the CPU and emits one envelope per frame
/// from its dirty rect, so this only has to write the sub-region and let that machinery run.
class DynamicTexture : public gfx::DynamicTexture {
public:
    DynamicTexture(Context& context, Size size, gfx::TexturePixelType pixelType);

    void uploadImage(const uint8_t* pixelData, gfx::TextureHandle& texHandle) override;
};

} // namespace capture
} // namespace mln
