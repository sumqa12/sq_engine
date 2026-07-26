#pragma once

#include <glm/glm.hpp>
#include <glm/trigonometric.hpp>

namespace sq::scene {

// 操作スキーム（このエンティティをどう動かすか）。
enum class ControlScheme {
    FreeFly,  // WASDで平行移動、マウス（右ドラッグ）で視線回転。position が主、target が従
    Orbit,    // 注視点(Camera::target)を中心に周回。WASDで注視点をパン、ホイールで距離ズーム。target が主、position が従
};

// エンティティの操作方法と速度パラメータ。向き（yaw/pitch）状態もここに持つ（両スキーム共通）。
// 初期化時に yaw/pitch はカメラの初期 forward から設定するとよい。
struct Controller {
    ControlScheme scheme = ControlScheme::FreeFly;
    float move_speed = 3.0f;                   // 単位/秒
    float mouse_sensitivity = 0.0025f;         // ラジアン/ピクセル
    float yaw = 0.0f;                          // 水平角
    float pitch = 0.0f;                        // 垂直角。±約89°にクランプする

    // -- Orbit 用（scheme == Orbit のときのみ使う） --
    float orbit_distance = 5.0f;               // 注視点からカメラまでの距離
    float zoom_speed = 0.5f;                   // ホイール1ノッチあたりの距離変化
    float min_orbit_distance = 1.0f;           // ズームのクランプ範囲（下限）
    float max_orbit_distance = 50.0f;          // ズームのクランプ範囲（上限）
};

// 操作対象フラグ（タグ）。今このエンティティが入力を受ける。
// 付け替えることで操作対象を切り替えられる（データメンバは持たない）。
struct ControlTarget {};

}  // namespace sq::scene
