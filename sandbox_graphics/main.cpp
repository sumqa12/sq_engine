#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <GLFW/glfw3.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/details/registry.h>

#include "sq/assets/gltf_loader.hpp"    // glTF 読み込み（phase14 ③）
#include "sq/assets/model_spawner.hpp"  // LoadedModel → エンティティ展開（phase14 ③）
#include "sq/ecs/registry.hpp"
#include "sq/ecs/system_scheduler.hpp"
#include "sq/graphics/renderer.hpp"
#include "sq/input/camera_control_system.hpp"
#include "sq/input/input_manager.hpp"
#include "sq/input/input_map.hpp"
#include "sq/scene/transform.hpp"
#include "sq/scene/camera.hpp"
#include "sq/scene/controller.hpp"
#include "sq/scene/hierarchy.hpp"        // Parent / WorldTransform（phase14 ④）
#include "sq/scene/light.hpp"
#include "sq/scene/material.hpp"
#include "sq/scene/mesh_handle.hpp"
#include "sq/scene/transform_system.hpp" // ワールド行列の伝播（phase14 ④）

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
    double one_second = 0;

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

static sq::ecs::Entity create_camera(sq::ecs::Registry& registry,
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

// phase15 ①: 方向光を1つ作る（D-2）。
//
// ★ Light は位置も向きも持たない。位置は WorldTransform::matrix[3]、
//   向きは -matrix[2]（-Z 前方）から Renderer が取り出す。そのため
//   **Transform / WorldTransform / Light の3点セット**で生成すること。
//   WorldTransform を付け忘れると TransformSystem の対象から外れ、
//   黙って「原点・無回転のライト」として扱われる（落ちないので気付きにくい）。
//
// direction は「光が進む向き」。真上から差す光なら (0, -1, 0)。
static sq::ecs::Entity create_directional_light(sq::ecs::Registry& registry,
                                                const glm::vec3& direction,
                                                const glm::vec3& color,
                                                float intensity) {
    const sq::ecs::Entity entity = registry.create();

    // -Z が direction を向く回転を作る。
    //   mat3 は「列が x/y/z 軸」なので、+Z 軸には direction の逆向きを入れる。
    //   ★ glm::quatLookAt を使わないのは、それが gtx（実験的拡張）にあり
    //     GLM_ENABLE_EXPERIMENTAL の定義を要求するため。基底を手で組む方が素直で、
    //     「-Z 前方とは何か」も式の形で残る。
    const glm::vec3 axis_z = -glm::normalize(direction);

    // ★ 参照の上方向が z 軸とほぼ平行だと cross がゼロになり、回転が NaN になる。
    //   真上・真下からの光は「ほぼ平行」そのものなので、ここは必ず分岐が要る。
    const glm::vec3 reference_up = (std::abs(axis_z.y) > 0.99f)
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);

    const glm::vec3 axis_x = glm::normalize(glm::cross(reference_up, axis_z));
    const glm::vec3 axis_y = glm::cross(axis_z, axis_x);

    // 方向光は位置を使わないが、Transform は回転を運ぶ器として必要。
    registry.add<Transform>(entity,
        Transform(glm::vec3(0.0f), glm::quat(glm::mat3(axis_x, axis_y, axis_z)), glm::vec3(1.0f)));
    registry.add<WorldTransform>(entity, WorldTransform{});
    registry.add<Light>(entity,
        Light {
            .type = LightType::Directional,
            .color = color,
            .intensity = intensity,
        }
    );

    return entity;
}

// phase15 ①: 点光源を1つ作る。向きは使わないので回転は単位quat。
static sq::ecs::Entity create_point_light(sq::ecs::Registry& registry,
                                          const glm::vec3& position,
                                          const glm::vec3& color,
                                          float intensity,
                                          float range) {
    const sq::ecs::Entity entity = registry.create();

    registry.add<Transform>(entity,
        Transform(position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)));
    registry.add<WorldTransform>(entity, WorldTransform{});
    registry.add<Light>(entity,
        Light {
            .type = LightType::Point,
            .color = color,
            .intensity = intensity,
            .range = range,
        }
    );

    return entity;
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

    // ---- phase14 ①: マテリアルを起動時にまとめて作る ----
    //
    // phase13 までは体ごとに base_color を持たせていたが、per-instance からマテリアルへ
    // 追い出したので「色差はマテリアルを複数作って表現する」形になる（D-1）。
    //
    // kMaterialCount 個（8〜16 程度）のマテリアルを作り、materials に積む。
    // 不透明用と半透明用の両方が要る（transparent は Material コンポーネント側のフラグだが、
    // a < 1.0 の base_color は MaterialData 側に要るため、実体を分けて作る必要がある）。
    constexpr int kMaterialCount = 16;
    std::vector<MaterialId> opaque_materials;
    std::vector<MaterialId> transparent_materials;
    for (int i = 0; i < kMaterialCount; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(kMaterialCount - 1);
        const TextureId tex = textures.empty() ? default_texture : textures[i % textures.size()];
        opaque_materials.push_back(renderer.materials().add(sq::graphics::MaterialData{
            .base_color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)}, tex));
        transparent_materials.push_back(renderer.materials().add(sq::graphics::MaterialData{
            .base_color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)}, tex));
    }
    //   ★ テクスチャの巡回は「マテリアルごと」になる。phase13 では体ごとに添字が変わっていたが、
    //     マテリアル数ぶんしか変わらなくなる。bindless / nonuniformEXT の検証としては
    //     kMaterialCount を textures.size() 以上にしておけば従来どおり機能する。

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
    constexpr int kGridSide = 10;
    constexpr float kSpacing = 2.0f;
    // 何体に1体を半透明にするか。半透明はインスタンス化されず1体1ドローなので、
    // ここを小さくするとドローコール数が半透明の体数に支配され、
    // 「不透明がバッチ化されている」ことが見えにくくなる。
    // 格子の中心が原点に来るようにするオフセット
    constexpr float kGridOrigin = -0.5f * static_cast<float>(kGridSide - 1) * kSpacing;

    int index = 0;
    for (int gx = 0; gx < kGridSide; ++gx) {
        for (int gy = 0; gy < kGridSide; ++gy) {
            for (int gz = 0; gz < kGridSide; ++gz) {
                constexpr int kTransparentEvery = 16;
                const sq::ecs::Entity e = registry.create();

                // 格子内の正規化座標（0..1）。色の決定にも使う。
                const float u = static_cast<float>(gx) / static_cast<float>(kGridSide - 1);
                const float v = static_cast<float>(gy) / static_cast<float>(kGridSide - 1);
                const float w = static_cast<float>(gz) / static_cast<float>(kGridSide - 1);

                // 回転は体ごとにランダム。TRS 合成と per-instance の model が
                // 正しく行き渡っているかを見るため（全部同じ向きだとずれに気付けない）。
                const glm::vec3 axis = glm::normalize(
                    glm::vec3(axis_dis(gen), axis_dis(gen), axis_dis(gen)) + glm::vec3(0.001f));

                registry.add<Transform>(e, Transform(
                    glm::vec3(kGridOrigin + static_cast<float>(gx) * kSpacing,
                              kGridOrigin + static_cast<float>(gy) * kSpacing,
                              kGridOrigin + static_cast<float>(gz) * kSpacing),
                    glm::angleAxis(angle_dis(gen), axis),
                    glm::vec3(0.35f + 0.25f * v))   // 高さでスケールを変える
                );

                // ★ WorldTransform をここで付ける。
                registry.add<WorldTransform>(e, WorldTransform{});
                //   伝播システムの中で後付けすると View::each() の反復中に
                //   アーキタイプ移動が起きてストレージが壊れる（D-4）。
                //   「描画対象には生成時に必ず付ける」を規約にすること。

                // 3体に1体を板にする。メッシュが2種類なので、不透明パスの
                // vkCmdDrawIndexed は（ソートが効いていれば）2回に収まるはず。
                registry.add<MeshHandle>(e, MeshHandle{ .id = (index % 3 == 0) ? plane_mesh : cube_mesh });

                const bool is_transparent = (index % kTransparentEvery == 0);

                // 「格子座標 → base_color」を「格子座標 → マテリアル選択」に変える。
                //   phase13 では u/v/w からなめらかなグラデーションを作り、
                //   「per-instance データがずれると縞・まだらになる」ことを検証に使っていた。
                //   マテリアル経由でも、格子座標から決定的に選べば同じ検証が成立する
                //   （段階は kMaterialCount 段に粗くなるが、ずれれば模様が崩れるのは同じ）。
                //
                const auto pick = static_cast<std::size_t>((u + v + w) / 3.0f * (kMaterialCount - 1));
                const MaterialId material_id = is_transparent
                    ? transparent_materials[pick]
                    : opaque_materials[pick];

                registry.add<Material>(e, Material{ .id = material_id, .transparent = is_transparent });
                //   ★ u/v/w は Transform の位置・スケールでまだ使っているので消さないこと。

                ++index;
            }
        }
    }

    // ---- phase14 ③: glTF モデルの読み込みと配置 ----
    // ★ assets/models/ の中身が、ビルド後に実行ファイルの隣の models/ へコピーされる
    //   （sandbox_graphics/CMakeLists.txt の POST_BUILD）。パスは実行時CWD基準。
    std::vector<sq::assets::LoadedModel> models;
    models.reserve(2);
    models.emplace_back(sq::assets::load_gltf("models/Duck.glb",
        renderer.meshes(), renderer.textures(), renderer.materials())
    );
    models.emplace_back(sq::assets::load_gltf("models/Box.glb",
        renderer.meshes(), renderer.textures(), renderer.materials())
    );

    for (int j = 0; j < 5; ++j) {
        for (int i = 0; i < models.size(); i++) {
            // モデル全体をまとめて動かすための空の親を1つ作る（④ の階層の使いどころ）
            auto& model = models[i];
            const sq::ecs::Entity model_root = registry.create();
            registry.add<Transform>(model_root, Transform(
                glm::vec3(0.0f, i * 10, j * 10), glm::quat(1,0,0,0), glm::vec3(2)));
            registry.add<WorldTransform>(model_root, WorldTransform{});

            std::vector<sq::ecs::Entity> model_entities = sq::assets::spawn_model(registry, model, model_root);
        }
    }
    //   ★ 検証順序（③-6）。いま assets/models にあるのは Box.glb のみ:
    //     1. Box.glb（無地・バイナリ形式）→ 形が出るか・**裏返っていないか**  ← 現在ここ
    //     2. BoxTextured               → UV が上下反転していないか（glTF の UV 原点は左上）
    //     3. SimpleMeshes              → ノードの入れ子が反映されるか
    //     4. .gltf（JSON形式）          → テキスト形式も読めるか（.bin / 画像の相対参照を含む）
    //     5. 65536 頂点超のモデル       → uint32 インデックスで崩れないか
    //     6. matrix 形式のノードを持つモデル
    //   ★ 同時に「組み込み cube / plane が従来どおり表示されること」も毎回見ること
    //     （巻き順を CCW に変えた影響がこちらに出る）。

    {
        sq::ecs::Entity camera_1 = create_camera(
            registry, ControlScheme::FreeFly,
            0.0f, 1.5f, 3.0f, 0.0f,0.0f,0.0f);

        // このカメラを使用して描画する（phase10プラン D-3）
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

    // ---- 光源（phase15 ①）----
    //
    // ★ 確認の順序（①-8）: まず点光源だけで「陰影が出るか」「距離で減衰するか」を見て、
    //   通ってから方向光を足すと、向きの取り違え（-Z 前方）を切り分けやすい。
    {
        // 太陽。方向光なので位置は使われず、向きだけが効く。
        create_directional_light(registry,
            glm::vec3(0.0f, -1.0f, 0.0f),  // 光が進む向き。下向き成分が大きいほど真上からの光になる
            glm::vec3(1.0f, 0.95f, 0.9f),    // ★ linear 値（⓪ の約束。sRGB の値をそのまま入れない）
            2.0f);

        // 点光源。格子（kGridSide × kSpacing = 18 四方）の内側に散らし、
        // 距離減衰と range の打ち切りが見えるようにする。
        //
        // ★ 色を強く振ってあるのは確認のため。赤い面が右側だけ、緑が左側だけ…と分かれて出れば
        //   「位置が正しく渡っている」ことが一目で分かる。全部が同じ色に染まるなら、
        //   位置の取り出し（matrix[3]）か減衰の式を疑う。
        // ★ intensity が方向光より1桁大きいのは 1/d² で急速に落ちるため
        //   （距離5で 1/25 になる）。方向光と同じ 2.0 ではまず見えない。
        create_point_light(registry, glm::vec3(-6.0f, 2.0f, -6.0f), glm::vec3(1.0f, 0.2f, 0.2f), 40.0f, 15.0f);
        create_point_light(registry, glm::vec3( 6.0f, 2.0f, -6.0f), glm::vec3(0.2f, 1.0f, 0.2f), 40.0f, 15.0f);
        create_point_light(registry, glm::vec3(-6.0f, 2.0f,  6.0f), glm::vec3(0.2f, 0.4f, 1.0f), 40.0f, 15.0f);
        create_point_light(registry, glm::vec3( 0.0f, 8.0f, 12.0f), glm::vec3(1.0f, 1.0f, 1.0f), 60.0f, 25.0f);
    }

    // ---- メインループ ----
    loop(registry, renderer);
    return 0;
}
