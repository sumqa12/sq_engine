#pragma once

#include <cstdint>

namespace sq::scene {

// 影に関する定数のうち、scene 層（CameraUBO）と graphics 層（Renderer）の両方が使うもの（phase17 ⓪-4）。
//
// なぜ Renderer ではなくここに置くのか:
//   ②-4 で CameraUBO に cascade_view_projection[kCascadeCount] を持たせる。camera.hpp は
//   graphics を知らない層なので、Renderer のメンバ定数を参照できない。両方から見える場所に置く。

// カスケードシャドウの段数（phase17 D-7）。
//   ⓪-4 では描画リストの個数（Renderer::cascade_lists_）にだけ使う。②-4 で CameraUBO にも使う。
// ★ シェーダ側の SQ_CASCADE_COUNT（②-4 で shaders/include/camera.glsl に足す）と**必ず揃える**こと。
//   片方だけ変えると、CameraUBO の大きさが C++ とシェーダで食い違う（static_assert では気付けない）。
inline constexpr std::uint32_t kCascadeCount = 4;

}  // namespace sq::scene
