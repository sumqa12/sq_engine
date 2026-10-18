#version 450
#extension GL_EXT_nonuniform_qualifier : require   // ★ material.glsl（textures[]）の include より前に置く

// 点光源のシャドウパス用のフラグメントシェーダ（phase17 ③-5 / D-9）。
//
// 深度として「透視投影の深度」ではなく「ライトからの線形距離 / far」を書く（D-9）。
//   本パスの比較が length(v) / far だけで済み、bias をワールド単位で考えられる。
//
// ★ gl_FragDepth を書くと、ラスタライザが付ける depth bias（vkCmdSetDepthBias）は**無効**になる
//   （bias はラスタライズした深度に足されるもので、frag が上書きすれば消える）。
//   点光源の bias は本パスの比較側（triangle.frag の sample_point_shadow）だけで取る。
// ★ gl_FragDepth を書くシェーダは早期深度テストが効かなくなる（全画素で frag が走る）。
//   シャドウマップは 512² と小さいので、まず気にしなくてよい。
// ★ out（カラー出力）は宣言しない（shadow.frag と同じ。カラーアタッチメントが無い）。
// ★ このファイルを足したら **CMake を再実行すること**。

#include "material.glsl"
#include "light.glsl"         // lights[pc.indices.y] からライトの位置と far を引く
#include "shadow_push.glsl"   // pc.indices.y = このパスのライトの lights_ 内の添字

// ★ shadow_point.vert の out と location・型・flat を一致させること。
layout(location = 0) in vec2 frag_uv;
layout(location = 1) in flat uint frag_material_index;
layout(location = 2) in vec3 frag_world_pos;

void main() {
    //   アルファマスク
    MaterialData m = materials[frag_material_index];
    vec4 albedo_sample = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv);
    if (is_masked_out(m, albedo_sample.a * m.base_color.a)) { discard; }

    LightData light = lights[pc.indices.y];
    gl_FragDepth = length(frag_world_pos - light.position_type.xyz) / light.shadow.z;
    //   ★ gl_FragDepth は discard しない全経路で書くこと（書かない経路があると値が未定義になる）。
    //   ★ far（shadow.z）を超えた距離は 1.0 を超えるが、ビューポートの深度範囲 [0, 1] にクランプされる。
    //     本パスの比較側で「ref > 1 なら影なし」にしておけば問題ない（③-7）。
}
