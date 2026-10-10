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
// ★ phase17 ②-4 で 160 → 400 バイト（light_view_proj → cascade_view_proj[]、ほか3本追加）。

// カスケードシャドウの段数（phase17 D-7）。
// ★ C++ 側 scene::kCascadeCount（engine/include/sq/scene/shadow_constants.hpp）と**必ず揃える**こと。
//   片方だけ変えると UBO の大きさが食い違い、camera_position 以降がすべてずれる
//   （C++ 側の static_assert では検出できない）。
#define SQ_CASCADE_COUNT 4

// light_count.y のビット（デバッグフラグ。phase17 ②-9）。
// ★ C++ 側 scene::kDebugFlagCascadeColors（camera.hpp）と同じ値にすること。
#define SQ_DEBUG_CASCADE_COLORS 1u   // bit0: カスケードの段を色分けする

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4  view_proj;
    mat4  cascade_view_proj[SQ_CASCADE_COUNT];  // 各段の光源クリップ空間への変換（②-3）
    vec4  camera_position;       // xyz = ワールド位置
    vec4  camera_forward;        // xyz = 視線方向（正規化済み）。ビュー深度 = dot(p - camera_position.xyz, camera_forward.xyz)
    vec4  cascade_splits;        // 各段の**遠い側**のビュー深度（[c] が c 段目）
    vec4  cascade_texel_sizes;   // 各段の1テクセルのワールド幅（法線オフセット。D-8）
    uvec4 light_count;           // x = 有効ライト数 / y = デバッグフラグ（②-9 で bit0 = カスケード色分け）
    //   影を持つライトは LightData::shadow.x で表す（light.glsl。D-4）。
} camera;

#endif
