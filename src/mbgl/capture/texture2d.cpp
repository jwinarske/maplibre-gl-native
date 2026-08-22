#include <mbgl/capture/texture2d.hpp>

#include <mbgl/capture/context.hpp>

#include <mbgl/util/logging.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace mln {
namespace capture {

namespace {

/// FNV-1a. Cheap and good enough to let a consumer share one GPU texture between map
/// instances that produced byte-identical atlases (plan §3.6).
std::uint64_t contentHash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

gfx::Texture2D& Texture2D::setSamplerConfiguration(const SamplerState& state) noexcept {
    samplerState = state;
    return *this;
}

gfx::Texture2D& Texture2D::setFormat(gfx::TexturePixelType pixelFormat_,
                                     gfx::TextureChannelDataType channelType_) noexcept {
    if (pixelFormat_ == pixelFormat && channelType_ == channelType) {
        return *this;
    }
    pixelFormat = pixelFormat_;
    channelType = channelType_;
    created = false;
    return *this;
}

gfx::Texture2D& Texture2D::setSize(Size size_) noexcept {
    if (size_ == size) {
        return *this;
    }
    size = size_;
    created = false;
    return *this;
}

gfx::Texture2D& Texture2D::setImage(std::shared_ptr<PremultipliedImage> image_) noexcept {
    imageData = std::move(image_);
    return *this;
}

size_t Texture2D::getPixelStride() const noexcept {
    switch (channelType) {
        case gfx::TextureChannelDataType::UnsignedByte:
            return 1 * numChannels();
        case gfx::TextureChannelDataType::HalfFloat:
            return 2 * numChannels();
        default:
            return 0;
    }
}

size_t Texture2D::numChannels() const noexcept {
    switch (pixelFormat) {
        case gfx::TexturePixelType::RGBA:
            return 4;
        case gfx::TexturePixelType::Alpha:
            return 1;
        default:
            return 0;
    }
}

size_t Texture2D::getDataSize() const noexcept {
    // Widen before multiplying: Size::width/height are uint32_t, so `width * height * stride`
    // is evaluated in 32-bit and wraps for large textures, producing an undersized allocation.
    return static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * getPixelStride();
}

void Texture2D::allocate() {
    const auto bytes = getDataSize();
    if (pixels.size() != bytes) {
        pixels.assign(bytes, 0);
    }
    created = true;
}

void Texture2D::create() {
    if (!created) {
        allocate();
    }
}

void Texture2D::markDirty(const Rect<uint16_t>& rect) {
    if (context && *context) {
        (*context)->registerDirtyTexture(*this);
    }

    if (!dirtyRect) {
        dirtyRect = rect;
        return;
    }

    // Union, so a burst of atlas sub-uploads collapses into one envelope per frame.
    // Compute the far edges in 32 bits: `x + w` truncated back to uint16 wraps to 0 at 65536,
    // which would silently shrink the union and drop part of the update.
    const uint32_t x0 = std::min<uint32_t>(dirtyRect->x, rect.x);
    const uint32_t y0 = std::min<uint32_t>(dirtyRect->y, rect.y);
    const uint32_t x1 = std::max<uint32_t>(static_cast<uint32_t>(dirtyRect->x) + dirtyRect->w,
                                           static_cast<uint32_t>(rect.x) + rect.w);
    const uint32_t y1 = std::max<uint32_t>(static_cast<uint32_t>(dirtyRect->y) + dirtyRect->h,
                                           static_cast<uint32_t>(rect.y) + rect.h);

    constexpr uint32_t maxExtent = std::numeric_limits<uint16_t>::max();
    dirtyRect = Rect<uint16_t>{static_cast<uint16_t>(x0),
                               static_cast<uint16_t>(y0),
                               static_cast<uint16_t>(std::min(x1 - x0, maxExtent)),
                               static_cast<uint16_t>(std::min(y1 - y0, maxExtent))};
}

void Texture2D::upload(const void* pixelData, const Size& size_) {
    setSize(size_);
    allocate();
    if (pixelData && !pixels.empty()) {
        std::memcpy(pixels.data(), pixelData, std::min(pixels.size(), getDataSize()));
    }
    markDirty(Rect<uint16_t>{0, 0, static_cast<uint16_t>(size.width), static_cast<uint16_t>(size.height)});
}

void Texture2D::uploadSubRegion(const void* pixelData, const Size& size_, uint16_t xOffset, uint16_t yOffset) {
    if (!created) {
        allocate();
    }
    const auto stride = getPixelStride();
    if (!pixelData || !stride || !size.width) {
        return;
    }

    // Reject a region that does not fit rather than letting rows wrap into their neighbours.
    // The destination check below keeps us in bounds either way, but a wrapped row is silent
    // corruption of an atlas whose contents came off the network as sprite/glyph images.
    if (static_cast<std::size_t>(xOffset) + size_.width > size.width ||
        static_cast<std::size_t>(yOffset) + size_.height > size.height) {
        Log::Error(Event::General, "capture: texture sub-region upload out of bounds, ignoring");
        assert(false);
        return;
    }

    const auto* src = static_cast<const std::uint8_t*>(pixelData);
    const auto rowBytes = static_cast<std::size_t>(size_.width) * stride;
    for (uint32_t row = 0; row < size_.height; ++row) {
        const auto dstOffset = (static_cast<std::size_t>(yOffset + row) * size.width + xOffset) * stride;
        if (dstOffset + rowBytes > pixels.size()) {
            break;
        }
        std::memcpy(pixels.data() + dstOffset, src + row * rowBytes, rowBytes);
    }

    markDirty(
        Rect<uint16_t>{xOffset, yOffset, static_cast<uint16_t>(size_.width), static_cast<uint16_t>(size_.height)});
}

void Texture2D::upload() {
    if (!imageData) {
        return;
    }
    upload(imageData->data.get(), imageData->size);
    imageData.reset();
}

Texture2D::~Texture2D() {
    if (context && *context) {
        (*context)->unregisterTexture(*this);
    }
}

void Texture2D::flush() {
    if (!dirtyRect) {
        return;
    }
    if (!context || !*context) {
        dirtyRect.reset();
        return;
    }
    (*context)->recordTextureUpdate(TextureUpdate{.id = uniqueID,
                                                  .size = size,
                                                  .format = pixelFormat,
                                                  .contentHash = contentHash(pixels),
                                                  .dirtyRect = dirtyRect,
                                                  .pixels = pixels.data(),
                                                  .pixelBytes = pixels.size()});
    dirtyRect.reset();
}

} // namespace capture
} // namespace mln
