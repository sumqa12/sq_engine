#pragma once

#include <glm/glm.hpp>

namespace sq::graphics {

// シャドウパスへ渡すプッシュ定数（phase17 ⓪-3 / D-3）。このエンジンのグラフィックスパイプラインで初。
//
// ★ 使うのはシャドウパスだけだが、レンジは**全グラフィックスパイプライン**に同じものを宣言する
//   （Renderer::kPushConstantRange）。プッシュ定数レンジが違うパイプライン同士は「レイアウト互換」で
//   なくなり、パイプラインを切り替えた瞬間にバインド済みのディスクリプタセットが黙って外れる（phase16 D-3）。
// ★ 本パスのシェーダは push_constant ブロックを宣言しない。シェーダが読まないなら、
//   vkCmdPushConstants を一度も呼ばなくて問題ない。
//
// ② と ③ で渡し方が違う理由（phase16 D-3「同じ値を2経路で運ばない」）:
//   ② CSM   : カスケードの行列は本パスの frag も読むので CameraUBO に置き、ここでは**番号だけ**を渡す。
//   ③ 点光源: 面ごとの行列はシャドウパスしか読まないので、行列そのものを渡す。
//             ライトの位置と far は本パスも読むので、ここでは**添字だけ**を渡して lights[] から引く。
//
// ★ シェーダ側 shaders/include/shadow_push.glsl と対。std430 相当のレイアウト（プッシュ定数の既定）。
struct ShadowPushConstants {
    glm::mat4  view_projection{1.0f};  // offset  0, 64  ③ 点光源の「この面」の行列（② では未使用）
    glm::uvec4 indices{0u};            // offset 64, 16  x = カスケード番号（②）
                                       //                y = ライトの添字（③。lights_ 内）
                                       //                z, w = 未使用
};

// ★ プッシュ定数の保証下限は 128 バイト（maxPushConstantsSize）。超えるとレンジの作成が不正になる。
static_assert(sizeof(ShadowPushConstants) == 80, "シェーダ側のレイアウトと一致させること");
static_assert(sizeof(ShadowPushConstants) <= 128, "maxPushConstantsSize の保証下限を超えている");

}  // namespace sq::graphics
