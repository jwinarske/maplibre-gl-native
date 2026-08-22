#pragma once

// Internal to mbgl-core: this header reaches src/mbgl/gfx/{render_pass,upload_pass}.hpp,
// which are not installed. Consumers of the capture backend only need
// <mbgl/capture/renderer_backend.hpp> and <mbgl/capture/frame_diff.hpp>.

#include <mbgl/capture/renderable.hpp>
#include <mbgl/gfx/command_encoder.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/gfx/upload_pass.hpp>

#include <memory>

namespace mln {
namespace capture {

class Context;

class RenderPass final : public gfx::RenderPass {
public:
    RenderPass(Context& context_, const gfx::RenderPassDescriptor& descriptor_)
        : context(context_),
          descriptor(descriptor_) {}
    ~RenderPass() override = default;

    Context& getContext() const noexcept { return context; }
    const gfx::RenderPassDescriptor& getDescriptor() const noexcept { return descriptor; }

protected:
    void pushDebugGroup(const char*) override {}
    void popDebugGroup() override {}
    void addDebugSignpost(const char*) override {}

private:
    Context& context;
    gfx::RenderPassDescriptor descriptor;
};

/// Buffer "resources" that hold nothing. mbgl still calls createVertexBufferResource for a
/// few paths; we hand back an inert object because the real geometry travels as shared
/// vertex vectors on the drawable, not through these.
class VertexBufferResource final : public gfx::VertexBufferResource {
public:
    explicit VertexBufferResource(std::size_t size_)
        : byteSize(size_) {}
    ~VertexBufferResource() override = default;

    std::size_t getSizeInBytes() const noexcept { return byteSize; }

private:
    std::size_t byteSize;
};

class IndexBufferResource final : public gfx::IndexBufferResource {
public:
    explicit IndexBufferResource(std::size_t size_)
        : byteSize(size_) {}
    ~IndexBufferResource() override = default;

    std::size_t getSizeInBytes() const noexcept { return byteSize; }

private:
    std::size_t byteSize;
};

class UploadPass final : public gfx::UploadPass {
public:
    explicit UploadPass(Context& context_)
        : context(context_) {}
    ~UploadPass() override = default;

    gfx::Context& getContext() override;
    const gfx::Context& getContext() const override;

    gfx::AttributeBindingArray buildAttributeBindings(
        std::size_t vertexCount,
        gfx::AttributeDataType vertexType,
        std::size_t vertexAttributeIndex,
        const std::vector<std::uint8_t>& vertexData,
        const gfx::VertexAttributeArray& defaults,
        const gfx::VertexAttributeArray& overrides,
        gfx::BufferUsageType,
        const std::optional<std::chrono::duration<double>> lastUpdate,
        /*out*/ std::vector<std::unique_ptr<gfx::VertexBufferResource>>& outBuffers) override;

    std::unique_ptr<gfx::IndexBufferResource> createIndexBufferResource(const void* data,
                                                                        std::size_t size,
                                                                        gfx::BufferUsageType,
                                                                        bool persistent) override;
    void updateIndexBufferResource(gfx::IndexBufferResource&, const void* data, std::size_t size) override;

protected:
    std::unique_ptr<gfx::VertexBufferResource> createVertexBufferResource(const void* data,
                                                                          std::size_t size,
                                                                          gfx::BufferUsageType,
                                                                          bool persistent) override;
    void updateVertexBufferResource(gfx::VertexBufferResource&, const void* data, std::size_t size) override;

    void pushDebugGroup(const char*) override {}
    void popDebugGroup() override {}

private:
    Context& context;
};

class CommandEncoder final : public gfx::CommandEncoder {
public:
    explicit CommandEncoder(Context& context_)
        : context(context_) {}
    ~CommandEncoder() override = default;

    std::unique_ptr<gfx::UploadPass> createUploadPass(const char* name, gfx::Renderable&) override;
    std::unique_ptr<gfx::RenderPass> createRenderPass(const char* name, const gfx::RenderPassDescriptor&) override;
    void present(gfx::Renderable&) override;

protected:
    void pushDebugGroup(const char*) override {}
    void popDebugGroup() override {}

private:
    Context& context;
};

} // namespace capture
} // namespace mln
