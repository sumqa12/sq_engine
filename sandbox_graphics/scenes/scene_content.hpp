#pragma once

#include <glm/glm.hpp>

#include "scene.hpp"

// 検証テーマごとの「中身」（phase17 ④の前に main.cpp から移動）。
//
// ★ 中身の関数は**照明を作らない**。照明（add_sun / add_point_light_grid）とカメラは、
//   シーン（scene_registry.cpp）の側で組み合わせる。全部入りのシーンで太陽が2つにならないようにするため。
//   例外は add_material_balls / add_survey_cameras のカメラで、これは元の main() の並びを保つため中身に含めている。
namespace sandbox {

// 床・影の検証用の空間の高さ。phase13 の格子（y = -9..+9）の下に置く。
inline constexpr float kFloorY = -9.5f;

// ---- phase13 / phase14 ①: インスタンシング・フラスタムカリング・bindless ----

// grid_side^3 体を等間隔の立方格子に並べる（原点中心・間隔 2）。
//   3体に1体を板、16体に1体を半透明にする。マテリアルは 16 種（不透明 / 半透明それぞれ）を巡回する。
//   ★ grid_side は段階的に上げること:
//       10 -> 1000 体（まず正しく描けることを確認する）
//       26 -> 17576 体（kMaxInstances = 16384 超え。遠い影から欠けていく境界テスト。phase17 ⓪-4）
void add_instancing_grid(SceneBuildContext& ctx, int grid_side);

// 原点を向くカメラ1台（アクティブ・操作対象）と、格子の内外にランダムに置いたカメラ9台（C キーで巡回）。
//   格子の内側・外側の両方に置き、カメラを切り替えたときも正しくカリングされること（phase13 ⑤-4）を確認できるようにする。
void add_survey_cameras(SceneBuildContext& ctx);

// ---- phase14 ③: glTF ----

// Duck / ABeautifulGame / Box を読み込み、x 軸上に並べる（ctx.state.models が所有する）。
void add_gltf_models(SceneBuildContext& ctx);

// ---- phase15 ②: マテリアルボール ----

// roughness 6段 × metallic 2段 の計12体を y = 10 の高さに並べ、パネル正面のカメラをアクティブにする。
void add_material_balls(SceneBuildContext& ctx);

// ---- phase16 / phase17: 影 ----

// 床（plane を size 倍。y = kFloorY）を1枚置く。無地の粗い誘電体（鏡面が強いと影のコントラストが読みにくい）。
void add_ground(SceneBuildContext& ctx, float size);

// phase17 ①: アルファマスク（MASK）の板を4枚、表向きと裏向きを交互に立てる（x = 10..40, z = 15）。
//   ★ 確認: 板の透明な部分が描かれない（本パスの discard）／床の影がアイコンの形に抜ける
//     （シャドウパスの discard）／表向きと裏向きの両方が影を落とす（シャドウのカリング NONE。D-5）。
void add_alpha_mask_boards(SceneBuildContext& ctx);

// phase17 ②-10 / ③-9: 影の検証場。床（500）・天井（50）・左右に往復する立方体と球の 5×5（x, z = 0..40）。
//   ★ 天井は点光源の上方向（+Y 面）の影の確認用。
void add_shadow_playground(SceneBuildContext& ctx);

// ---- 照明 ----

// 影を落とす太陽を1つ作り、ctx.state.sun に入れる（loop() が Y 軸まわりに回す）。
void add_sun(SceneBuildContext& ctx, const glm::vec3& direction, float intensity);

// 点光源を 2 列 × 5 個（x = 5, 15 / z = 5..45 / y = 4）に並べる。色はランダム。
//   1列目の 5 個が影を落とす（kMaxPointShadows = 4 なので、5個目は警告が出て無視される。③-9 の確認用）。
//   ★ intensity が大きいのは IBL に埋もれないようにするため（③-8 の注記）。
void add_point_light_grid(SceneBuildContext& ctx);

}  // namespace sandbox
