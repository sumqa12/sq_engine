#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace sq::graphics {

// マテリアル1件のGPU側レイアウト（phase14 ① / D-1）。SSBO の配列要素になる。
//
// なぜ InstanceData から切り出すのか:
//   phase13 までは base_color / texture_index を体ごとの InstanceData に埋めていた。
//   その形では metallic / roughness / emissive / 法線マップ… と項目が増えるたびに
//   **全インスタンスぶんデータが重複する**。同じ見た目の体が何百あっても
//   マテリアルの実体は1つでよいので、共有の配列へ追い出して添字だけを持たせる。
//
// ★ シェーダの std430 ブロックと**完全に一致**させること（InstanceData と同じ規則）。
//   vec4 は16バイト境界に揃う。後半の float/uint は4個並べてちょうど16バイトになっており、
//   これによって構造体全体が 48 バイト（16の倍数）に収まっている。
struct MaterialData {
    glm::vec4 base_color{1.0f};      // offset  0, 16  テクスチャに乗算する色（a は不透明度）
    glm::vec4 emissive{0.0f};        // offset 16, 16  自己発光（w は未使用。将来 strength）
    float metallic = 0.0f;           // offset 32,  4
    float roughness = 1.0f;          // offset 36,  4
    float alpha_cutoff = 0.5f;       // offset 40,  4  （MASK モード用。将来使う）
    std::uint32_t albedo_index = 0;  // offset 44,  4  bindless テクスチャ配列の添字

    // 将来（phase15）: normal_index / metallic_roughness_index / occlusion_index / emissive_index
    // ★ ここに uint を足すときは、合計が16の倍数になるようパディングを調整すること。
};

static_assert(sizeof(MaterialData) == 48, "std430 のレイアウトと一致させること");
static_assert(offsetof(MaterialData, emissive) == 16, "シェーダ側のメンバ順と一致させること");
static_assert(offsetof(MaterialData, albedo_index) == 44, "シェーダ側のメンバ順と一致させること");

}  // namespace sq::graphics
