#include "sq/ecs/registry.hpp"
#include "sq/graphics/renderer.hpp"
#include "sq/scene/transform.hpp"
#include <glm/gtc/matrix_transform.hpp>

#include "sq/scene/camera.hpp"

int main() {
    sq::ecs::Registry registry;

    constexpr int kEntityCount = 5;
    for (int i = 0; i < kEntityCount; ++i) {
        const sq::ecs::Entity e = registry.create();
        registry.add<sq::scene::Transform>(e, sq::scene::Transform{glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(i) * 0.1f, 0.0f, 0.0f))});
    }

    {
        const sq::ecs::Entity camera_entity = registry.create();
        registry.add<sq::scene::Camera>(camera_entity, sq::scene::Camera{});
    }

    auto renderer = sq::graphics::Renderer(800, 600, "sq_engine sandbox_graphics");
    renderer.run(registry);
    return 0;
}
