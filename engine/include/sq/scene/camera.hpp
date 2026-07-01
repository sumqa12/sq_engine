#pragma once

#include <glm/glm.hpp>

namespace sq::scene {

// 透視投影カメラのECSコンポーネント。位置・注視点・画角などのパラメータを保持し、
// view_projection()でview行列とprojection行列を合成して返す。
// registry.view<Camera>()で取得し、最初に見つかったものをアクティブカメラとして使う想定。
struct Camera {
    glm::vec3 position{0.0f, 0.0f, 3.0f};  // カメラの位置
    glm::vec3 target{0.0f, 0.0f, 0.0f};    // 注視点
    glm::vec3 up{0.0f, 1.0f, 0.0f};        // 上方向
    float fov_y_radians = glm::radians(45.0f);  // 垂直画角
    float near_plane = 0.1f;
    float far_plane = 10.0f;

    // view-projection行列を返す。aspectはウィンドウの (幅 / 高さ)。
    // 実装はcamera.cpp（TODO）。glm::perspectiveの深度範囲を[0,1]にするため
    // GLM_FORCE_DEPTH_ZERO_TO_ONE が必要（CMakeでプロジェクト全体に定義するのを推奨）。
    [[nodiscard]] glm::mat4 view_projection(float aspect) const;
};

// Uniform Bufferへ転送するカメラデータのGPU側レイアウト。
// シェーダーの layout(set=0, binding=0) uniform CameraUBO と一致させる。
struct CameraUBO {
    glm::mat4 view_projection;
};

}  // namespace sq::scene
