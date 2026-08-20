#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace sq::graphics {

// 描画1件ごとにシェーダへ渡す定数（phase12 手順5）。
// ディスクリプタセットを介さず、コマンドバッファに直接埋め込める小さなデータ。
//
// ★ シェーダの push_constant ブロックと**完全に一致**させること。
//   vert / frag の両方で同じメンバ順・同じ型で宣言する（片方で使わない場合も宣言は揃える）。
//   VkPushConstantRange の stageFlags も VERTEX | FRAGMENT にする（graphics_pipeline.cpp）。
//
// サイズは maxPushConstantsSize の最小保証 128 バイトに収める必要がある。
// これ以上マテリアル属性を増やすならマテリアルUBOへ移す（将来課題）。
struct PushConstants {
    glm::mat4 model;               // offset  0, 64 bytes（頂点変換。VERTEX で使う）
    glm::vec4 base_color;          // offset 64, 16 bytes（色 tint。FRAGMENT で使う）
    // phase13 ①-5: bindless テクスチャ配列の添字（= TextureId）。FRAGMENT で使う。
    // これが入ることでテクスチャの切り替えが「ディスクリプタのバインド」から
    // 「ただの整数」になり、②のインスタンシングで per-instance にできるようになる。
    std::uint32_t texture_index;   // offset 80,  4 bytes
};

// glm::mat4 / vec4 のアライメントが16なので、構造体末尾に12バイトのパディングが入り 96 になる。
// シェーダ側のブロックは 84 バイトまでしか使わないが、VkPushConstantRange が
// それ以上でも問題ない（範囲はシェーダの使用量以上であればよい）。
static_assert(sizeof(PushConstants) == 96,
              "PushConstants のサイズがシェーダの push_constant ブロックと一致しません");
static_assert(sizeof(PushConstants) % 4 == 0,
              "push constant のサイズは4の倍数である必要があります");

}  // namespace sq::graphics
