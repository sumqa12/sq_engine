#ifndef SQ_MATERIAL_GLSL
#define SQ_MATERIAL_GLSL

// マテリアルとテクスチャ（set=1）。phase17 ⓪-1 で triangle.frag から括り出した。
//
// ★ include する側で、**この include より前に**次の1行を書くこと:
//     #extension GL_EXT_nonuniform_qualifier : require
//   サイズ無しのサンプラ配列（textures[]）と nonuniformEXT に要る。
//   #extension はソースの先頭側（宣言より前）に置く決まりなので、ここには書かない。
// ★ 使うシェーダ: triangle.frag（① で shadow.frag、③ で shadow_point.frag も）
// ★ C++ 側のディスクリプタの stageFlags は binding=0 / 1 とも FRAGMENT のみ。

// bindless テクスチャ配列（phase13 ①）。添字は MaterialData の *_index。
layout(set = 1, binding = 0) uniform sampler2D textures[];

// マテリアル本体（phase14 ①、phase15 ③-3 で 64 バイト）。
// ★ C++ 側 graphics::MaterialData（engine/include/sq/graphics/material_data.hpp）と対。
//   メンバの順・型を1つでもずらすと、全マテリアルの見た目が崩れる。
struct MaterialData {
    vec4 base_color;
    vec4 emissive;
    float metallic;
    float roughness;
    float alpha_cutoff;              // 0 なら MASK 無効（phase15 ②-2 の約束）
    uint albedo_index;
    uint normal_index;
    uint metallic_roughness_index;
    uint occlusion_index;
    uint emissive_index;
};

// set=1 = 「起動後は不変なアセット」（phase14 D-2）。フレーム複製は無く、全体で1本。
layout(std430, set = 1, binding = 1) readonly buffer MaterialBuffer {
    MaterialData materials[];
};

// アルファマスク（glTF の alphaMode == "MASK"）で、この画素を捨てるべきか（phase17 ①-1 / D-6）。
//   alpha: albedo テクスチャの a × base_color.a（本パスの out_color.a と同じ値）
//   戻り値: true なら呼び出し側で discard する
//
// ★ 本パス（triangle.frag）・シャドウパス（shadow.frag）・③ の点光源（shadow_point.frag）で**共有する**。
//   コピーすると片方だけ直す事故が起きる（本パスでは抜けているのに影は四角い、など）。
// ★ discard そのものは関数の中に書かない。discard は呼び出し元のシェーダの main で書く方が、
//   「ここで画素が消える」が読み手に見える。
// ★ alpha_cutoff == 0 は「MASK 無効」（phase15 ②-2 の約束）。0 との比較を省くと、
//   alpha が 0 の BLEND（半透明）マテリアルの画素まで消える。
bool is_masked_out(MaterialData m, float alpha) {
    return m.alpha_cutoff > 0.0 && alpha < m.alpha_cutoff;
}

#endif
