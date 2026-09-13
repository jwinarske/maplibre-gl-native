#include <mbgl/capture/offscreen_texture.hpp>

#include <mbgl/capture/context.hpp>

namespace mln {
namespace capture {

OffscreenTexture::OffscreenTexture(Context& context, const Size size, const gfx::TextureChannelDataType channelType)
    : gfx::OffscreenTexture(size, std::make_unique<RenderableResource>()) {
    texture = context.createTexture2D();
    texture->setSize(size);
    texture->setFormat(gfx::TexturePixelType::RGBA, channelType);
}

} // namespace capture
} // namespace mln
