if(NOT MLN_WITH_CAPTURE)
    return()
endif()

# The capture backend diverts the renderer's output into a FrameDiff stream instead of a GPU.
#
# It requires a concrete backend to be selected as well, because shared (non-backend) code in
# paint_parameters.cpp / renderer_impl.cpp / line_layer_tweaker.cpp is compile-time dispatched
# on MLN_RENDER_BACKEND_* and references that backend's concrete Context type. Vulkan is the
# supported pairing: OpenGL is disqualified because PaintParameters::updateStencilBufferAvailability
# static_casts to gl::Context on the *shared* path (renderer_impl.cpp:326,379), which the
# capture Context cannot satisfy. Under Vulkan the equivalent casts live only in
# renderTileClippingMasks/clearStencil, which are reachable exclusively from a backend's own
# layer group -- and ours never calls them. See plan §4.
if(NOT MLN_WITH_VULKAN)
    message(FATAL_ERROR
        "MLN_WITH_CAPTURE requires MLN_WITH_VULKAN=ON. The Vulkan backend is compiled and linked "
        "but never instantiated; it exists so mbgl-core's compile-time backend branches resolve.")
endif()

message(STATUS "Configuring capture renderer backend")

target_compile_definitions(
        mbgl-core
        PUBLIC
        MLN_RENDER_BACKEND_CAPTURE=1
)

list(APPEND
        INCLUDE_FILES
        ${PROJECT_SOURCE_DIR}/include/mln/capture/renderable.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/context.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/drawable.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/drawable_builder.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/frame_diff.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/layer_group.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/offscreen_texture.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/renderer_backend.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/shader_program.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/texture2d.hpp
        ${PROJECT_SOURCE_DIR}/include/mln/capture/uniform_buffer.hpp
)

list(APPEND
        SRC_FILES
        ${PROJECT_SOURCE_DIR}/src/mln/capture/command_encoder.hpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/command_encoder.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/context.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/drawable.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/drawable_builder.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/frame_diff.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/layer_group.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/offscreen_texture.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/renderer_backend.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/dynamic_texture.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/texture2d.cpp
        ${PROJECT_SOURCE_DIR}/src/mln/capture/uniform_buffer.cpp
)

# Phase 0 probe: runs the frontend headless against the capture backend and reports the
# FrameDiff stream. Needs the platform layer (RunLoop, file source), so it is skipped in
# core-only builds.
if(NOT MLN_WITH_CORE_ONLY)
    add_executable(mbgl-capture-probe ${PROJECT_SOURCE_DIR}/bin/capture_probe.cpp)
    target_link_libraries(mbgl-capture-probe PRIVATE mbgl-core)
    # --dump hashes vertex and index buffer contents, and the vector types that own them are
    # internal headers rather than public API.
    target_include_directories(mbgl-capture-probe PRIVATE ${PROJECT_SOURCE_DIR}/src)
endif()
