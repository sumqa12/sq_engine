#ifndef SQ_SHADOW_PUSH_GLSL
#define SQ_SHADOW_PUSH_GLSL

// シャドウパスのプッシュ定数（phase17 ⓪-3 / D-3）。
//
// ★ C++ 側 graphics::ShadowPushConstants（engine/include/sq/graphics/shadow_push_constants.hpp、80 バイト）と対。
// ★ 使うシェーダ: shadow.vert（②）/ shadow_point.vert・shadow_point.frag（③）
//   本パス（triangle.*）とスカイボックスは include しないこと。
//   読まないシェーダが宣言だけしても害は無いが、「本パスは push しない」という約束が見えにくくなる。
// ★ C++ 側のレンジは stageFlags = VERTEX | FRAGMENT。vkCmdPushConstants も同じ stageFlags で呼ぶこと。
// ★ ⓪-3 の時点ではどのシェーダも include しない（宣言だけ先に置く）。
layout(push_constant) uniform ShadowPush {
    mat4  view_proj;   // ③ 点光源の「この面」の行列（② では未使用）
    uvec4 indices;     // x = カスケード番号（②）/ y = ライトの添字（③）/ z, w = 未使用
} pc;

#endif
