#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace sq::graphics {

// 1インスタンス分の描画データ（phase13 ②-1 / D-5）。SSBO の配列要素になる。
//
// なぜ push constant ではないのか:
//   push constant は「1ドローに1組」しか渡せない。N体を1回の vkCmdDrawIndexed で描くと、
//   体ごとに違う値を渡す手段が無くなる。そこでフレームごとのストレージバッファに配列で置き、
//   シェーダ側から gl_InstanceIndex で引く形にする。
//
// ★ シェーダの std430 レイアウトと**完全に一致**させること。
//   std430 では vec4 と mat4 が16バイト境界に揃い、構造体自体も
//   「最大メンバのアライメント」に切り上げられる。C++側は glm の alignas で同じ結果になるが、
//   一致は static_assert で必ず確認すること（ずれると全インスタンスの表示が崩れる）。
struct InstanceData {
    glm::mat4 model;              // offset  0, 64
    glm::vec4 base_color;         // offset 64, 16
    std::uint32_t texture_index;  // offset 80,  4（bindless 配列の添字 = TextureId）
    std::uint32_t _pad[3]{};      // offset 84, 12（16バイト境界へ揃えるための詰め物）
};

static_assert(sizeof(InstanceData) == 96, "std430 のレイアウトと一致させること");
static_assert(offsetof(InstanceData, base_color) == 64, "シェーダ側のメンバ順と一致させること");
static_assert(offsetof(InstanceData, texture_index) == 80, "シェーダ側のメンバ順と一致させること");

}  // namespace sq::graphics
