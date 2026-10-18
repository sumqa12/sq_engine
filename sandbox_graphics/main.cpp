#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <string_view>
#include <thread>
#include <vector>
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "scenes/scene.hpp"              // 検証用シーンの選択（phase17 ④の前に main() から分離）
#include "scenes/scene_helpers.hpp"      // update_oscillators（phase17 ③-9）
#include "sq/ecs/registry.hpp"
#include "sq/ecs/system_scheduler.hpp"
#include "sq/graphics/renderer.hpp"
#include "sq/input/camera_control_system.hpp"
#include "sq/input/input_manager.hpp"
#include "sq/input/input_map.hpp"
#include "sq/scene/transform.hpp"
#include "sq/scene/camera.hpp"
#include "sq/scene/controller.hpp"
#include "sq/scene/transform_system.hpp" // ワールド行列の伝播（phase14 ④）

using namespace sq::scene;
using namespace std::chrono;

#define TARGET_FPS 240.0f
#define TARGET_UPS 30.0f

// phase12 手順2: メッシュ登録に MeshId が必要になったため、Renderer の生成を main へ移し、
// エンティティ生成より前にアセットを登録できるようにした（renderer は main が所有する）。
// sun: phase16 ①-9。時間で回す方向光（影の向きの検証用）。null なら回さない。
static void loop(sq::ecs::Registry &registry, sq::graphics::Renderer &renderer, sq::ecs::Entity sun) {
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
    double one_second = 0;

    steady_clock::time_point prev_f = steady_clock::now();
    steady_clock::time_point prev_u = steady_clock::now();
    constexpr float du = 1.0f / TARGET_UPS;
    constexpr float kSunAngularSpeed = 0.2f;
    float elapsed = 0.0f;  // 更新 tick の経過秒（Oscillator 用。描画レートに依存させない）

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
        one_second += delta_t / 1000;

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

            if (input_map.is_pressed(input, sq::input::Action::ToggleShadowDebug)) {
                renderer.set_shadow_debug(!renderer.shadow_debug());
            }

            renderer.set_cursor_captured(!input_map.is_down(input, sq::input::Action::CursorEnable));

            // カメラ切り替え
            // ★ カメラが1台も無いシーンもあり得るので、空なら何もしない（cameras[0] で落ちないように）。
            if (!cameras.empty() && input_map.is_pressed(input, sq::input::Action::SwitchCamera)) {

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

                spdlog::info("アクティブカメラ : {}", next);
                set_active_controllable_camera(registry, cameras[next]);
            }

            scheduler.update(registry, input, input_map, du);  // du = 1/TARGET_UPS 秒

            // 太陽を Y 軸まわりに回す（kSunAngularSpeed = 0.2 rad/s 程度）。
            if (!sun.is_null()) {
                auto& t = registry.get<Transform>(sun);
                t.set_rotation(glm::angleAxis(kSunAngularSpeed * du, glm::vec3(0, 1, 0)) * t.rotation());
            }
            //   ★ 左から掛ける（ワールドの Y 軸まわり）。右から掛けるとライトのローカル軸まわりになり、
            //     仰角ごと回って太陽が地平線の下へ潜る。
            //   ★ 更新レート側（ここ）で回す。描画レート側で回すと FPS で回転速度が変わる。
            //   ★ 確認: 影が「太陽と反対側へ」「物体の高さに比例した長さで」伸び、滑らかに回ること。
            //     逆向きに伸びるなら light_dir の符号、長さが変なら正射影の範囲を疑う（①-9）。

            // phase17 ③-9: Oscillator を持つエンティティを X 軸方向に往復させる（scenes/scene_helpers.cpp）。
            elapsed += du;
            sandbox::update_oscillators(registry, elapsed);

            input.new_frame(); // 入力の消費

            delta_u--;
            prev_u = now;
        }

        // フルスクリーン中にフォーカスを失ったら描画をスキップ（更新は好みで継続/停止）
        const bool render_paused = renderer.is_fullscreen() && !renderer.is_focused();

        // フレーム制限
        if (!render_paused && delta_f >= 1.0) {
            double delta_f_time = duration_cast<duration<double>>(now - prev_f).count() * 1000;

            // ワールド行列の伝播を draw_frame の**直前**に毎フレーム1回走らせる。
            TransformSystem::update(registry);
            //   ★ 更新レート（TARGET_UPS）側ではなく描画レート側に置く理由:
            //     draw_frame が読むのは WorldTransform なので、描画のたびに最新化されていないと
            //     フレーム間で1テンポ遅れた行列で描くことになる。
            //   ★ SystemScheduler へ載せない理由: scheduler.update は入力系のシステムを回す場所で、
            //     呼ばれる頻度が描画と一致しない。将来 Scheduler が描画前フェーズを持ったら移す。
            renderer.draw_frame(registry);

            delta_f--;
            prev_f = now;
        }

        if (one_second >= 1.0) {
            // spdlog::info("総エンティティ数: {}", registry.entity_count());
            one_second--;
        }

        if (render_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // busyループでCPUを焼かない
        }

        init = now;
    }
}

// 使い方: sandbox_graphics.exe [シーン名]
//   シーン名を省略すると sandbox::kDefaultSceneName（all）。
//   "list" を渡すとシーンの一覧を表示して終了する。シーンの中身は scenes/ 以下（scene.hpp の説明を参照）。
//   ★ テクスチャ・モデル・シェーダは実行時 CWD 基準の相対パスで開く。exe の隣を作業ディレクトリにして起動すること。
int main(int argc, char** argv) {
    const std::string_view scene_name = argc > 1 ? std::string_view(argv[1]) : sandbox::kDefaultSceneName;

    if (scene_name == "list") {
        sandbox::log_scene_list();
        std::cout << "Enterを押して終了" << std::endl;
        std::cin.get();
        return 0;
    }

    // ★ ウィンドウを開く前に名前を確かめる（打ち間違いで重い初期化を待たされないように）。
    const sandbox::SceneDefinition* scene = sandbox::find_scene(scene_name);
    if (scene == nullptr) {
        spdlog::error("シーン \"{}\" はありません。", scene_name);
        sandbox::log_scene_list();
        std::cout << "Enterを押して終了" << std::endl;
        std::cin.get();
        return 1;
    }

    sq::ecs::Registry registry;

    // Renderer を先に作る（メッシュ登録で得た MeshId をコンポーネントに入れるため。phase12 手順2）。
    auto renderer = sq::graphics::Renderer(800, 600, "sq_engine sandbox_graphics");

    // 全シーン共通のメッシュ・テクスチャ（cube / plane / sphere、hsr_icon_01、白・既定テクスチャ）。
    const sandbox::CommonAssets assets = sandbox::register_common_assets(renderer);

    // 非決定的な乱数シードで初期化したメルセンヌ・ツイスタ（格子の回転・カメラの位置・点光源の色）。
    std::random_device rd;
    std::mt19937 rng(rd());

    // ★ renderer より**後**に宣言すること（glTF の LoadedModel を Renderer より先に破棄するため。phase16 ③-4）。
    sandbox::SceneState state;
    sandbox::SceneBuildContext ctx{
        .registry = registry,
        .renderer = renderer,
        .assets = assets,
        .rng = rng,
        .state = state,
    };

    spdlog::info("シーン: {}（{}）", scene->name, scene->description);
    scene->build(ctx);

    loop(registry, renderer, state.sun);
    return 0;
}
