#ifndef SQ_LIGHT_GLSL
#define SQ_LIGHT_GLSL

// ライトのデータ（set=0 binding=2）。phase17 ⓪-1 で triangle.frag から括り出した。
//
// ★ C++ 側 graphics::LightData（engine/include/sq/graphics/light_data.hpp、64 バイト）と対。
//   phase17 ⓪-2 で 4 本目の vec4 shadow を足して 48 → 64 バイトにした（D-4）。
//   ★ C++ 側だけ広げると、配列の刻み幅が 48 と 64 で食い違い、**2番目以降のライトがすべて壊れる**
//     （0番だけ正しく見えるので、太陽しか無いシーンでは気付けない）。必ず同時に直すこと。
// ★ 使うシェーダ: triangle.frag（③ で shadow_point.frag も）
// ★ C++ 側のディスクリプタの stageFlags は FRAGMENT のみ。
struct LightData {
    vec4 position_type;     // xyz = ワールド位置 / w = 種別（0 = Directional, 1 = Point。int(w) で読む）
    vec4 direction_range;   // xyz = 光の進む向き（正規化済み）/ w = range
    vec4 color_intensity;   // rgb = 色（linear）/ a = 強度
    vec4 shadow;            // x = シャドウスロット（負 = 影なし。int(x) で読む）/ y = near / z = far（Point のみ）
};

layout(std430, set = 0, binding = 2) readonly buffer LightBuffer {
    LightData lights[];
};

#endif
