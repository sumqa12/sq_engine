#include "scene.hpp"

#include <array>
#include <filesystem>
#include <string>

#include <spdlog/spdlog.h>

#include "scene_content.hpp"
#include "scene_helpers.hpp"
#include "sq/graphics/mesh_registry.hpp"
#include "sq/scene/camera.hpp"
#include "sq/scene/controller.hpp"

using namespace sq::scene;

namespace sandbox {

namespace {

// ---- シーンの組み立て ----
//
// ★ 中身（scene_content.hpp）・照明・カメラの組み合わせ方だけを書く。中身そのものは content_*.cpp。
// ★ カメラは最後に set_active_controllable_camera したものが使われる。

// phase17 ③ までの main() と同じ全部入り。並び順も元の main() と同じ。
void build_all(SceneBuildContext& ctx) {
    // 格子は全部入りだと他の検証の邪魔になるので小さくしてある（phase13 の検証は instancing シーンで行う）。
    constexpr int kGridSide = 2;
    add_instancing_grid(ctx, kGridSide);
    add_gltf_models(ctx);
    add_survey_cameras(ctx);
    add_sun(ctx, glm::vec3(0.5f, -1.0f, 0.5f), 2.0f);
    add_point_light_grid(ctx);
    add_material_balls(ctx);   // ★ パネル正面のカメラがアクティブになる（survey カメラの後に呼ぶこと）
    add_shadow_playground(ctx);
}

// phase13 / phase14 ①: インスタンシング・フラスタムカリング・bindless。
//   ★ C キーで10台のカメラを巡回し、格子の内外どこから見てもカリングが正しいことを確かめる。
//   ★ kGridSide を 26 にすると kMaxInstances（16384）を超え、遠い影から欠けていく（phase17 ⓪-4）。
void build_instancing(SceneBuildContext& ctx) {
    constexpr int kGridSide = 10;
    add_instancing_grid(ctx, kGridSide);
    add_survey_cameras(ctx);
    add_sun(ctx, glm::vec3(0.3f, -1.0f, 0.5f), 2.0f);
}

// phase14 ③: glTF の読み込み・階層・マテリアル（phase15 ③ の法線マップも含む）。
void build_gltf(SceneBuildContext& ctx) {
    add_gltf_models(ctx);
    add_sun(ctx, glm::vec3(0.3f, -1.0f, 0.5f), 2.0f);

    const sq::ecs::Entity camera = create_camera(
        ctx.registry, ControlScheme::FreeFly,
        2.0f, 4.0f, 18.0f,
        2.0f, 0.0f, 0.0f);
    set_active_controllable_camera(ctx.registry, camera);
}

// phase15 ② / phase16 ②: Cook-Torrance と IBL（roughness × metallic のマテリアルボール）。
//   ★ 太陽1つだけにしてある（ライトを増やすと、どの変化が roughness 由来なのか切り分けられない）。
void build_materials(SceneBuildContext& ctx) {
    add_sun(ctx, glm::vec3(0.3f, -1.0f, 0.5f), 2.0f);
    add_material_balls(ctx);   // パネル正面のカメラがアクティブになる
}

// phase17 ①: アルファマスク（MASK）の本パスとシャドウパスの discard。
void build_alpha_mask(SceneBuildContext& ctx) {
    constexpr float kGroundSize = 100.0f;
    add_ground(ctx, kGroundSize);
    add_alpha_mask_boards(ctx);
    add_sun(ctx, glm::vec3(0.3f, -1.0f, 0.5f), 2.0f);

    // 板（x = 10..40, z = 15）を手前（-Z 側）から見下ろす。太陽は -Z 側から差すので、影は板の向こう側へ落ちる。
    const sq::ecs::Entity camera = create_camera(
        ctx.registry, ControlScheme::FreeFly,
        25.0f, 15.0f, -20.0f,
        25.0f, 0.0f, 20.0f);
    set_active_controllable_camera(ctx.registry, camera);
}

// phase17 ② / ③: カスケードシャドウ（F3 で段の色分け）と点光源のキューブシャドウ。
void build_shadows(SceneBuildContext& ctx) {
    add_shadow_playground(ctx);
    add_sun(ctx, glm::vec3(0.5f, -1.0f, 0.5f), 2.0f);
    add_point_light_grid(ctx);

    // 5×5（x, z = 0..40）を手前（-Z 側）から見下ろす。
    const sq::ecs::Entity camera = create_camera(
        ctx.registry, ControlScheme::FreeFly,
        20.0f, 10.0f, -20.0f,
        20.0f, kFloorY, 20.0f);
    set_active_controllable_camera(ctx.registry, camera);
}

// phase17 ④: エンティティの生成・破棄（Duck が湧いては消える／F5 で ABeautifulGame をアンロード）。
//   ★ 1秒ごとにエンティティ数とアセット数がログに出る（SceneState::log_counts）。
void build_lifecycle(SceneBuildContext& ctx) {
    constexpr float kGroundSize = 100.0f;
    add_ground(ctx, kGroundSize);
    add_sun(ctx, glm::vec3(0.3f, -1.0f, 0.5f), 2.0f);
    add_lifecycle_test(ctx);

    // 湧く範囲（原点付近 ±8）と ABeautifulGame（x = 20 付近）の両方が入るように、手前の高い位置から見下ろす。
    const sq::ecs::Entity camera = create_camera(
        ctx.registry, ControlScheme::FreeFly,
        8.0f, 15.0f, 30.0f,
        8.0f, 0.0f, 0.0f);
    set_active_controllable_camera(ctx.registry, camera);
}

// シーンの一覧。★ 足すときは、ここに1行と build_* を1つ足すだけでよい。
constexpr std::array kScenes = {
    SceneDefinition{ "all",        "phase17 ③ までの検証を全部入りで（既定）",                  build_all },
    SceneDefinition{ "instancing", "phase13: 1000 体の格子でインスタンシング・カリング（C でカメラ巡回）", build_instancing },
    SceneDefinition{ "gltf",       "phase14: glTF（Duck / ABeautifulGame / Box）",              build_gltf },
    SceneDefinition{ "materials",  "phase15-16: マテリアルボール（Cook-Torrance / IBL）",        build_materials },
    SceneDefinition{ "alpha_mask", "phase17 ①: アルファマスクの板とその影",                     build_alpha_mask },
    SceneDefinition{ "shadows",    "phase17 ②③: カスケードシャドウ（F3）と点光源のキューブシャドウ", build_shadows },
    SceneDefinition{ "lifecycle",  "phase17 ④: エンティティの湧きと消滅（F5 でモデルをアンロード）", build_lifecycle },
};

}  // namespace

CommonAssets register_common_assets(sq::graphics::Renderer& renderer) {
    CommonAssets assets;

    // phase12 手順2: 使うメッシュを登録し、MeshId を受け取る。
    // 同じ MeshId を何体が参照してもGPUバッファは1組しか作られない（レジストリのキャッシュ）。
    assets.cube_mesh = sq::graphics::add_cube_mesh(renderer.meshes());
    assets.plane_mesh = sq::graphics::add_plane_mesh(renderer.meshes());
    // phase15 ②: roughness の連続変化を読むための球（マテリアルボール検証用）。
    assets.sphere_mesh = sq::graphics::add_sphere_mesh(renderer.meshes());

    // phase12 手順4: 使うテクスチャを登録し、TextureId を受け取る。
    // 同じパスを2回 load しても実際のロードは1回だけ（レジストリのキャッシュ）。
    const std::string dir_path = "./textures/hsr_icon_01"; // 対象のディレクトリ
    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        assets.icon_textures.push_back(renderer.textures().load(entry.path().string(), VK_FORMAT_R8G8B8A8_SRGB));
    }
    assets.default_texture = renderer.textures().default_texture();
    assets.white_texture = renderer.textures().white_texture();

    return assets;
}

std::span<const SceneDefinition> scene_definitions() {
    return kScenes;
}

const SceneDefinition* find_scene(const std::string_view name) {
    for (const auto& scene : kScenes) {
        if (scene.name == name) {
            return &scene;
        }
    }
    return nullptr;
}

void log_scene_list() {
    spdlog::info("シーンの一覧（sandbox_graphics.exe <シーン名>。省略時は {}）:", kDefaultSceneName);
    for (const auto& scene : kScenes) {
        spdlog::info("  {:<12} {}", scene.name, scene.description);
    }
}

}  // namespace sandbox
