#include <chrono>
#include <cmath>
#include <GLFW/glfw3.h>

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
    constexpr float du = 1.0f / TARGET_UPS;

    bool f11_prev = false;

    while (!renderer.should_close()) {
        sq::graphics::Window::poll_events();

        // フルスクリーン切り替え
        bool f11_now = renderer.is_key_pressed(GLFW_KEY_F11);
        if (f11_now && !f11_prev) {
            renderer.set_fullscreen(!renderer.is_fullscreen());
        }
        f11_prev = f11_now;

        steady_clock::time_point now = steady_clock::now();

        // ミリ秒
        double delta_t = duration_cast<duration<double>>(now - init).count() * 1000;
        delta_u += delta_t / time_u;
        delta_f += delta_t / time_f;

        // モーダルブロック（ウィンドウのドラッグ・最小化等）からの復帰時に
        // 借金が爆発してバーストするのを防ぐ。長い停止は「なかったこと」にして穏やかに再開する。
        constexpr double kMaxPendingUpdates = 3.0;
        constexpr double kMaxPendingFrames  = 1.0;
        delta_u = std::min(delta_u, kMaxPendingUpdates);
        delta_f = std::min(delta_f, kMaxPendingFrames);

        // 更新制限
        if (delta_u >= 1.0) {
            double delta_u_time = duration_cast<duration<double>>(now - prev_u).count() * 1000;

            // ECSの更新処理をここに追加することができます

            // オブジェクトがカメラを中心に、地面(XZ平面)と平行な円軌道を回る例。
            const sq::ecs::Entity camera_entity = registry.view<Camera>().front();

            glm::vec3 target;

            if (camera_entity.is_null()) {
                target = glm::vec3(0.0f, 1.5f, 3.0f);
            } else {
                const Camera& cam = registry.get<Camera>(camera_entity);
                target = cam.position;
            }

            constexpr float radius = 3.0f;

            registry.view<Position, Velocity>().each(
                [target](const sq::ecs::Entity &e, Position& pos, Velocity& vel) {
                    // カメラの周りを回転するように、位置を更新する
                    const float dx = pos.x - target.x;
                    const float dz = pos.z - target.z;

                    // 正規化
                    const float dist = glm::min(sqrt(dx * dx + dz * dz), radius);
                    const float nx = dx / dist;
                    const float nz = dz / dist;

                    // 接線方向 = 半径方向を90度回転
                    vel.vx = -nz * radius * du;
                    vel.vz = nx * radius * du;

                    // 位置の更新
                    pos += vel * 1.2f;
                }
            );

            delta_u--;
            prev_u = now;
        }

        // フルスクリーン中にフォーカスを失ったら描画をスキップ（更新は好みで継続/停止）
        const bool render_paused = renderer.is_fullscreen() && !renderer.is_focused();

        // フレーム制限
        if (!render_paused && delta_f >= 1.0) {
            double delta_f_time = duration_cast<duration<double>>(now - prev_f).count() * 1000;
            renderer.draw_frame(registry);

            delta_f--;
            prev_f = now;
        }

        if (render_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // busyループでCPUを焼かない
        }

        init = now;
    }
}

int main() {
    sq::ecs::Registry registry;

    constexpr int kEntityCount = 10;
    float angle = 0.0f;
    for (int i = 0; i < kEntityCount; ++i) {
        float radius = 3.0f;
        angle += 360.0 / kEntityCount;
        float x = radius * sin(angle);
        float z = radius * cos(angle);

        const sq::ecs::Entity e = registry.create();
        registry.add<Position>(e, {x, 0.0f, z});
        registry.add<Velocity>(e, {0.0f, 0.0f, 0.0f});
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
        registry.add<Camera>(camera_entity, Camera{
            .position = {0.0f, 1.5f, 3.0f},
            .target = glm::vec3(0.0f, 0.0f, 6.0f),
        });
    }

    loop(registry);
    return 0;
}
