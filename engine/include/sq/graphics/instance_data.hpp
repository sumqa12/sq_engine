#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace sq::graphics {

// 1インスタンス分の描画データ（phase13 ②-1 / D-5、phase14 ① で痩せた）。SSBO の配列要素になる。
//
// なぜ push constant ではないのか:
//   push constant は「1ドローに1組」しか渡せない。N体を1回の vkCmdDrawIndexed で描くと、
//   体ごとに違う値を渡す手段が無くなる。そこでフレームごとのストレージバッファに配列で置き、
//   シェーダ側から gl_InstanceIndex で引く形にする。
//
// phase14 ①: base_color / texture_index を廃止し、material_index だけを持つ形にした。
//   見た目のパラメータは graphics::MaterialData（set=1 の共有 SSBO）へ移した。
//   96 → 80 バイト。4096 体ぶんで 384KiB → 320KiB になり、
//   **マテリアル項目をいくら増やしてもこのサイズは変わらない**のが要点。
//
// ★ シェーダの std430 レイアウトと**完全に一致**させること。
//   std430 では vec4 と mat4 が16バイト境界に揃い、構造体自体も
//   「最大メンバのアライメント」に切り上げられる。C++側は glm の alignas で同じ結果になるが、
//   一致は static_assert で必ず確認すること（ずれると全インスタンスの表示が崩れる）。
struct InstanceData {
    glm::mat4 model{};                // offset  0, 64  ワールド変換（phase14 ④ 以降は WorldTransform 由来）
    std::uint32_t material_index = 0; // offset 64,  4（MaterialBuffer の添字 = MaterialId）
    std::uint32_t _pad[3]{};          // offset 68, 12（mat4 に合わせ16バイト境界へ揃えるための詰め物）
};

static_assert(sizeof(InstanceData) == 80, "std430 のレイアウトと一致させること");
static_assert(offsetof(InstanceData, material_index) == 64, "シェーダ側のメンバ順と一致させること");

}  // namespace sq::graphics
