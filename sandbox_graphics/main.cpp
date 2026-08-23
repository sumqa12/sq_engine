#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <GLFW/glfw3.h>

#include <glm/gtc/constants.hpp>
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
#include "sq/scene/material.hpp"
#include "sq/scene/mesh_handle.hpp"

using namespace sq::scene;
using namespace std::chrono;

#define TARGET_FPS 240.0f
#define TARGET_UPS 30.0f

// phase12 手順2: メッシュ登録に MeshId が必要になったため、Renderer の生成を main へ移し、
// エンティティ生成より前にアセットを登録できるようにした（renderer は main が所有する）。
static void loop(sq::ecs::Registry &registry, sq::graphics::Renderer &renderer) {
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
    auto position = glm::vec3(x, y, z);
    auto target = glm::vec3(tx, ty, tz);

    registry.add<Camera>(camera_entity,
        Camera {
            .position = position,
            .target = target,
        }
    );

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

    // Renderer を先に作る（メッシュ登録で得た MeshId をコンポーネントに入れるため）。
    auto renderer = sq::graphics::Renderer(800, 600, "sq_engine sandbox_graphics");

    // phase12 手順2: 使うメッシュを登録し、MeshId を受け取る。
    // 同じ MeshId を何体が参照してもGPUバッファは1組しか作られない（レジストリのキャッシュ）。
    const MeshId cube_mesh = sq::graphics::add_cube_mesh(renderer.meshes());
    const MeshId plane_mesh = sq::graphics::add_plane_mesh(renderer.meshes());

    // phase12 手順4: 使うテクスチャを登録し、TextureId を受け取る。
    // 同じパスを2回 load しても実際のロードは1回だけ（レジストリのキャッシュ）。
    std::string dir_path = "./textures/hsr_icon_01"; // 対象のディレクトリ
    std::vector<TextureId> textures;

    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        const TextureId texture = renderer.textures().load(entry.path().string());
        textures.push_back(texture);
    }
    const TextureId default_texture = renderer.textures().default_texture();

    // 1. 非決定的な乱数シードを取得
    std::random_device rd;
    // 2. メルセンヌ・ツイスタの乱数エンジンを初期化
    std::mt19937 gen(rd());
    // 3. 回転軸・回転角の分布
    std::uniform_real_distribution axis_dis(-1.0f, 1.0f);
    std::uniform_real_distribution angle_dis(0.0f, glm::two_pi<float>());

    // ---- phase13 検証用の大量エンティティ ----
    //
    // kGridSide^3 体を等間隔の立方格子に並べる。円環ではなく立方格子にするのは、
    // カメラを回したときに視界へ入る割合が大きく変わり、フラスタムカリング（⑤）の
    // 効きが観察しやすいため。
    //
    // ★ 段階的に上げること:
    //     15 -> 3375 体（kMaxInstances = 4096 未満。まず正しく描けることを確認する）
    //     17 -> 4913 体（上限超え。描画側クランプが無いと表示が壊れる境界テスト）
    constexpr int kGridSide = 17;
    constexpr float kSpacing = 2.0f;
    // 何体に1体を半透明にするか。半透明はインスタンス化されず1体1ドローなので、
    // ここを小さくするとドローコール数が半透明の体数に支配され、
    // 「不透明がバッチ化されている」ことが見えにくくなる。
    constexpr int kTransparentEvery = 16;

    constexpr int kEntityCount = kGridSide * kGridSide * kGridSide;
    // 格子の中心が原点に来るようにするオフセット
    constexpr float kGridOrigin = -0.5f * static_cast<float>(kGridSide - 1) * kSpacing;

    int index = 0;
    for (int gx = 0; gx < kGridSide; ++gx) {
        for (int gy = 0; gy < kGridSide; ++gy) {
            for (int gz = 0; gz < kGridSide; ++gz) {
                const sq::ecs::Entity e = registry.create();

                // 格子内の正規化座標（0..1）。色の決定にも使う。
                const float u = static_cast<float>(gx) / static_cast<float>(kGridSide - 1);
                const float v = static_cast<float>(gy) / static_cast<float>(kGridSide - 1);
                const float w = static_cast<float>(gz) / static_cast<float>(kGridSide - 1);

                // 回転は体ごとにランダム。TRS 合成と per-instance の model が
                // 正しく行き渡っているかを見るため（全部同じ向きだとずれに気付けない）。
                const glm::vec3 axis = glm::normalize(
                    glm::vec3(axis_dis(gen), axis_dis(gen), axis_dis(gen)) + glm::vec3(0.001f));

                registry.add<Transform>(e,
                    Transform{
                        .position = {
                            kGridOrigin + static_cast<float>(gx) * kSpacing,
                            kGridOrigin + static_cast<float>(gy) * kSpacing,
                            kGridOrigin + static_cast<float>(gz) * kSpacing,
                        },
                        .rotation = glm::angleAxis(angle_dis(gen), axis),
                        .scale = glm::vec3(0.35f + 0.25f * v),  // 高さでスケールを変える
                    }
                );

                // 3体に1体を板にする。メッシュが2種類なので、不透明パスの
                // vkCmdDrawIndexed は（ソートが効いていれば）2回に収まるはず。
                registry.add<MeshHandle>(e, MeshHandle{ .id = (index % 3 == 0) ? plane_mesh : cube_mesh });

                const bool is_transparent = (index % kTransparentEvery == 0);
                // テクスチャは順に巡回させる。1回のドローの中で添字が変わるので、
                // bindless（①）と nonuniformEXT が効いていないとここが崩れる。
                const TextureId texture = textures.empty()
                    ? default_texture
                    : textures[static_cast<std::size_t>(index) % textures.size()];

                // ★ base_color を格子座標から決めるのが要点。
                //   per-instance データがずれると、なめらかなグラデーションが
                //   縞・まだらになって一目で分かる（乱数色だと気付けない）。
                registry.add<Material>(e,
                    Material{
                        .albedo = texture,
                        .base_color = glm::vec4(u, v, w, is_transparent ? 0.5f : 1.0f),
                        .transparent = is_transparent,
                    }
                );

                ++index;
            }
        }
    }

    fmt::println("エンティティ数: {} ({}^3), うち半透明: {}",
                 kEntityCount, kGridSide, (kEntityCount + kTransparentEvery - 1) / kTransparentEvery);

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

        // 後9個のカメラ。格子の内側・外側の両方に置き、カメラを切り替えたときも
        // 正しくカリングされること（⑤-4）を確認できるようにする。
        std::uniform_real_distribution camera_pos_dis(-10.0f, 10.0f);
        std::uniform_int_distribution scheme_dis(0, 1);

        for (int i = 0; i < 9; ++i) {
            const auto scheme = scheme_dis(gen) == 0 ? ControlScheme::FreeFly : ControlScheme::Orbit;

            float x = camera_pos_dis(gen);
            float y = camera_pos_dis(gen);
            float z = camera_pos_dis(gen);

            create_camera(
                registry, scheme,
                x, y, z, 0.0f, 0.0f, 0.0f);
        }
    }

    loop(registry, renderer);
    return 0;
}
