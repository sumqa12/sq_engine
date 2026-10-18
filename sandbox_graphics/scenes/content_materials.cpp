#include "scene_content.hpp"

#include <glm/gtc/quaternion.hpp>

#include "scene_helpers.hpp"
#include "sq/graphics/material_data.hpp"
#include "sq/scene/camera.hpp"
#include "sq/scene/controller.hpp"
#include "sq/scene/hierarchy.hpp"   // WorldTransform（phase14 ④）
#include "sq/scene/material.hpp"
#include "sq/scene/mesh_handle.hpp"
#include "sq/scene/transform.hpp"

using namespace sq::scene;

namespace sandbox {

void add_material_balls(SceneBuildContext& ctx) {
    auto& registry = ctx.registry;
    auto& renderer = ctx.renderer;

    // ---- phase15 ② の検証シーン: マテリアルボール ----
    //
    // roughness 6段 × metallic 2段 の計12体を並べ、Cook-Torrance の挙動を1画面で見る。
    //
    // ★ 白テクスチャ（乗算の恒等元）を渡し、base_color だけが出る状態にする。
    //   既定テクスチャ（市松模様）を使うと模様がハイライトに紛れて読めなくなる。
    // ★ 既存の格子（phase13 のカリング／インスタンシング検証用）から離れた高所に置く。
    //   **太陽光1つだけが当たる**状態にすると、roughness の違いがそのまま読み取れる。
    //   ライトを増やすと、どの変化が roughness 由来なのか切り分けられなくなる（materials シーンは太陽だけ）。
    constexpr float kPanelY         = 10.0f;  // 格子と点光源から離す高さ
    constexpr int   kRoughnessSteps = 6;      // 横方向の段数
    constexpr float kBallSpacing    = 3.0f;
    constexpr float kBallScale      = 2.0f;   // メッシュは半径 0.5 なので直径 1.0 → 2.0

    for (int row = 0; row < 2; ++row) { // row 0 = 非金属, row 1 = 金属
        const auto metallic = static_cast<float>(row);

        for (int col = 0; col < kRoughnessSteps; ++col) {
            // roughness は 0.05 → 1.0 の等間隔。
            // ★ 0 ちょうどは避ける（D 項の分母が発散するため。シェーダ側でも 0.03 にクランプ済み）。
            const float roughness = glm::mix(0.05f, 1.0f,
                static_cast<float>(col) / static_cast<float>(kRoughnessSteps - 1));

            const MaterialId material = renderer.materials().add(
                sq::graphics::MaterialData{
                    .base_color = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f),
                    .emissive   = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
                    .metallic   = metallic,
                    .roughness  = roughness,
                },
                { .albedo = ctx.assets.white_texture });

            const sq::ecs::Entity ball = registry.create();

            const float x = (static_cast<float>(col) - 0.5f * static_cast<float>(kRoughnessSteps - 1))
                          * kBallSpacing;
            const float y = kPanelY + (row == 0 ? 1.75f : -1.75f);

            registry.add<Transform>(ball, Transform(
                glm::vec3(x, y, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(kBallScale)));
            registry.add<WorldTransform>(ball, WorldTransform{});
            registry.add<MeshHandle>(ball, MeshHandle{ .id = ctx.assets.sphere_mesh });
            registry.add<Material>(ball, Material{ .id = material, .transparent = false });
        }
    }

    // パネル正面のカメラ。起動直後に②の確認ができるよう、これをアクティブ＆操作対象にする。
    //
    // ★ 他のカメラ（add_survey_cameras）の**後**に呼ぶこと。set_active_controllable_camera は
    //   他のエンティティから ActiveCamera を外して回るので、先に呼ぶと
    //   後から付けられたカメラの ActiveCamera と二重になる。
    const sq::ecs::Entity panel_camera = create_camera(
        registry, ControlScheme::FreeFly,
        0.0f, kPanelY, 18.0f,
        0.0f, kPanelY, 0.0f);
    set_active_controllable_camera(registry, panel_camera);
}

}  // namespace sandbox
