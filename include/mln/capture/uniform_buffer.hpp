#pragma once

#include <mln/gfx/uniform_buffer.hpp>

#include <cstdint>
#include <vector>

namespace mln {
namespace capture {

/// Retains the bytes a tweaker wrote, plus a version counter so the frame sink can emit
/// dirty-only `UboUpdate`s. Tweakers rewrite every drawable UBO every frame
/// (renderer_impl.cpp:279-287), so this comparison is what keeps the diff small.
class UniformBuffer final : public gfx::UniformBuffer {
public:
    UniformBuffer(const void* data, std::size_t size_);
    /// Copy is required by UniformBufferArray::copy(); there is deliberately no move
    /// constructor, because moving the base while still reading `other`'s members is a
    /// use-after-move and copy-constructing the base inside a move constructor is a
    /// pessimisation -- and nothing needs one.
    UniformBuffer(const UniformBuffer&);
    ~UniformBuffer() override = default;

    void update(const void* data, std::size_t dataSize) override;

    const std::vector<std::uint8_t>& getContents() const noexcept { return contents; }
    std::uint64_t getVersion() const noexcept { return version; }

    /// True when the contents changed since the last `clearDirty()`.
    bool isDirty() const noexcept { return dirty; }
    void clearDirty() noexcept { dirty = false; }

private:
    std::vector<std::uint8_t> contents;
    std::uint64_t version = 0;
    bool dirty = true;
};

class UniformBufferArray final : public gfx::UniformBufferArray {
public:
    UniformBufferArray() = default;
    UniformBufferArray(UniformBufferArray&& other) noexcept
        : gfx::UniformBufferArray(std::move(other)) {}
    ~UniformBufferArray() override = default;

    UniformBufferArray& operator=(UniformBufferArray&& other) noexcept {
        gfx::UniformBufferArray::operator=(std::move(other));
        return *this;
    }

    /// No GPU binding to do — the consumer reads the retained bytes instead.
    void bind(gfx::RenderPass&) override {}

protected:
    std::unique_ptr<gfx::UniformBuffer> copy(const gfx::UniformBuffer& buffer) override;
};

} // namespace capture
} // namespace mln
