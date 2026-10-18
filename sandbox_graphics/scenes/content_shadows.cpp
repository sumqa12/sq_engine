#include "scene_content.hpp"

#include <cstddef>

#include <glm/gtc/quaternion.hpp>

#include "scene_helpers.hpp"
#include "sq/graphics/material_data.hpp"
#include "sq/scene/hierarchy.hpp"   // WorldTransform（phase14 ④）
#include "sq/scene/material.hpp"
#include "sq/scene/mesh_handle.hpp"
#include "sq/scene/transform.hpp"

using namespace sq::scene;

namespace sandbox {

namespace {

// 影を受ける面のマテリアル。
//   ★ 鏡面が強いと影のコントラストが読みにくい。無地の粗い誘電体にする。
MaterialId add_floor_material(SceneBuildContext& ctx) {
    return ctx.renderer.materials().add(
        sq::graphics::MaterialData{
            .base_color = {0.8, 0.8, 0.8, 1},
            .metallic = 0.0,
            .roughness = 0.9,
        }, { .albedo = ctx.assets.white_texture }
    );
}

// plane を1枚置く（Transform / WorldTransform / MeshHandle / Material の4点セット）。
sq::ecs::Entity add_plane(SceneBuildContext& ctx, const glm::vec3& position, const glm::quat& rotation,
                          const glm::vec3& scale, const MaterialId material) {
    const sq::ecs::Entity e = ctx.registry.create();
    ctx.registry.add<Transform>(e, Transform(position, rotation, scale));
    ctx.registry.add<WorldTransform>(e, WorldTransform{});
    ctx.registry.add<MeshHandle>(e, MeshHandle{ ctx.assets.plane_mesh });
    ctx.registry.add<Material>(e, Material{ .id = material, .transparent = false });
    return e;
}

}  // namespace

void add_ground(SceneBuildContext& ctx, const float size) {
    // ★ plane_mesh は XZ 平面・法線 +Y・一辺 1（mesh_registry.cpp）。
    // ★ kFloorY は格子（y = -9..+9）の下。y = 0 に置くと格子を貫通する。
    // ★ phase17 D-5 でシャドウのカリングを NONE にしたので、床は自分自身の深度も書く。
    //   bias（法線オフセット。②-10）が足りないと床に縞が出る。
    add_plane(ctx, glm::vec3(0, kFloorY, 0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
              glm::vec3(size, 1, size), add_floor_material(ctx));
}

void add_alpha_mask_boards(SceneBuildContext& ctx) {
    // ---- phase17 ①-5 の検証シーン: アルファマスクの板 ----
    //
    // glTF の MASK モデルはアセットに無いので、αチャンネル付きの PNG で MASK マテリアルを手で作る。
    // ★ Material::transparent は false（MASK は不透明パスで描き、discard で抜く）。
    const TextureId texture = ctx.renderer.textures().load("./textures/hsr_icon_01/Blade 1.png", VK_FORMAT_R8G8B8A8_SRGB);
    const MaterialId material = ctx.renderer.materials().add(
        sq::graphics::MaterialData{
            .base_color = {1, 1, 1, 1},
            .roughness = 0.8f,
            .alpha_cutoff = 0.5f,
        }, { .albedo = texture }
    );

    // X 軸まわりに ±90° 回して縦に立てる。+90° は表（法線）が +Z、-90° は -Z を向く。
    // ★ 表向きと裏向きを交互に置き、D-5（シャドウのカリング NONE）で両方が影を落とすことを確かめる。
    constexpr float kBoardZ = 15;
    const glm::quat face_plus_z  = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
    const glm::quat face_minus_z = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0));
    for (int i = 1; i <= 4; ++i) {
        add_plane(ctx, glm::vec3(static_cast<float>(i) * 10.0f, 10, kBoardZ),
                  i % 2 == 0 ? face_plus_z : face_minus_z, glm::vec3(10), material);
    }
}

void add_shadow_playground(SceneBuildContext& ctx) {
    auto& registry = ctx.registry;

    // ---- phase17 ②-10 / ③-9 の検証シーン: シャドウアクネ・ピーターパンニング・点光源の影 ----
    const MaterialId floor_material = add_floor_material(ctx);

    // 床（500）。カスケードの全段（影の最大距離 100）が床の上に収まる広さ。
    add_plane(ctx, glm::vec3(0, kFloorY, 0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
              glm::vec3(500, 1, 500), floor_material);

    // 天井（50）。表（法線）を下へ向けるため X 軸まわりに 180° 回す。
    //   ★ 点光源の上方向（+Y 面）の影を受ける面。床だけだと +Y 面の向きの誤りに気付けない。
    constexpr float kCeilingHeight = 20;
    add_plane(ctx, glm::vec3(10, kFloorY + kCeilingHeight, 10),
              glm::angleAxis(glm::radians(180.0f), glm::vec3(1, 0, 0)),
              glm::vec3(50, 1, 50), floor_material);

    const MaterialId material = ctx.renderer.materials().add(
        sq::graphics::MaterialData{
            .base_color = {1.0, 0.4, 0.2, 1},
        }, { .albedo = ctx.assets.white_texture }
    );

    // 立方体と球の 5×5（x, z = 0..40、間隔 10）。左右に往復させる（phase17 ③-9）。
    constexpr float kGridSize = 10;
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 5; ++j) {
            const auto entity = registry.create();
            const glm::vec3 origin(static_cast<float>(i) * kGridSize, kFloorY + 5, static_cast<float>(j) * kGridSize);
            registry.add<Transform>(entity, Transform(
                origin,
                glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                glm::vec3(2)
            ));
            registry.add<WorldTransform>(entity, WorldTransform{});
            // 左右に往復させる（影の追従と残像の確認用）。
            //   位相を格子の位置でずらし（隣と 1.5 rad 差）、全部が揃って動かないようにする。
            //   ★ 振幅 3・間隔 10・大きさ 2 なので、隣どうしが逆向きに振れても重ならない。
            registry.add<Oscillator>(entity, Oscillator{
                .origin = origin,
                .amplitude = 3.0f,
                .speed = 1.0f,
                .phase = static_cast<float>(i + j) * 1.5f,
            });
            registry.add<MeshHandle>(entity, MeshHandle{
                .id = (i * 5 + j) % 2 == 0 ? ctx.assets.cube_mesh : ctx.assets.sphere_mesh });
            registry.add<Material>(entity, Material{ .id = material, .transparent = false });
        }
    }
}

}  // namespace sandbox
