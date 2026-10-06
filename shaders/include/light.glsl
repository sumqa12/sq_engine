#ifndef SQ_LIGHT_GLSL
#define SQ_LIGHT_GLSL

// ライトのデータ（set=0 binding=2）。phase17 ⓪-1 で triangle.frag から括り出した。
//
// ★ C++ 側 graphics::LightData（engine/include/sq/graphics/light_data.hpp、48 バイト）と対。
//   ⓪-2 で 4 本目の vec4 shadow を足して 64 バイトにする（D-4）。
// ★ 使うシェーダ: triangle.frag（③ で shadow_point.frag も）
// ★ C++ 側のディスクリプタの stageFlags は FRAGMENT のみ。
struct LightData {
    vec4 position_type;     // xyz = ワールド位置 / w = 種別（0 = Directional, 1 = Point。int(w) で読む）
    vec4 direction_range;   // xyz = 光の進む向き（正規化済み）/ w = range
    vec4 color_intensity;   // rgb = 色（linear）/ a = 強度
};

layout(std430, set = 0, binding = 2) readonly buffer LightBuffer {
    LightData lights[];
};

#endif
