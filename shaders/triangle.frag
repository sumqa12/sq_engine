#version 450
// TODO(phase13 ①-4): bindless 化する。以下の3点をまとめて書き換えること
//   （レイアウト・レジストリ・シェーダは足並みを揃えないと起動時に落ちる）。
//
//  1. 拡張の宣言（この #version の直後）:
//       #extension GL_EXT_nonuniform_qualifier : require
//
//  2. サンプラーを配列にする:
//       layout(set = 1, binding = 0) uniform sampler2D textures[];
//     ★ サイズを書かない（runtimeDescriptorArray）。C++側の kMaxTextures が実際の長さになる。
//
//  3. push constant に texture_index を足し、main() で添字引きする:
//       layout(push_constant) uniform PushConstants {
//           mat4 model;
//           vec4 base_color;
//           uint texture_index;
//       } pc;
//       out_color = texture(textures[nonuniformEXT(pc.texture_index)], frag_uv) * pc.base_color;
//
//     nonuniformEXT: 「同一ドロー内で添字が変わり得る」ことをコンパイラへ伝える。
//     push constant 由来なら実際にはドロー内で一様なので今は無くても動くが、
//     ②のインスタンシングで per-instance の添字になると必須になるので最初から付ける。
//
// ★ vert 側の push constant ブロックも同じ内容に揃えること（片方だけだとリンクで落ちる）。

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;
// phase12 手順3: テクスチャを set=1 へ分離する（set=0 はカメラUBO専用）。
// set=1 はマテリアル単位でバインドし直すため、binding は 0 から振り直す。
layout(set = 1, binding = 0) uniform sampler2D tex_sampler;

// phase12 手順5: マテリアルの色 tint。★ vert と宣言を完全に一致させること。
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    // テクスチャに base_color を乗算する。
    // base_color.a < 1.0 で半透明の濃さを表現できる（半透明パスのブレンドが効く）。
    out_color = texture(tex_sampler, frag_uv) * pc.base_color;
}
