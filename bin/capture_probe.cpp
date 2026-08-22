// Phase 0 probe for the ECS capture backend.
//
// Runs the complete MapLibre frontend -- Map, Style, sources, tile layout, buckets, layer
// drawable construction, tweakers, the whole RenderOrchestrator pass -- against
// `capture::RendererBackend`, and reports the FrameDiff stream that comes out.
//
// The point is to demonstrate the Phase 0 exit criterion: mbgl completes real frames with no
// GPU of any kind present, and background/fill/line drawables arrive with their vertex
// attribute descriptors and shader permutation keys resolved.
//
// By default it renders a self-contained style (inline GeoJSON, no network) so the run is
// hermetic. Pass a style URL or file path to point it at a real style instead.

#include <mbgl/capture/frame_diff.hpp>
#include <mbgl/capture/renderer_backend.hpp>

#include <mbgl/gfx/backend.hpp>
#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_observer.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/renderer/renderer.hpp>
#include <mbgl/renderer/renderer_frontend.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/run_loop.hpp>

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

using namespace mln;

namespace {

constexpr const char* kInlineStyle = R"JSON({
  "version": 8,
  "name": "capture-probe",
  "sources": {
    "probe": {
      "type": "geojson",
      "data": {
        "type": "FeatureCollection",
        "features": [
          {
            "type": "Feature",
            "properties": { "kind": "a" },
            "geometry": {
              "type": "Polygon",
              "coordinates": [[[-0.10,51.49],[-0.10,51.53],[-0.05,51.53],[-0.05,51.49],[-0.10,51.49]]]
            }
          },
          {
            "type": "Feature",
            "properties": { "kind": "b" },
            "geometry": {
              "type": "Polygon",
              "coordinates": [[[-0.16,51.49],[-0.16,51.52],[-0.12,51.52],[-0.12,51.49],[-0.16,51.49]]]
            }
          },
          {
            "type": "Feature",
            "properties": { "kind": "a" },
            "geometry": {
              "type": "LineString",
              "coordinates": [[-0.18,51.48],[-0.12,51.51],[-0.06,51.50],[-0.02,51.53]]
            }
          },
          {
            "type": "Feature",
            "properties": { "kind": "b" },
            "geometry": { "type": "Point", "coordinates": [-0.09,51.505] }
          }
        ]
      }
    }
  },
  "layers": [
    {
      "id": "bg",
      "type": "background",
      "paint": { "background-color": "#101418" }
    },
    {
      "id": "fill-constant",
      "type": "fill",
      "source": "probe",
      "filter": ["==", "$type", "Polygon"],
      "paint": { "fill-color": "#2f6f4f", "fill-opacity": 0.8 }
    },
    {
      "id": "fill-datadriven",
      "type": "fill",
      "source": "probe",
      "filter": ["==", "$type", "Polygon"],
      "paint": {
        "fill-color": ["match", ["get", "kind"], "a", "#c04030", "#3050c0"],
        "fill-opacity": ["match", ["get", "kind"], "a", 0.5, 0.9]
      }
    },
    {
      "id": "line-datadriven",
      "type": "line",
      "source": "probe",
      "filter": ["==", "$type", "LineString"],
      "paint": {
        "line-color": ["match", ["get", "kind"], "a", "#e0d040", "#40e0d0"],
        "line-width": ["match", ["get", "kind"], "a", 4.0, 2.0]
      }
    },
    {
      "id": "circle-constant",
      "type": "circle",
      "source": "probe",
      "filter": ["==", "$type", "Point"],
      "paint": { "circle-color": "#ffffff", "circle-radius": 8 }
    }
  ]
})JSON";

/// Minimal `RendererFrontend`: owns the capture backend and a `Renderer`, exactly as
/// `MapLibreFrontend` in maplibre_view owns a real one. Pull model -- `update()` stores the
/// parameters and the caller decides when to render, which is the dirty-gating discipline
/// the plan calls for (§8.3).
class CaptureFrontend final : public RendererFrontend {
public:
    CaptureFrontend(Size size, float pixelRatio, capture::FrameSink& sink)
        : backend(std::make_unique<capture::RendererBackend>(size, sink)),
          renderer(std::make_unique<Renderer>(*backend, pixelRatio)) {}

    ~CaptureFrontend() override = default;

    void reset() override { renderer.reset(); }

    void setObserver(RendererObserver& observer) override {
        if (renderer) {
            renderer->setObserver(&observer);
        }
    }

    void update(std::shared_ptr<UpdateParameters> params) override {
        updateParameters = std::move(params);
        dirty = true;
    }

    const TaggedScheduler& getThreadPool() const override { return backend->getThreadPool(); }

    /// Returns true if a frame was actually produced.
    bool renderFrameIfDirty() {
        if (!renderer || !updateParameters || !dirty) {
            return false;
        }
        dirty = false;
        gfx::BackendScope guard{*backend};
        auto params = updateParameters;
        renderer->render(params);
        return true;
    }

private:
    std::unique_ptr<capture::RendererBackend> backend;
    std::unique_ptr<Renderer> renderer;
    std::shared_ptr<UpdateParameters> updateParameters;
    bool dirty = false;
};

class ProbeObserver final : public MapObserver {
public:
    void onDidFinishLoadingStyle() override {
        styleLoaded = true;
        Log::Info(Event::General, "probe: style loaded");
    }
    void onDidFailLoadingMap(MapLoadError, const std::string& what) override { failure = "map load failed: " + what; }

    bool styleLoaded = false;
    std::string failure;
};

} // namespace

int main(int argc, char* argv[]) {
    const std::string styleArg = (argc > 1) ? argv[1] : std::string{};
    const bool verbose = [&] {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-v") == 0) {
                return true;
            }
        }
        return false;
    }();
    constexpr int kFrames = 8;

    // Data-driven paint properties should always materialize as vertex attributes or UBO
    // fields rather than GPU-evaluated expression trees (plan §3.1). Only the line layer
    // consults this today, which is exactly the layer in the probe style that needs it.
    gfx::Backend::setEnableGPUExpressionEval(false);

    util::RunLoop runLoop;

    capture::LogFrameSink sink{verbose};
    CaptureFrontend frontend{Size{1024, 768}, 1.0f, sink};
    ProbeObserver observer;

    Map map{frontend,
            observer,
            MapOptions().withMapMode(MapMode::Continuous).withSize(Size{1024, 768}),
            ResourceOptions().withCachePath(":memory:")};

    if (styleArg.empty() || styleArg[0] == '-') {
        map.getStyle().loadJSON(kInlineStyle);
        Log::Info(Event::General, "probe: using built-in offline style");
    } else {
        map.getStyle().loadURL(styleArg);
        Log::Info(Event::General, "probe: loading style " + styleArg);
    }

    map.jumpTo(CameraOptions().withCenter(LatLng{51.505, -0.11}).withZoom(13.0));

    int framesRendered = 0;
    int framesWithContent = 0;
    int spins = 0;
    constexpr int kMaxSpins = 200000;

    // Tile parse and layout run on the background pool, so the first frames legitimately
    // carry nothing. Keep pumping until drawables actually arrive, then render a few more so
    // the steady-state diff (which should be UBO-only, not geometry) is observable.
    while (framesWithContent < kFrames && spins++ < kMaxSpins) {
        runLoop.runOnce();
        if (!observer.failure.empty()) {
            break;
        }
        if (frontend.renderFrameIfDirty()) {
            ++framesRendered;
            if (sink.getStats().drawsOrdered > 0) {
                ++framesWithContent;
            }
            // Keep the map busy so tiles keep arriving and drawables keep being rebuilt.
            map.triggerRepaint();
        }
    }

    const auto& stats = sink.getStats();

    std::printf("\n=== capture probe result ===\n");
    if (!observer.failure.empty()) {
        std::printf("FAILED: %s\n", observer.failure.c_str());
        return 1;
    }
    std::printf("style loaded      : %s\n", observer.styleLoaded ? "yes" : "no");
    std::printf("frames rendered   : %d (%d with content, loop spins %d)\n", framesRendered, framesWithContent, spins);
    std::printf(
        "drawable adds     : %llu (created %llu, idx-replaced %llu, attrs-replaced %llu, attrs-modified %llu)\n",
        static_cast<unsigned long long>(stats.drawableAdds),
        static_cast<unsigned long long>(stats.addsCreated),
        static_cast<unsigned long long>(stats.addsIndexDataReplaced),
        static_cast<unsigned long long>(stats.addsAttributesReplaced),
        static_cast<unsigned long long>(stats.addsAttributesModified));
    std::printf("drawable removes  : %llu\n", static_cast<unsigned long long>(stats.drawableRemoves));
    std::printf("live drawables    : %llu\n", static_cast<unsigned long long>(stats.liveDrawables));
    std::printf("ubo updates       : %llu\n", static_cast<unsigned long long>(stats.uboUpdates));
    std::printf("texture updates   : %llu\n", static_cast<unsigned long long>(stats.textureUpdates));
    std::printf("stencil tile sets : %llu\n", static_cast<unsigned long long>(stats.stencilTileSets));
    std::printf("draws in last order: %llu\n", static_cast<unsigned long long>(stats.drawsOrdered));

    const bool ok = observer.styleLoaded && framesWithContent >= kFrames && stats.drawableAdds > 0 &&
                    stats.drawsOrdered > 0;
    std::printf(
        "\n%s\n",
        ok ? "PASS: frontend ran headless with zero GPU and produced a diff stream" : "FAIL: no drawables captured");
    return ok ? 0 : 1;
}
