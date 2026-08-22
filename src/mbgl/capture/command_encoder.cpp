#include "command_encoder.hpp"

#include <mbgl/capture/context.hpp>
#include <mbgl/gfx/vertex_attribute.hpp>

namespace mln {
namespace capture {

std::unique_ptr<gfx::UploadPass> CommandEncoder::createUploadPass(const char*, gfx::Renderable&) {
    return std::make_unique<UploadPass>(context);
}

std::unique_ptr<gfx::RenderPass> CommandEncoder::createRenderPass(const char*,
                                                                  const gfx::RenderPassDescriptor& descriptor) {
    return std::make_unique<RenderPass>(context, descriptor);
}

void CommandEncoder::present(gfx::Renderable&) {}

gfx::Context& UploadPass::getContext() {
    return context;
}

const gfx::Context& UploadPass::getContext() const {
    return context;
}

std::unique_ptr<gfx::VertexBufferResource> UploadPass::createVertexBufferResource(const void*,
                                                                                  std::size_t size,
                                                                                  gfx::BufferUsageType,
                                                                                  bool) {
    return std::make_unique<VertexBufferResource>(size);
}

void UploadPass::updateVertexBufferResource(gfx::VertexBufferResource&, const void*, std::size_t) {}

std::unique_ptr<gfx::IndexBufferResource> UploadPass::createIndexBufferResource(const void*,
                                                                                std::size_t size,
                                                                                gfx::BufferUsageType,
                                                                                bool) {
    return std::make_unique<IndexBufferResource>(size);
}

void UploadPass::updateIndexBufferResource(gfx::IndexBufferResource&, const void*, std::size_t) {}

gfx::AttributeBindingArray UploadPass::buildAttributeBindings(
    std::size_t /*vertexCount*/,
    gfx::AttributeDataType /*vertexType*/,
    std::size_t /*vertexAttributeIndex*/,
    const std::vector<std::uint8_t>& /*vertexData*/,
    const gfx::VertexAttributeArray& /*defaults*/,
    const gfx::VertexAttributeArray& /*overrides*/,
    gfx::BufferUsageType,
    const std::optional<std::chrono::duration<double>> /*lastUpdate*/,
    std::vector<std::unique_ptr<gfx::VertexBufferResource>>& /*outBuffers*/) {
    // The real backends resolve defaults against overrides here and build GPU bindings. We
    // don't: the capture Drawable forwards the attribute array itself, shared vectors and
    // all, so the consumer sees the unresolved descriptors and does its own layout.
    return {};
}

} // namespace capture
} // namespace mln
