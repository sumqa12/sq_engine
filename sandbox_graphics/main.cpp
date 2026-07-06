#include <chrono>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/details/registry.h>

#include "sq/ecs/registry.hpp"
#include "sq/graphics/renderer.hpp"
#include "sq/scene/transform.hpp"
#include "sq/scene/camera.hpp"
#include "sq/scene/position.hpp"
#include "sq/scene/velocity.hpp"

using namespace sq::scene;
using namespace std::chrono;

#define TARGET_FPS 60.0f
#define TARGET_UPS 30.0f

void loop(const sq::ecs::Registry &registry) {
    auto renderer = sq::graphics::Renderer(800, 600, "sq_engine sandbox_graphics");

    steady_clock::time_point init = steady_clock::now();
    const steady_clock::time_point start = init;  // カメラ旋回の経過時間の基準
    constexpr double time_f = 1000 / TARGET_FPS;
    constexpr double time_u = 1000 / TARGET_UPS;
    double delta_f = 0;
    double delta_u = 0;

    steady_clock::time_point prev_f = steady_clock::now();
    steady_clock::time_point prev_u = steady_clock::now();
    while (!renderer.should_close()) {
        sq::graphics::Window::poll_events();

        steady_clock::time_point now = steady_clock::now();

        // ミリ秒
        double delta_t = duration_cast<duration<double>>(now - init).count() * 1000;
        delta_u += delta_t / time_u;
        delta_f += delta_t / time_f;

        // 更新制限
        if (delta_u >= 1.0) {
            // ECSの更新処理をここに追加することができます

            // カメラが注視点(target)を中心に、地面(XZ平面)と平行な円軌道を回る例。
            // 高さ(Y)は保ったまま、経過時間に応じた角度でXZ平面上の円周に位置を置く。
            constexpr float kOrbitRadius = 3.0f;   // 旋回半径
            constexpr float kAngularSpeed = 0.5f;  // 角速度 [rad/秒]
            const auto elapsed_sec = static_cast<float>(duration_cast<duration<double>>(now - start).count());
            const float angle = kAngularSpeed * elapsed_sec;

            const sq::ecs::Entity camera_entity = registry.view<Camera>().front();
            auto& cam = registry.get<Camera>(camera_entity);
            const float height = cam.position.y - cam.target.y;  // 地面からの高さを維持
            cam.position = cam.target + glm::vec3(
                kOrbitRadius * std::sin(angle),
                height,
                kOrbitRadius * std::cos(angle)
            );

            registry.view<Transform, Position, Velocity>().each(
                [](const sq::ecs::Entity, Transform& transform, Position& pos, Velocity& vel) {
                    transform.model = glm::translate(glm::mat4(1.0f), glm::vec3(pos.x, pos.y, 0.0f));
                }
            );

            delta_u--;
            prev_u = now;
        }

        // フレーム制限
        if (delta_f >= 1.0) {
            renderer.draw_frame(registry);

            delta_f--;
            prev_f = now;
        }

        init = now;
    }
}

int main() {
    sq::ecs::Registry registry;

    constexpr int kEntityCount = 5;
    for (int i = 0; i < kEntityCount; ++i) {
        const sq::ecs::Entity e = registry.create();
        registry.add<Position>(e, {static_cast<float>(i) * 0.1f, 0.0f});
        registry.add<Velocity>(e, {0.01f, 0.0f});
        registry.add<Transform>(e,
            Transform{
                glm::translate(glm::mat4(1.0f),
                    glm::vec3())
            }
        );
    }

    {
        const sq::ecs::Entity camera_entity = registry.create();
        // 少し高い位置から原点を見下ろすカメラ。高さ(y)は旋回中も維持される。
        registry.add<sq::scene::Camera>(camera_entity, sq::scene::Camera{
            .position = {0.0f, 1.5f, 3.0f},
        });
    }

    loop(registry);
    return 0;
}
