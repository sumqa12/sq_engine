#pragma once

#include <cstddef>
#include <glm/glm.hpp> // ☆ IDEがなぜか使用されていない可能性があると判断している

namespace sq::graphics {

// ライト1件のGPU側レイアウト（phase15 ① / D-4）。SSBO の配列要素になる。
//
// なぜ vec4 3本に詰めるのか:
//   std430 では vec3 も **16バイト境界に揃う**ため、`vec3 position; float range;` のように
//   並べても、結局 vec4 2本ぶんの領域を消費する（詰まってはくれない）。
//   ならば最初から vec4 で持ち、余る w 成分に意味を持たせた方が、
//   パディングを数える作業そのものが消えてレイアウト事故も起きない。
//   MaterialData（phase14 ①）で emissive.w を将来用に空けてあるのと同じ考え方。
//
// なぜ種別を float の w に入れるのか:
//   `std::uint32_t type;` を別メンバとして置くと、そこから16バイトの詰め直しが発生する。
//   w に入れて **シェーダ側で int(w) として読む**方が、レイアウトが vec4 の倍数で閉じる。
//   ★ 0 / 1 という値は scene::LightType の値そのもの。片方だけ変えないこと。
//
// ★ シェーダの std430 ブロックと**完全に一致**させること
//   （InstanceData / MaterialData と同じ規則。ずれると全ライトの向きと色が壊れる）。
struct LightData {
    // xyz = ワールド位置（Directional では未使用）
    //   w = 種別（0 = Directional, 1 = Point）
    glm::vec4 position_type{0.0f};    // offset  0, 16

    // xyz = ワールド方向（★ 正規化済みであること。Point では未使用）
    //   w = 有効半径 range（Directional では未使用）
    glm::vec4 direction_range{0.0f};  // offset 16, 16

    // rgb = 色（linear）
    //   a = 強度 intensity
    glm::vec4 color_intensity{1.0f};  // offset 32, 16

    // 影の情報（phase17 ⓪-2 / D-4）。
    //   x = シャドウスロット（kNoShadowSlot = 影なし）。シェーダ側で int(x) として読む。
    //       Directional: 0（CSM。方向光の影は1つだけ）
    //       Point:       0..kMaxPointShadows-1（キューブ配列の何番目か。③-1）
    //   y = near（Point のみ。③-1）
    //   z = far （Point のみ。= range。③-1）
    //   w = 未使用
    //
    // なぜ CameraUBO::light_count.y（phase16 D-4）をやめるのか:
    //   phase17 では方向光1 + 点光源4 の最大5灯が影を落とす。uvec4 の空き成分には入りきらない。
    //   「どのライトが影を持つか」はライト自身のデータとして持たせる方が、灯数が増えても形が変わらない。
    //
    // ★ 既定値は {kNoShadowSlot, 0, 0, 0}。{0, 0, 0, 0} にすると全ライトが
    //   「スロット 0 の影を持つ」扱いになり、**全ライトに太陽の影が掛かる**。
    glm::vec4 shadow{-1.0f, 0.0f, 0.0f, 0.0f};  // offset 48, 16
};

// LightData::shadow.x の「影なし」（phase17 ⓪-2）。
// ★ シェーダ側は int(shadow.x) >= 0 で判定するので、負であれば何でもよいが、値は1つに決めておく。
inline constexpr float kNoShadowSlot = -1.0f;

static_assert(sizeof(LightData) == 64, "std430 のレイアウトと一致させること");
static_assert(offsetof(LightData, direction_range) == 16, "シェーダ側のメンバ順と一致させること");
static_assert(offsetof(LightData, color_intensity) == 32, "シェーダ側のメンバ順と一致させること");
static_assert(offsetof(LightData, shadow) == 48, "シェーダ側のメンバ順と一致させること");

}  // namespace sq::graphics
