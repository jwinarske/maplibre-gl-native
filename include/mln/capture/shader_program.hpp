#pragma once

#include <mln/gfx/context.hpp>
#include <mln/gfx/shader_group.hpp>
#include <mln/shaders/shader_program_base.hpp>
#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/program_parameters.hpp>
#include <mln/util/hash.hpp>
#include <mln/util/containers.hpp>

// The capture backend masquerades as `Backend::Type::Vulkan` so it can reuse the
// per-shader attribute/texture metadata tables in src/mln/shaders/vulkan/*.cpp rather
// than maintaining its own ~950 LOC copy. That is the plan §4 / §7 decision; it is the
// reason this header reaches into the vulkan shader tree. Nothing Vulkan is instantiated.
#include <mln/shaders/vulkan/shader_program.hpp>

#include <array>
#include <optional>
#include <string>

namespace mln {
namespace capture {

/// A shader with no source and no compilation: it exists only to declare which vertex
/// attributes and texture slots this permutation uses, which is all the render layers ask of
/// it (`ShaderProgramBase`, shaders/shader_program_base.hpp:34-40).
class ShaderProgram final : public gfx::ShaderProgramBase {
public:
    ShaderProgram(shaders::BuiltIn shaderID_, std::string name_, std::uint64_t permutationKey_)
        : shaderID(shaderID_),
          shaderName(std::move(name_)),
          permutationKey(permutationKey_) {}
    ~ShaderProgram() noexcept override = default;

    static constexpr std::string_view Name{"CaptureShader"};
    const std::string_view typeName() const noexcept override { return Name; }

    shaders::BuiltIn getShaderID() const noexcept { return shaderID; }
    const std::string& getName() const noexcept { return shaderName; }
    std::uint64_t getPermutationKey() const noexcept { return permutationKey; }

    std::optional<size_t> getSamplerLocation(const size_t id) const override {
        return id < textureBindings.size() ? textureBindings[id] : std::nullopt;
    }
    const gfx::VertexAttributeArray& getVertexAttributes() const override { return vertexAttributes; }
    const gfx::VertexAttributeArray& getInstanceAttributes() const override { return instanceAttributes; }

    void initVertexAttribute(const shaders::AttributeInfo& info) {
        vertexAttributes.set(info.id, static_cast<int>(info.index), info.dataType, 1);
    }
    void initInstanceAttribute(const shaders::AttributeInfo& info) {
        instanceAttributes.set(info.id, static_cast<int>(info.index), info.dataType, 1);
    }
    void initTexture(const shaders::TextureInfo& info) {
        if (info.id < textureBindings.size()) {
            textureBindings[info.id] = info.index;
        }
    }

private:
    shaders::BuiltIn shaderID;
    std::string shaderName;
    std::uint64_t permutationKey;

    gfx::VertexAttributeArray vertexAttributes;
    gfx::VertexAttributeArray instanceAttributes;
    std::array<std::optional<size_t>, shaders::maxTextureCountPerShader> textureBindings;
};

using UniqueShaderProgram = std::unique_ptr<ShaderProgram>;

/// Resolves one shader permutation per distinct `propertiesAsUniforms` set, filtering the
/// declared attribute list exactly as the real backends do (vulkan/shader_group.hpp:78-90).
/// The filtering is the whole point: it is what tells the consumer whether a data-driven
/// property arrives as a vertex attribute or as a layer-UBO field.
template <shaders::BuiltIn ShaderID>
class ShaderGroup final : public gfx::ShaderGroup {
public:
    explicit ShaderGroup(const ProgramParameters& programParameters_)
        : programParameters(programParameters_) {}
    ~ShaderGroup() noexcept override = default;

    gfx::ShaderPtr getOrCreateShader(gfx::Context&,
                                     const StringIDSetsPair& propertiesAsUniforms,
                                     std::string_view /*firstAttribName*/) override {
        using ShaderClass = shaders::ShaderSource<ShaderID, gfx::Backend::Type::Vulkan>;

        std::size_t seed = 0;
        mln::util::hash_combine(seed, propertyHash(propertiesAsUniforms));
        mln::util::hash_combine(seed, programParameters.getDefinesHash());
        const std::string shaderName = getShaderName(ShaderClass::name, seed);

        if (auto existing = get<ShaderProgram>(shaderName)) {
            return existing;
        }

        auto shader = std::make_shared<ShaderProgram>(ShaderID, shaderName, static_cast<std::uint64_t>(seed));

        for (const auto& attrib : ShaderClass::attributes) {
            if (!propertiesAsUniforms.second.contains(attrib.id)) {
                shader->initVertexAttribute(attrib);
            }
        }
        for (const auto& attrib : ShaderClass::instanceAttributes) {
            if (!propertiesAsUniforms.second.contains(attrib.id)) {
                shader->initInstanceAttribute(attrib);
            }
        }
        for (const auto& texture : ShaderClass::textures) {
            shader->initTexture(texture);
        }

        std::shared_ptr<gfx::Shader> asShader = shader;
        if (!registerShader(std::move(asShader), shaderName)) {
            // Another thread registered this permutation between our lookup and now. Take
            // theirs; returning null here would leave the layer with no shader at all.
            return get<ShaderProgram>(shaderName);
        }
        return shader;
    }

private:
    ProgramParameters programParameters;
};

} // namespace capture
} // namespace mln
