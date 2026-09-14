// Reading annotations for a command-line tool.
//
// An annotation is not a style layer. There is no `"type": "annotation"`, and nothing in a
// stylesheet can produce one: they arrive through `Map::addAnnotation`, and `AnnotationManager`
// synthesizes the source and the layers behind the style's back. So a tool that takes a style
// file and nothing else cannot draw one.
//
// Shared by `render.cpp` and `capture_probe.cpp` rather than written twice. The two tools answer
// different questions about the same scene -- one makes a picture and the other dumps the
// drawables -- and a scene read differently by each is a scene whose two answers cannot be
// compared, which is the whole reason to have both.

#pragma once

#include <mbgl/annotation/annotation.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/util/geojson.hpp>
#include <mbgl/util/image.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace mln_annotations {

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

} // namespace mln_annotations
