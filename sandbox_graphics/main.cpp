#include <chrono>
#include <cmath>
#include <random>
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/details/registry.h>

#include "sq/ecs/registry.hpp"
#include "sq/ecs/system_scheduler.hpp"
#include "sq/graphics/renderer.hpp"
#include "sq/input/camera_control_system.hpp"
#include "sq/input/input_manager.hpp"
#include "sq/input/input_map.hpp"
#include "sq/scene/transform.hpp"
#include "sq/scene/camera.hpp"
#include "sq/scene/controller.hpp"
#include "sq/scene/position.hpp"
#include "sq/scene/velocity.hpp"

using namespace sq::scene;
using namespace std::chrono;

#define TARGET_FPS 240.0f
#define TARGET_UPS 30.0f

static void loop(sq::ecs::Registry &registry) {
    auto renderer = sq::graphics::Renderer(800, 600, "sq_engine sandbox_graphics");

    // InputManager を生成し、Renderer 経由でコールバックを配線する (phase9)
    sq::input::InputManager input;
    renderer.set_key_callback([&](int k, int a){ input.on_key(k, a != GLFW_RELEASE); });
    renderer.set_mouse_button_callback([&](int b, int a){ input.on_mouse_button(b, a != GLFW_RELEASE); });
    renderer.set_cursor_pos_callback([&](double x, double y){ input.on_cursor_pos(x, y); });
    renderer.set_scroll_callback([&](double x, double y){ input.on_scroll(x, y); });

    // キーコンフィグとシステムスケジューラを用意する（phase10プラン F）
    sq::input::InputMap input_map = sq::input::InputMap::default_map();
    sq::ecs::SystemScheduler scheduler;
    scheduler.emplace_control_system<sq::input::CameraControlSystem>();
    //   （将来）scheduler.emplace_system<...>();  // 入力に依存しない更新処理

    steady_clock::time_point init = steady_clock::now();
    constexpr double time_f = 1000 / TARGET_FPS;
    constexpr double time_u = 1000 / TARGET_UPS;
    double delta_f = 0;
    double delta_u = 0;

    steady_clock::time_point prev_f = steady_clock::now();
    steady_clock::time_point prev_u = steady_clock::now();
    constexpr float du = 1.0f / TARGET_UPS;

    std::vector<sq::ecs::Entity> cameras;
    registry.view<Camera>().each([&](sq::ecs::Entity e, Camera) {
        cameras.push_back(e);
    });

    while (!renderer.should_close()) {
        sq::graphics::Window::poll_events();

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

            // 入力処理
            // 生キー問い合わせを Action ベースへ置き換える（phase10プラン F）
            if (input_map.is_pressed(input, sq::input::Action::Quit)) { break; }

            if (input_map.is_pressed(input, sq::input::Action::ToggleFullscreen)) {
                renderer.set_fullscreen(!renderer.is_fullscreen());
            }

            renderer.set_cursor_captured(!input_map.is_down(input, sq::input::Action::CursorEnable));

            // カメラ切り替え
            if (input_map.is_pressed(input, sq::input::Action::SwitchCamera)) {

                int active = 0;
                if (sq::ecs::Entity active_controllable = get_active_controllable_camera(registry)
                        ; active_controllable.is_null()) {
                    set_active_controllable_camera(registry, cameras[0]);
                    active = 0;
                } else {
                    for (int i = 0; i < cameras.size(); i++) {
                        if (cameras[i] == active_controllable) {
                            active = i;
                            break;
                        }
                    }
                }

                int next = active + 1;
                if (next >= cameras.size()) {
                    next = 0;
                }

                printf("アクティブカメラ : %d\n", next);
                set_active_controllable_camera(registry, cameras[next]);
            }

            scheduler.update(registry, input, input_map, du);  // du = 1/TARGET_UPS 秒

            input.new_frame(); // 入力の消費

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

sq::ecs::Entity create_camera(sq::ecs::Registry& registry,
        ControlScheme scheme,
        float x, float y, float z,
        float tx, float ty, float tz) {
    const sq::ecs::Entity camera_entity = registry.create();

    // 少し高い位置から原点を見下ろすカメラ。高さ(y)は旋回中も維持される。
    auto position = glm::vec3(x, y, z);
    auto target = glm::vec3(tx, ty, tz);

    registry.add<Camera>(camera_entity,
        Camera {
            .position = position,
            .target = target,
        }
    );

    // このカメラを操作対象にする（FreeFly）:
    auto forward = glm::normalize(target - position);
    registry.add<Controller>(camera_entity,
        Controller {
            .scheme = scheme,
            .yaw = atan2(forward.z, forward.x),
            .pitch = atan2(forward.y, sqrt(forward.x * forward.x + forward.z * forward.z))
        }
    );

    return camera_entity;
}

int main() {
    sq::ecs::Registry registry;

    constexpr int kEntityCount = 10;
    float angle = 0.0f;
    for (int i = 0; i < kEntityCount; ++i) {
        float radius = 3.0f;
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
        angle += 360.0 / kEntityCount;
    }

    {
        sq::ecs::Entity camera_1 = create_camera(
            registry, ControlScheme::FreeFly,
            0.0f, 1.5f, 3.0f, 0.0f,0.0f,0.0f);

        // このカメラを描画対象にする（phase10プラン D-3）
        // または、set_active_camera
        registry.add<ActiveCamera>(camera_1, {});

        // このカメラを操作対象にする
        // または、set_controllable_camera
        registry.add<ControlTarget>(camera_1, {});

        // 1. 非決定的な乱数シードを取得
        std::random_device rd;

        // 2. メルセンヌ・ツイスタの乱数エンジンを初期化
        std::mt19937 gen(rd());

        // 3. 0.0 から 1.0 の範囲で一様に分布させる実数分布を設定
        std::uniform_real_distribution dis(-10.0f, 10.0f);

        // 後9個のカメラ
        for (int i = 0; i < 9; ++i) {
            int r_scheme = rand() % 2;
            auto scheme = ControlScheme::FreeFly;
            switch (r_scheme) {
                case 0: scheme = ControlScheme::FreeFly; break;
                case 1: scheme = ControlScheme::Orbit; break;
                default: ;
            }

            float x = dis(gen);
            float y = dis(gen);
            float z = dis(gen);

            create_camera(
                registry, scheme,
                x, y, z, 0.0f, 0.0f, 0.0f);
        }
    }

    loop(registry);
    return 0;
}
