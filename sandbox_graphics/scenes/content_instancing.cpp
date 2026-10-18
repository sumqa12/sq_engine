#include "scene_content.hpp"

#include <cstddef>
#include <random>
#include <vector>

#include <glm/gtc/constants.hpp>
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

void add_instancing_grid(SceneBuildContext& ctx, const int grid_side) {
    auto& registry = ctx.registry;
    auto& renderer = ctx.renderer;
    const auto& textures = ctx.assets.icon_textures;

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
        const TextureId tex = textures.empty() ? ctx.assets.default_texture : textures[i % textures.size()];
        opaque_materials.push_back(renderer.materials().add(
            sq::graphics::MaterialData{
                .base_color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
            }, { .albedo = tex })
        );
        transparent_materials.push_back(renderer.materials().add(
            sq::graphics::MaterialData{
                .base_color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)
            }, { .albedo = tex })
        );
    }
    //   ★ テクスチャの巡回は「マテリアルごと」になる。phase13 では体ごとに添字が変わっていたが、
    //     マテリアル数ぶんしか変わらなくなる。bindless / nonuniformEXT の検証としては
    //     kMaterialCount を textures.size() 以上にしておけば従来どおり機能する。

    // 回転軸・回転角の分布
    std::uniform_real_distribution axis_dis(-1.0f, 1.0f);
    std::uniform_real_distribution angle_dis(0.0f, glm::two_pi<float>());

    // ---- phase13 検証用の大量エンティティ ----
    //
    // grid_side^3 体を等間隔の立方格子に並べる。円環ではなく立方格子にするのは、
    // カメラを回したときに視界へ入る割合が大きく変わり、フラスタムカリング（⑤）の
    // 効きが観察しやすいため。
    constexpr float kSpacing = 2.0f;
    // 格子の中心が原点に来るようにするオフセット
    const float grid_origin = -0.5f * static_cast<float>(grid_side - 1) * kSpacing;
    // 正規化座標の分母（grid_side == 1 のときの 0 除算を避ける）
    const float grid_denominator = static_cast<float>(grid_side > 1 ? grid_side - 1 : 1);

    int index = 0;
    for (int gx = 0; gx < grid_side; ++gx) {
        for (int gy = 0; gy < grid_side; ++gy) {
            for (int gz = 0; gz < grid_side; ++gz) {
                // 何体に1体を半透明にするか。半透明はインスタンス化されず1体1ドローなので、
                // ここを小さくするとドローコール数が半透明の体数に支配され、
                // 「不透明がバッチ化されている」ことが見えにくくなる。
                constexpr int kTransparentEvery = 16;
                const sq::ecs::Entity e = registry.create();

                // 格子内の正規化座標（0..1）。色の決定にも使う。
                const float u = static_cast<float>(gx) / grid_denominator;
                const float v = static_cast<float>(gy) / grid_denominator;
                const float w = static_cast<float>(gz) / grid_denominator;

                // 回転は体ごとにランダム。TRS 合成と per-instance の model が
                // 正しく行き渡っているかを見るため（全部同じ向きだとずれに気付けない）。
                const glm::vec3 axis = glm::normalize(
                    glm::vec3(axis_dis(ctx.rng), axis_dis(ctx.rng), axis_dis(ctx.rng)) + glm::vec3(0.001f));

                registry.add<Transform>(e, Transform(
                    glm::vec3(grid_origin + static_cast<float>(gx) * kSpacing,
                              grid_origin + static_cast<float>(gy) * kSpacing,
                              grid_origin + static_cast<float>(gz) * kSpacing),
                    glm::angleAxis(angle_dis(ctx.rng), axis),
                    glm::vec3(0.35f + 0.25f * v))   // 高さでスケールを変える
                );

                // ★ WorldTransform をここで付ける。
                registry.add<WorldTransform>(e, WorldTransform{});
                //   伝播システムの中で後付けすると View::each() の反復中に
                //   アーキタイプ移動が起きてストレージが壊れる（D-4）。
                //   「描画対象には生成時に必ず付ける」を規約にすること。

                // 3体に1体を板にする。メッシュが2種類なので、不透明パスの
                // vkCmdDrawIndexed は（ソートが効いていれば）2回に収まるはず。
                registry.add<MeshHandle>(e, MeshHandle{
                    .id = (index % 3 == 0) ? ctx.assets.plane_mesh : ctx.assets.cube_mesh });

                const bool is_transparent = (index % kTransparentEvery == 0);

                // 「格子座標 → base_color」を「格子座標 → マテリアル選択」に変える。
                //   phase13 では u/v/w からなめらかなグラデーションを作り、
                //   「per-instance データがずれると縞・まだらになる」ことを検証に使っていた。
                //   マテリアル経由でも、格子座標から決定的に選べば同じ検証が成立する
                //   （段階は kMaterialCount 段に粗くなるが、ずれれば模様が崩れるのは同じ）。
                const auto pick = static_cast<std::size_t>((u + v + w) / 3.0f * (kMaterialCount - 1));
                const MaterialId material_id = is_transparent
                    ? transparent_materials[pick]
                    : opaque_materials[pick];

                registry.add<Material>(e, Material{ .id = material_id, .transparent = is_transparent });

                ++index;
            }
        }
    }
}

void add_survey_cameras(SceneBuildContext& ctx) {
    auto& registry = ctx.registry;

    const sq::ecs::Entity camera_1 = create_camera(
        registry, ControlScheme::FreeFly,
        0.0f, 1.5f, 3.0f, 0.0f, 0.0f, 0.0f);

    // このカメラを使用して描画し、操作対象にする（phase10プラン D-3）。
    // ★ 後から別のカメラを set_active_controllable_camera すれば、そちらに切り替わる
    //   （add_material_balls がパネル正面のカメラでそうしている）。
    set_active_controllable_camera(registry, camera_1);

    // 後9個のカメラ。格子の内側・外側の両方に置き、カメラを切り替えたときも
    // 正しくカリングされること（⑤-4）を確認できるようにする。
    std::uniform_real_distribution camera_pos_dis(-10.0f, 10.0f);
    std::uniform_int_distribution scheme_dis(0, 1);

    for (int i = 0; i < 9; ++i) {
        const auto scheme = scheme_dis(ctx.rng) == 0 ? ControlScheme::FreeFly : ControlScheme::Orbit;

        const float x = camera_pos_dis(ctx.rng);
        const float y = camera_pos_dis(ctx.rng);
        const float z = camera_pos_dis(ctx.rng);

        create_camera(
            registry, scheme,
            x, y, z, 0.0f, 0.0f, 0.0f);
    }
}

}  // namespace sandbox
