#ifndef SQ_INSTANCE_GLSL
#define SQ_INSTANCE_GLSL

// インスタンスごとのデータ（set=0 binding=1）。phase17 ⓪-1 で各シェーダから括り出した。
//
// ★ C++ 側 graphics::InstanceData（engine/include/sq/graphics/instance_data.hpp、80 バイト）と対。
// ★ 使うシェーダ: triangle.vert / shadow.vert（③ で shadow_point.vert も）
// ★ C++ 側のディスクリプタの stageFlags は VERTEX のみ。frag から読むなら Renderer 側も直すこと。

// 型の定義。struct 自体はメモリ上の配置を持たないので layout 修飾子は付けない
// （付けると glslang が "useless application of layout qualifier" を出す）。
struct InstanceData {
    mat4 model;
    uint material_index;
};

// ★ std430 はブロック側に付ける。末尾の可変長配列はブロックの最後のメンバにしか置けない。
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

#endif
