#include <mbgl/annotation/annotation.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/util/geojson.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/run_loop.hpp>

#include <mbgl/gfx/backend.hpp>
#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/style/style.hpp>

#include <args.hxx>

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <optional>
#include <sstream>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::optional<double> numberProperty(const mln::GeoJSONFeature& feature, const std::string& key) {
    const auto it = feature.properties.find(key);
    if (it == feature.properties.end()) return std::nullopt;
    return it->second.match([](uint64_t v) { return std::optional<double>(static_cast<double>(v)); },
                            [](int64_t v) { return std::optional<double>(static_cast<double>(v)); },
                            [](double v) { return std::optional<double>(v); },
                            [](const auto&) { return std::optional<double>(); });
}

std::optional<std::string> stringProperty(const mln::GeoJSONFeature& feature, const std::string& key) {
    const auto it = feature.properties.find(key);
    if (it == feature.properties.end() || !it->second.template is<std::string>()) return std::nullopt;
    return it->second.template get<std::string>();
}

std::optional<mln::Color> colorProperty(const mln::GeoJSONFeature& feature, const std::string& key) {
    const auto text = stringProperty(feature, key);
    if (!text) return std::nullopt;
    return mln::Color::parse(*text);
}

// A float paint property, left at the annotation class's own default when the feature is silent.
template <typename T>
mln::style::PropertyValue<T> paint(const std::optional<T>& value, T fallback) {
    return mln::style::PropertyValue<T>(value ? *value : fallback);
}

// The geometry type picks the annotation class, the way mbgl's own three classes split: a point is
// a symbol, a line or multi-line is a line, a polygon or multi-polygon is a fill. Nothing in the
// file says which class it wants, because nothing has to.
void addAnnotations(mln::Map& map, const std::string& path) {
    const auto geojson = mapbox::geojson::parse(readFile(path));
    if (!geojson.is<mln::FeatureCollection>()) throw std::runtime_error(path + " is not a FeatureCollection");

    for (const auto& feature : geojson.get<mln::FeatureCollection>()) {
        const auto opacity = numberProperty(feature, "opacity");
        const auto width = numberProperty(feature, "width");
        const auto color = colorProperty(feature, "color");
        const auto outline = colorProperty(feature, "outlineColor");

        const auto line = [&](mln::ShapeAnnotationGeometry geometry) {
            map.addAnnotation(mln::LineAnnotation{
                std::move(geometry),
                paint<float>(opacity ? std::optional<float>(static_cast<float>(*opacity)) : std::nullopt, 1.0f),
                paint<float>(width ? std::optional<float>(static_cast<float>(*width)) : std::nullopt, 1.0f),
                paint<mln::Color>(color, mln::Color::black())});
        };
        const auto fill = [&](mln::ShapeAnnotationGeometry geometry) {
            map.addAnnotation(mln::FillAnnotation{
                std::move(geometry),
                paint<float>(opacity ? std::optional<float>(static_cast<float>(*opacity)) : std::nullopt, 1.0f),
                paint<mln::Color>(color, mln::Color::black()),
                outline ? mln::style::PropertyValue<mln::Color>(*outline) : mln::style::PropertyValue<mln::Color>()});
        };

        feature.geometry.match(
            [&](const mln::Point<double>& geometry) {
                map.addAnnotation(mln::SymbolAnnotation{geometry, stringProperty(feature, "icon").value_or("")});
            },
            [&](const mln::LineString<double>& geometry) { line(geometry); },
            [&](const mln::MultiLineString<double>& geometry) { line(geometry); },
            [&](const mln::Polygon<double>& geometry) { fill(geometry); },
            [&](const mln::MultiPolygon<double>& geometry) { fill(geometry); },
            [&](const auto&) { std::cerr << "skipping an annotation geometry no annotation class holds" << std::endl; });
    }
}

// `id=path.png`. The id is what a symbol annotation's `icon` names, and `default_marker` is what an
// annotation with no icon asks for.
void addAnnotationImage(mln::Map& map, const std::string& spec, float pixelRatio) {
    const auto split = spec.find('=');
    if (split == std::string::npos) throw std::runtime_error("annotation image wants id=path, got " + spec);
    const auto id = spec.substr(0, split);
    map.addAnnotationImage(
        std::make_unique<mln::style::Image>(id, mln::decodeImage(readFile(spec.substr(split + 1))), pixelRatio, false));
}

} // namespace

int main(int argc, char* argv[]) {
    args::ArgumentParser argumentParser("MapLibre Native render tool");
    args::HelpFlag helpFlag(argumentParser, "help", "Display this help menu", {"help"});

    args::ValueFlag<std::string> backendValue(argumentParser, "Backend", "Rendering backend", {"backend"});
    args::ValueFlag<std::string> apikeyValue(argumentParser, "key", "API key", {'t', "apikey"});
    args::ValueFlag<std::string> styleValue(argumentParser, "URL", "Map stylesheet", {'s', "style"});
    args::ValueFlag<std::string> outputValue(argumentParser, "file", "Output file name", {'o', "output"});
    args::ValueFlag<std::string> cacheValue(argumentParser, "file", "Cache database file name", {'c', "cache"});
    args::ValueFlag<std::string> assetsValue(
        argumentParser, "file", "Directory to which asset:// URLs will resolve", {'a', "assets"});

    args::Flag debugFlag(argumentParser, "debug", "Debug mode", {"debug"});

    args::ValueFlag<double> pixelRatioValue(argumentParser, "number", "Image scale factor", {'r', "ratio"});

    // grouping ensures either bounds or center based position is used
    args::Group boundsOrCenterZoom(argumentParser, "Position (either one):", args::Group::Validators::AtMostOne);

    args::NargsValueFlag<double> boundsValue(
        boundsOrCenterZoom, "degrees: north west south east", "Bounds of rendered map", {"bounds"}, 4);

    args::Group centerGroup(boundsOrCenterZoom, "Center:", args::Group::Validators::AtLeastOne);
    args::ValueFlag<double> zoomValue(centerGroup, "number", "Zoom level", {'z', "zoom"});
    args::ValueFlag<double> lonValue(centerGroup, "degrees", "Longitude", {'x', "lon"});
    args::ValueFlag<double> latValue(centerGroup, "degrees", "Latitude", {'y', "lat"});
    args::ValueFlag<double> altValue(argumentParser, "degrees", "Altitude", {'A', "alt"});
    args::ValueFlag<double> fovValue(argumentParser, "degrees", "FOV", {'f', "fov"});
    args::ValueFlag<double> bearingValue(argumentParser, "degrees", "Bearing", {'b', "bearing"});
    args::ValueFlag<double> pitchValue(argumentParser, "degrees", "Pitch", {'p', "pitch"});
    args::ValueFlag<double> rollValue(argumentParser, "degrees", "Roll", {'R', "roll"});
    args::ValueFlag<uint32_t> widthValue(argumentParser, "pixels", "Image width", {'w', "width"});
    args::ValueFlag<uint32_t> heightValue(argumentParser, "pixels", "Image height", {'h', "height"});

    args::ValueFlag<std::string> mapModeValue(
        argumentParser, "MapMode", "Map mode (e.g. 'static', 'tile', 'continuous')", {'m', "mode"});

    args::ValueFlag<std::string> annotationsValue(
        argumentParser, "file", "GeoJSON FeatureCollection to add as annotations", {"annotations"});
    args::ValueFlagList<std::string> annotationImageValues(
        argumentParser, "id=file", "Image a symbol annotation's `icon` can name", {"annotation-image"});

    try {
        argumentParser.ParseCLI(argc, argv);
    } catch (const args::Help&) {
        std::cout << argumentParser;
        exit(0);
    } catch (const args::ParseError& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << argumentParser;
        exit(1);
    } catch (const args::ValidationError& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << argumentParser;
        exit(2);
    }

    const double lat = latValue ? args::get(latValue) : 0;
    const double lon = lonValue ? args::get(lonValue) : 0;
    const double alt = altValue ? args::get(altValue) : 0;
    const double zoom = zoomValue ? args::get(zoomValue) : 0;
    const double fov = fovValue ? args::get(fovValue) : mln::util::rad2deg(mln::util::DEFAULT_FOV);
    const double bearing = bearingValue ? args::get(bearingValue) : 0;
    const double pitch = pitchValue ? args::get(pitchValue) : 0;
    const double roll = rollValue ? args::get(rollValue) : 0;
    const double pixelRatio = pixelRatioValue ? args::get(pixelRatioValue) : 1;

    const uint32_t width = widthValue ? args::get(widthValue) : 512;
    const uint32_t height = heightValue ? args::get(heightValue) : 512;
    const std::string output = outputValue ? args::get(outputValue) : "out.png";
    const std::string cache_file = cacheValue ? args::get(cacheValue) : "cache.sqlite";
    const std::string asset_root = assetsValue ? args::get(assetsValue) : ".";

    // Try to load the apikey from the environment.
    const char* apikeyEnv = getenv("MLN_API_KEY");
    const std::string apikey = apikeyValue ? args::get(apikeyValue) : (apikeyEnv ? apikeyEnv : std::string());

    const bool debug = debugFlag ? args::get(debugFlag) : false;

    using namespace mln;

    auto mapTilerConfiguration = mln::TileServerOptions::MapTilerConfiguration();
    std::string style = styleValue ? args::get(styleValue) : mapTilerConfiguration.defaultStyles().at(0).getUrl();

    util::RunLoop loop;

    MapMode mapMode = MapMode::Static;
    if (mapModeValue) {
        const auto modeStr = args::get(mapModeValue);
        if (modeStr == "tile") {
            mapMode = MapMode::Tile;
        } else if (modeStr == "continuous") {
            mapMode = MapMode::Continuous;
        }
    }

    HeadlessFrontend frontend({width, height}, static_cast<float>(pixelRatio));
    Map map(
        frontend,
        MapObserver::nullObserver(),
        MapOptions().withMapMode(mapMode).withSize(frontend.getSize()).withPixelRatio(static_cast<float>(pixelRatio)),
        ResourceOptions()
            .withCachePath(cache_file)
            .withAssetPath(asset_root)
            .withApiKey(apikey)
            .withTileServerOptions(mapTilerConfiguration));

    if (style.find("://") == std::string::npos) {
        style = std::string("file://") + style;
    }

    map.getStyle().loadURL(style);

    // Before the render, because the render is what loads the style, and `onStyleLoaded` re-runs
    // `AnnotationManager::updateStyle` -- so the source and layers survive the style replacing them.
    try {
        for (const auto& spec : args::get(annotationImageValues)) {
            addAnnotationImage(map, spec, static_cast<float>(pixelRatio));
        }
        if (annotationsValue) {
            addAnnotations(map, args::get(annotationsValue));
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        exit(1);
    }
    std::vector<double> bounds = args::get(boundsValue);
    if (bounds.size() == 4) {
        LatLngBounds boundingBox = LatLngBounds::hull(LatLng(bounds[0], bounds[1]), LatLng(bounds[2], bounds[3]));
        map.jumpTo(map.cameraForLatLngBounds(boundingBox, EdgeInsets(), bearing, pitch));
    } else {
        map.jumpTo(CameraOptions()
                       .withCenter(LatLng{lat, lon})
                       .withCenterAltitude(alt)
                       .withZoom(zoom)
                       .withBearing(bearing)
                       .withPitch(pitch)
                       .withRoll(roll)
                       .withFov(fov));
    }

    if (debug) {
        map.setDebug(debug ? mln::MapDebugOptions::TileBorders | mln::MapDebugOptions::ParseStatus
                           : mln::MapDebugOptions::NoDebug);
    }

    try {
        std::ofstream out(output, std::ios::binary);
        out << encodePNG(frontend.render(map).image);
        out.close();
    } catch (std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        exit(1);
    }

    return 0;
}
