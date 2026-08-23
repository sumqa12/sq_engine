add_library(sq_engine_lib STATIC
    src/ecs/archetype.cpp
    src/ecs/registry.cpp
    src/ecs/system_scheduler.cpp
    src/graphics/window.cpp
    src/graphics/vulkan_instance.cpp
    src/graphics/debug_messenger.cpp
    src/graphics/physical_device.cpp
    src/graphics/device.cpp
    src/graphics/swapchain.cpp
    src/graphics/render_pass.cpp
    src/graphics/graphics_pipeline.cpp
    src/graphics/buffer.cpp
    src/graphics/gpu_allocator.cpp
    src/graphics/mesh.cpp
    src/graphics/mesh_registry.cpp
    src/graphics/command_buffers.cpp
    src/graphics/sync_objects.cpp
    src/graphics/uniform_buffer.cpp
    src/graphics/instance_buffer.cpp
    src/graphics/renderer.cpp
    src/scene/camera.cpp
    src/scene/transform.cpp
    src/scene/frustum.cpp
    src/graphics/depth_image.cpp
    src/graphics/texture.cpp
    src/graphics/texture_registry.cpp
    src/graphics/sampler.cpp
    src/graphics/single_time_commands.cpp
    src/input/input_manager.cpp
    src/input/input_map.cpp
    src/input/camera_control_system.cpp
)

target_include_directories(sq_engine_lib PUBLIC include)
target_compile_features(sq_engine_lib PUBLIC cxx_std_20)

# VulkanのNDC深度は[0,1]。GLMの既定([-1,1])と食い違うため全体で定義する。
if(WIN32)
    # NOMINMAX: VK_USE_PLATFORM_WIN32_KHR により vulkan.h が windows.h を引き込むため、
    #   minwindef.h の関数形式マクロ min/max が全TUに漏れる。std::max(a, b) と書いても
    #   `max` の直後が `(` なのでマクロ展開され `std::((a)>(b)?(a):(b))` になって壊れる。
    #   windows.h より前に定義される必要があるので、コマンドラインで与える。
    # WIN32_LEAN_AND_MEAN: winsock 等の未使用ヘッダを外し、汚染とコンパイル時間を減らす。
    target_compile_definitions(sq_engine_lib PUBLIC
            VK_USE_PLATFORM_WIN32_KHR
            GLFW_EXPOSE_NATIVE_WIN32
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            GLM_FORCE_DEPTH_ZERO_TO_ONE
            $<$<CONFIG:Debug>:DEBUG>)
else ()
    target_compile_definitions(sq_engine_lib PUBLIC
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        $<$<CONFIG:Debug>:DEBUG>)
endif()

find_package(spdlog CONFIG REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Stb REQUIRED)  # stb_image（ヘッダオンリー）。texture.cpp で実体化する。
find_program(glslc_executable NAMES glslc HINTS Vulkan::glslc)

# stb はヘッダオンリーのためインクルードディレクトリのみ追加する。
target_include_directories(sq_engine_lib PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(sq_engine_lib PUBLIC
    spdlog::spdlog
    Vulkan::Vulkan
    glfw
    glm::glm
)
