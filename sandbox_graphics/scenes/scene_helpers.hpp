#pragma once

#include <glm/glm.hpp>

#include "sq/ecs/entity.hpp"
#include "sq/ecs/registry.hpp"
#include "sq/scene/controller.hpp"

// どのシーンでも使う小道具（phase17 ④の前に main.cpp から移動）。
namespace sandbox {

// X 軸方向に往復させるエンティティの印（phase17 ③-9 の検証用。sandbox 専用のコンポーネント）。
//   update_oscillators が position = origin + (amplitude * sin(speed * t + phase), 0, 0) にする。
//   ★ 点光源の影が物体に追従し、**残像が残らない**こと（キャスタ0件の面も毎フレームクリアしていること）の確認に使う。
struct Oscillator {
    glm::vec3 origin{0.0f};  // 往復の中心（生成時の位置）
    float amplitude = 3.0f;  // 振幅（ワールド単位）
    float speed = 1.0f;      // 角速度（rad/s）
    float phase = 0.0f;      // 位相（物体ごとにずらして、全部が揃って動かないようにする）
};

// Oscillator を持つエンティティを動かす。elapsed は更新 tick の経過秒（描画レートに依存させない）。
// ★ loop() の更新レート側で毎 tick 呼ぶ。
void update_oscillators(sq::ecs::Registry& registry, float elapsed);

// 操作可能なカメラを1つ作る（Camera + Controller）。yaw / pitch は target への向きから決める。
// ★ ActiveCamera / ControlTarget は付けない。使うカメラは呼び出し側で
//   sq::scene::set_active_controllable_camera を呼んで決めること。
sq::ecs::Entity create_camera(sq::ecs::Registry& registry,
                              sq::scene::ControlScheme scheme,
                              float x, float y, float z,
                              float tx, float ty, float tz);

// phase15 ①: 方向光を1つ作る（D-2）。
//
// ★ Light は位置も向きも持たない。位置は WorldTransform::matrix[3]、
//   向きは -matrix[2]（-Z 前方）から Renderer が取り出す。そのため
//   **Transform / WorldTransform / Light の3点セット**で生成すること。
//   WorldTransform を付け忘れると TransformSystem の対象から外れ、
//   黙って「原点・無回転のライト」として扱われる（落ちないので気付きにくい）。
//
// direction は「光が進む向き」。真上から差す光なら (0, -1, 0)。
// cast_shadows: phase16 ①-9。影を落とすか（Light::cast_shadows へそのまま入れる）。
//   ★ 既定引数にしないこと。「影を出したいライト」と「色を確認するライト」を
//     呼び出し側で明示的に書き分けるための引数（D-4 で自動選択を採らなかった理由と同じ）。
sq::ecs::Entity create_directional_light(sq::ecs::Registry& registry,
                                         const glm::vec3& direction,
                                         const glm::vec3& color,
                                         float intensity,
                                         bool cast_shadows);

// phase15 ①: 点光源を1つ作る。向きは使わないので回転は単位quat。
//   ★ create_directional_light と同じく既定引数にしないこと（影を出すライトを呼び出し側で明示する。D-4）。
sq::ecs::Entity create_point_light(sq::ecs::Registry& registry,
                                   const glm::vec3& position,
                                   const glm::vec3& color,
                                   float intensity,
                                   float range,
                                   bool cast_shadows);

}  // namespace sandbox
