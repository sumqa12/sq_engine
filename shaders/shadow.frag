#version 450
#extension GL_EXT_nonuniform_qualifier : require   // ★ material.glsl（textures[]）の include より前に置く

// シャドウパス用のフラグメントシェーダ（phase17 ①-2 / D-6）。
//
// phase16 D-6 では「シャドウパスは frag を持たない（深度しか書かない）」としたが、
// アルファマスクの影を抜くために discard が要るので、frag を持たせる。
//
// ★ やることは「αテストに落ちた画素を捨てる」だけ。色は出さない。
//   out（layout(location = 0) out vec4 ...）を**宣言しないこと**。シャドウレンダーパスには
//   カラーアタッチメントが無いので、宣言すると「出力先の無い出力」になる。
// ★ 深度は固定機能が書く（gl_FragDepth は書かない）。書くと depth bias が無効になる（③ の点光源は別）。
// ★ 全キャスタがこの frag を通る（MASK でないマテリアルも）。discard を含むシェーダは
//   早期深度テストが効きにくいが、区間を分けるコストの方が大きいと判断した（D-6）。
// ★ このファイルを足したら **CMake を再実行すること**（file(GLOB ...) は構成時にしか評価されない）。

#include "material.glsl"

// ★ shadow.vert の out と location・型・flat を一致させること。
//   食い違うと uv がゼロになり、alpha が albedo の (0,0) の1点の値になる
//   （その点が透明なら**影が全部消える**）。
layout(location = 0) in vec2 frag_uv;
layout(location = 1) in flat uint frag_material_index;

void main() {
    MaterialData m = materials[frag_material_index];
    vec4 albedo_sample = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv);
    if (is_masked_out(m, albedo_sample.a * m.base_color.a)) { discard; }
}
