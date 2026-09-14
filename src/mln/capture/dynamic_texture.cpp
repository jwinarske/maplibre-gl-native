#include <mln/capture/dynamic_texture.hpp>

#include <mln/capture/context.hpp>
#include <mln/capture/texture2d.hpp>

namespace mln {
namespace capture {

DynamicTexture::DynamicTexture(Context& context_, Size size_, gfx::TexturePixelType pixelType_)
    : gfx::DynamicTexture(context_, size_, pixelType_) {}

void DynamicTexture::uploadImage(const uint8_t* pixelData, gfx::TextureHandle& texHandle) {
    std::scoped_lock lock(mutex);
    if (pixelData && texture) {
        const auto& rect = texHandle.getRectangle();
        // The atlas is created lazily by the packer; uploadSubRegion allocates on first use.
        texture->uploadSubRegion(pixelData, Size(rect.w, rect.h), rect.x, rect.y);
    }
    gfx::DynamicTexture::uploadImage(pixelData, texHandle);
}

} // namespace capture
} // namespace mln
