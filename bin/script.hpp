// Replaying a short script against a map before it is drawn.
//
// `mbgl-render` draws one still picture of a style at a camera, which is the whole of what a
// stylesheet can say and not the whole of what a map does. Half the GL JS examples hand a source
// new data once the style has loaded -- a point moving along a route, live data arriving, a
// polygon dragged -- so a tool that cannot do that cannot draw them, and cannot be a reference for
// a renderer that can.
//
// # The vocabulary is the render tests'
//
// `render-test/parser.cpp` already names these calls: an operation is an array with the name
// first -- `["setZoom", 4]`, `["setCenter", [lon, lat]]`, `["addSource", id, {...}]` -- read from
// a test's `metadata.test.operations`. This takes the same spellings, so a script written here
// says what a render test says and a reader who knows one knows the other. A second vocabulary
// for the same idea in the same repository would be the wrong thing to add, however convenient
// another shape is to write.
//
//   [
//     ["setData", "route", {"type": "FeatureCollection", "features": []}],
//     ["setData", "route", "https://example.com/points.geojson"],
//     ["addImage", "cat", "cat.png", {"pixelRatio": 2, "sdf": false}],
//     ["setCenter", [-74.0, 40.7]],
//     ["setZoom", 15.5],
//     ["setBearing", -17.6],
//     ["setPitch", 45]
//   ]
//
// `addImage` is theirs, arguments and all, with one difference: the render tests fetch the file
// through their own `mapbox://render-tests/` file source, and this reads it from disk, as
// `--annotation-image` does. A tool a person runs by hand has a path, not a test harness.
//
// `setData` is the one name the render tests do not have, and they have no way to express it:
// `removeSource` refuses a source a layer still uses, which is every source worth replacing, so
// remove-then-add cannot stand in for it. It is spelled the way GL JS spells the call it mirrors,
// `map.getSource(id).setData(...)`, which is the rule the other names follow.
//
// Written for `render.cpp`, and kept beside `annotations.hpp` in the shape that header has so the
// capture probe can take the same file when it needs to. The probe drives its own frontend and
// parses its own arguments, so wiring it is its own change rather than a line here.

#pragma once

#include <mbgl/map/map.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/style/sources/geojson_source.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/geojson.hpp>
#include <mbgl/util/image.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mln_script {

// The tool's own reading of its own file. `mbgl/util/rapidjson.hpp` is private to the library, and
// nothing downstream reads these values: only the GeoJSON inside an operation reaches a parser
// that cares, and it reaches it as text.
using Value = rapidjson::Value;

inline std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

inline std::string serialize(const Value& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}

// The source an operation names, as the kind of source the operation needs it to be.
//
// Refused rather than skipped: an operation naming a source the style does not have describes a
// different map, and a picture drawn past it would look like an answer.
inline mln::style::GeoJSONSource& geojsonSource(mln::Map& map, const std::string& id) {
    mln::style::Source* source = map.getStyle().getSource(id);
    if (source == nullptr) {
        throw std::runtime_error("the style has no source named " + id);
    }
    auto* geojson = source->as<mln::style::GeoJSONSource>();
    if (geojson == nullptr) {
        throw std::runtime_error("source " + id + " is not a GeoJSON source");
    }
    return *geojson;
}

// What an operation has to carry, beside its name. The render tests assert these arities; a tool
// a person runs by hand says what it wanted instead.
inline void wants(const Value& operation, rapidjson::SizeType count, const char* shape) {
    if (operation.Size() < count + 1) {
        throw std::runtime_error(std::string("wants ") + shape);
    }
}

inline double number(const Value& operation, rapidjson::SizeType at, const char* shape) {
    if (!operation[at].IsNumber()) {
        throw std::runtime_error(std::string("wants ") + shape);
    }
    return operation[at].GetDouble();
}

// `["setData", id, <geojson object> | <url string>]`
//
// GL JS's `map.getSource(id).setData(...)`. An object is the document itself and a string is a URL
// to fetch it from, which is the pair the style's own `data` accepts.
inline void setData(mln::Map& map, const Value& operation) {
    wants(operation, 2, R"(["setData", id, <geojson>|<url>])");
    if (!operation[1].IsString()) {
        throw std::runtime_error("wants a source id");
    }
    auto& source = geojsonSource(map, operation[1].GetString());
    if (operation[2].IsString()) {
        source.setURL(operation[2].GetString());
        return;
    }
    // Back to text and through the GeoJSON parser, rather than converted in place: that parser is
    // where the style's own `data` goes, and a second reading of the same bytes is a second chance
    // to disagree with it.
    source.setGeoJSON(mapbox::geojson::parse(serialize(operation[2])));
}

// `["addImage", id, path, {"pixelRatio": r, "sdf": b}]`
//
// GL JS's `map.addImage(id, image)`. The picture is read from `path` and joins the style's own
// sheet, under a name any layer can ask for -- so a style whose sprite does not carry an icon can
// still draw one, which is what half the examples that need this do.
inline void addImage(mln::Map& map, const Value& operation) {
    wants(operation, 2, R"(["addImage", id, path, {"pixelRatio": r, "sdf": b}])");
    if (!operation[1].IsString() || !operation[2].IsString()) {
        throw std::runtime_error("wants an id and a path");
    }
    float pixelRatio = 1.0f;
    bool sdf = false;
    if (operation.Size() >= 4 && operation[3].IsObject()) {
        const Value& options = operation[3];
        if (options.HasMember("pixelRatio") && options["pixelRatio"].IsNumber()) {
            pixelRatio = options["pixelRatio"].GetFloat();
        }
        if (options.HasMember("sdf") && options["sdf"].IsBool()) {
            sdf = options["sdf"].GetBool();
        }
    }
    map.getStyle().addImage(std::make_unique<mln::style::Image>(
        operation[1].GetString(), mln::decodeImage(readFile(operation[2].GetString())), pixelRatio,
        sdf));
}

// The camera, one property at a time, as the render tests move it. `setCenter` takes `[lon, lat]`,
// which is GeoJSON's order and theirs.
inline void setCenter(mln::Map& map, const Value& operation) {
    wants(operation, 1, R"(["setCenter", [lon, lat]])");
    if (!operation[1].IsArray() || operation[1].Size() < 2) {
        throw std::runtime_error(R"(wants ["setCenter", [lon, lat]])");
    }
    const auto& center = operation[1];
    map.jumpTo(mln::CameraOptions().withCenter(
        mln::LatLng{number(center, 1, "a latitude"), number(center, 0, "a longitude")}));
}

inline void setZoom(mln::Map& map, const Value& operation) {
    wants(operation, 1, R"(["setZoom", number])");
    map.jumpTo(mln::CameraOptions().withZoom(number(operation, 1, R"(["setZoom", number])")));
}

inline void setBearing(mln::Map& map, const Value& operation) {
    wants(operation, 1, R"(["setBearing", degrees])");
    map.jumpTo(mln::CameraOptions().withBearing(number(operation, 1, R"(["setBearing", degrees])")));
}

inline void setPitch(mln::Map& map, const Value& operation) {
    wants(operation, 1, R"(["setPitch", degrees])");
    map.jumpTo(mln::CameraOptions().withPitch(number(operation, 1, R"(["setPitch", degrees])")));
}

// Applies every operation of `path`, in order.
//
// Called after a first render, because an operation names what the *style* holds and the style is
// what the first render loads. Before it there is no source list to name -- a distinction the
// annotation path does not have to make, because `AnnotationManager` synthesizes its source and
// layers behind the style's back and they survive it loading, where a source does not.
inline void applyScript(mln::Map& map, const std::string& path) {
    rapidjson::Document document;
    document.Parse<rapidjson::kParseDefaultFlags>(readFile(path).c_str());
    if (document.HasParseError()) {
        throw std::runtime_error(path + " is not JSON");
    }
    if (!document.IsArray()) {
        throw std::runtime_error(path + " is not an array of operations");
    }
    for (const auto& operation : document.GetArray()) {
        if (!operation.IsArray() || operation.Empty() || !operation[0].IsString()) {
            throw std::runtime_error("an operation is an array with its name first");
        }
        const std::string name = operation[0].GetString();
        try {
            if (name == "setData") {
                setData(map, operation);
            } else if (name == "addImage") {
                addImage(map, operation);
            } else if (name == "setCenter") {
                setCenter(map, operation);
            } else if (name == "setZoom") {
                setZoom(map, operation);
            } else if (name == "setBearing") {
                setBearing(map, operation);
            } else if (name == "setPitch") {
                setPitch(map, operation);
            } else {
                // Named rather than ignored. The render tests have thirty more operations, and a
                // script asking for one this tool has not grown yet is a script whose picture
                // would be missing whatever it asked for.
                throw std::runtime_error("this tool does not do that one yet");
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(name + ": " + error.what());
        }
    }
}

} // namespace mln_script
