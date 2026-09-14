#include <mln/capture/uniform_buffer.hpp>

#include <cstring>

namespace mln {
namespace capture {

UniformBuffer::UniformBuffer(const void* data, std::size_t size_)
    : gfx::UniformBuffer(size_),
      contents(size_) {
    if (data && size_) {
        std::memcpy(contents.data(), data, size_);
    }
}

UniformBuffer::UniformBuffer(const UniformBuffer& other)
    : gfx::UniformBuffer(other),
      contents(other.contents),
      version(other.version),
      dirty(other.dirty) {}

void UniformBuffer::update(const void* data, std::size_t dataSize) {
    assert(dataSize <= size);
    if (dataSize > size || !data) {
        return;
    }

    // Tweakers rewrite every drawable UBO every frame (renderer_impl.cpp:279-287). Compare
    // before storing so the frame sink only sees buffers that actually changed — under
    // Phase B, where the tile matrix is discarded, this collapses to near zero.
    if (contents.size() >= dataSize && std::memcmp(contents.data(), data, dataSize) == 0) {
        return;
    }

    if (contents.size() < dataSize) {
        contents.resize(dataSize);
    }
    std::memcpy(contents.data(), data, dataSize);
    ++version;
    dirty = true;
}

std::unique_ptr<gfx::UniformBuffer> UniformBufferArray::copy(const gfx::UniformBuffer& buffer) {
    return std::make_unique<UniformBuffer>(static_cast<const UniformBuffer&>(buffer));
}

} // namespace capture
} // namespace mln
