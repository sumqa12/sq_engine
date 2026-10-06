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

// ★ ① で、本パスとシャドウパスが共有する alpha の discard 判定をここに関数として足す
//   （phase17 ①-1 / ③-5。コピーすると片方だけ直す事故が起きる）。

#endif
