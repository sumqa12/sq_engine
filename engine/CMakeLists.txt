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
    src/graphics/light_buffer.cpp
    src/graphics/deletion_queue.cpp
    src/graphics/material_buffer.cpp
    src/graphics/material_registry.cpp
    src/graphics/renderer.cpp
    src/scene/camera.cpp
    src/scene/transform.cpp
    src/scene/transform_system.cpp
    src/scene/frustum.cpp
    src/graphics/depth_image.cpp
    src/graphics/texture.cpp
    src/graphics/texture_registry.cpp
    src/graphics/sampler.cpp
    src/graphics/single_time_commands.cpp
    src/input/input_manager.cpp
    src/input/input_map.cpp
    src/input/camera_control_system.cpp
    src/assets/gltf_loader.cpp
    src/assets/model_spawner.cpp
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

# tinygltf もヘッダオンリー（phase14 ③-1）。実装の実体化は gltf_loader.cpp でのみ行う。
#
# ★ find_package(tinygltf CONFIG) は**使えない**。このポートは CMake の config ファイルを
#   生成せず、tiny_gltf.h を include/ へ置くだけだから。
#   vcpkg 自身が案内する使い方も find_path で、Stb と同じ「ヘッダの置き場を探す」流儀になる。
# ★ nlohmann-json は別パッケージとして要る。overlay port が tiny_gltf.h の
#   #include "json.hpp" を <nlohmann/json.hpp> に書き換えているため。
# ★ 内部で stb も使う（下の Stb_INCLUDE_DIR で通っている）。
#   texture.cpp が既に STB_IMAGE_IMPLEMENTATION を定義しているので、
#   実装の多重定義に注意すること（gltf_loader.cpp の冒頭コメント参照）。
find_package(nlohmann_json CONFIG REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")

find_program(glslc_executable NAMES glslc HINTS Vulkan::glslc)

# stb / tinygltf はヘッダオンリーのためインクルードディレクトリのみ追加する。
target_include_directories(sq_engine_lib PRIVATE ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS})

target_link_libraries(sq_engine_lib PUBLIC
    spdlog::spdlog
    Vulkan::Vulkan
    glfw
    glm::glm
)

# nlohmann-json は PRIVATE でよい（tiny_gltf.h を include するのは gltf_loader.cpp だけで、
# 公開ヘッダ gltf_loader.hpp には tinygltf も json も一切出てこない）。
target_link_libraries(sq_engine_lib PRIVATE nlohmann_json::nlohmann_json)
