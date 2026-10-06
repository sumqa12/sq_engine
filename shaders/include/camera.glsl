#ifndef SQ_CAMERA_GLSL
#define SQ_CAMERA_GLSL

// カメラ UBO（set=0 binding=0）。phase17 ⓪-1 で各シェーダから括り出した。
//
// ★ C++ 側 scene::CameraUBO（engine/include/sq/scene/camera.hpp）と対。
//   メンバを変えたら、C++ 側の static_assert(sizeof(CameraUBO) == ...) も必ず直すこと。
//   ここが**唯一の定義**なので、シェーダ側で揃える場所はもうこの1か所だけ。
// ★ 使うシェーダ: triangle.vert / triangle.frag / shadow.vert / skybox.vert
//   （*.comp は前計算専用の set=0 を持つので include しないこと。binding が衝突する）
//
// ★ include ガードは #pragma once ではなく #ifndef で書く（glslang の対応状況に依存しない）。
// ★ ⓪-1 の時点では phase16 のレイアウト（160 バイト）のまま。②-4 で 400 バイトへ広げる。
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4  view_proj;
    mat4  light_view_proj;   // phase16 ①-4（②-4 で cascade_view_proj[] に置き換える）
    vec4  camera_position;   // xyz = ワールド位置
    uvec4 light_count;       // x = 有効ライト数 / y = 影を落とすライトの添字（無ければ 0xFFFFFFFF。⓪-2 で廃止）
} camera;

#endif
