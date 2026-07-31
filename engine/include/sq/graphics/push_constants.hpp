#pragma once

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
    glm::mat4 model;       // offset  0, 64 bytes（頂点変換。VERTEX で使う）
    glm::vec4 base_color;  // offset 64, 16 bytes（色 tint。FRAGMENT で使う）
};

static_assert(sizeof(PushConstants) == 80,
              "PushConstants のサイズがシェーダの push_constant ブロックと一致しません");
static_assert(sizeof(PushConstants) % 4 == 0,
              "push constant のサイズは4の倍数である必要があります");

}  // namespace sq::graphics
