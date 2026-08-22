#pragma once

#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/util/identity.hpp>
#include <mbgl/util/rect.hpp>
#include <mbgl/util/size.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace mln {
namespace capture {

class Context;
using ContextRef = std::shared_ptr<Context*>;

/// Retains pixels and sampler state, and tracks the dirty sub-region so the glyph/icon
/// `DynamicTextureAtlas` (gfx/dynamic_texture_atlas.cpp) emits incremental updates rather
/// than whole-atlas re-uploads.
class Texture2D final : public gfx::Texture2D {
public:
    explicit Texture2D(ContextRef context_)
        : context(std::move(context_)) {}
    ~Texture2D() override;

    const util::SimpleIdentity& getID() const noexcept { return uniqueID; }

    gfx::Texture2D& setSamplerConfiguration(const SamplerState&) noexcept override;
    gfx::Texture2D& setFormat(gfx::TexturePixelType, gfx::TextureChannelDataType) noexcept override;
    gfx::Texture2D& setSize(Size) noexcept override;
    gfx::Texture2D& setImage(std::shared_ptr<PremultipliedImage>) noexcept override;

    gfx::TexturePixelType getFormat() const noexcept override { return pixelFormat; }
    Size getSize() const noexcept override { return size; }
    size_t getDataSize() const noexcept override;
    size_t getPixelStride() const noexcept override;
    size_t numChannels() const noexcept override;

    void create() override;
    void upload() override;
    void upload(const void* pixelData, const Size& size_) override;
    void uploadSubRegion(const void* pixelData, const Size& size_, uint16_t xOffset, uint16_t yOffset) override;
    bool needsUpload() const noexcept override { return !!imageData; }

    const std::vector<std::uint8_t>& getPixels() const noexcept { return pixels; }
    const SamplerState& getSamplerState() const noexcept { return samplerState; }

    /// Region touched since the last `flush()`, or nullopt if nothing changed.
    const std::optional<Rect<uint16_t>>& getDirtyRect() const noexcept { return dirtyRect; }

    /// Emit the accumulated dirty region to the frame sink and clear it. Called once per frame
    /// by Context::endFrame, not on every upload.
    void flush();

private:
    void markDirty(const Rect<uint16_t>&);
    void allocate();

    ContextRef context;
    const util::SimpleIdentity uniqueID;

    Size size{0, 0};
    gfx::TexturePixelType pixelFormat{gfx::TexturePixelType::RGBA};
    gfx::TextureChannelDataType channelType{gfx::TextureChannelDataType::UnsignedByte};
    SamplerState samplerState{};

    std::vector<std::uint8_t> pixels;
    std::shared_ptr<PremultipliedImage> imageData;
    std::optional<Rect<uint16_t>> dirtyRect;
    bool created = false;
};

} // namespace capture
} // namespace mln
