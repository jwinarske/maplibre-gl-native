#pragma once

#include <mbgl/capture/renderable.hpp>
#include <mbgl/capture/frame_diff.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/util/size.hpp>

#include <memory>

namespace mln {
namespace capture {

class Context;

/// A `gfx::RendererBackend` that owns no device, no surface and no swapchain.
///
/// Construct one, hand it to `mln::Renderer`, and the whole MapLibre frontend runs with the
/// renderer's output diverted to a `FrameSink` instead of a GPU. `Backend::Create<>` and the
/// `MLN_RENDER_BACKEND_*` instantiation machinery are never involved, because we construct
/// the Renderer ourselves.
class RendererBackend final : public gfx::RendererBackend {
public:
    /// @param size          Logical viewport, reported through getDefaultRenderable().
    /// @param sink          Consumer of the capture stream.
    /// @param mapId         Distinguishes this instance's drawable ids from other maps'
    ///                      feeding the same sink. See plan §3.2 / §3.6.
    /// @param threadPool    Layout/worker pool. Passing your own is strongly recommended:
    ///                      the default `Scheduler::GetBackground()` is a hardcoded 4-thread
    ///                      pool shared process-wide (thread_pool.hpp:171-182). See plan §8.1.
    RendererBackend(Size size, FrameSink& sink, MapID mapId = 0);
    RendererBackend(Size size, FrameSink& sink, MapID mapId, const TaggedScheduler& threadPool);
    ~RendererBackend() override;

    gfx::Renderable& getDefaultRenderable() override { return renderable; }

    void initShaders(gfx::ShaderRegistry&, const ProgramParameters&) override;

    void setSize(Size size) { renderable.setSize(size); }

    FrameSink& getSink() noexcept { return sink; }
    MapID getMapID() const noexcept { return mapId; }

protected:
    std::unique_ptr<gfx::Context> createContext() override;

    void activate() override {}
    void deactivate() override {}

private:
    FrameSink& sink;
    MapID mapId;
    Renderable renderable;
};

} // namespace capture
} // namespace mln
