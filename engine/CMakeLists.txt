add_library(sq_engine_lib STATIC
    src/ecs/archetype.cpp
    src/ecs/registry.cpp
    src/graphics/window.cpp
    src/graphics/vulkan_instance.cpp
    src/graphics/debug_messenger.cpp
    src/graphics/physical_device.cpp
    src/graphics/device.cpp
    src/graphics/swapchain.cpp
    src/graphics/render_pass.cpp
    src/graphics/graphics_pipeline.cpp
    src/graphics/command_buffers.cpp
    src/graphics/sync_objects.cpp
    src/graphics/renderer.cpp
)

target_include_directories(sq_engine_lib PUBLIC include)
target_compile_features(sq_engine_lib PUBLIC cxx_std_20)

find_package(spdlog CONFIG REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_program(glslc_executable NAMES glslc HINTS Vulkan::glslc)

target_link_libraries(sq_engine_lib PUBLIC
    spdlog::spdlog
    Vulkan::Vulkan
    glfw
    glm::glm
)
