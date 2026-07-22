#pragma once

#include <glm/glm.hpp>
#include <glm/trigonometric.hpp>

namespace sq::scene {

// 操作スキーム（このエンティティをどう動かすか）。将来 Orbit 等を追加する。
enum class ControlScheme {
    FreeFly,  // WASDで平行移動、マウス（右ドラッグ）で視線回転
};

// エンティティの操作方法と速度パラメータ。FreeFly の向き（yaw/pitch）状態もここに持つ。
// 初期化時に yaw/pitch はカメラの初期 forward から設定するとよい。
struct Controller {
    ControlScheme scheme = ControlScheme::FreeFly;
    float move_speed = 3.0f;                   // 単位/秒
    float mouse_sensitivity = 0.0025f;         // ラジアン/ピクセル
    float yaw = 0.0f;                          // 水平角（FreeFlyの向き）
    float pitch = 0.0f;                        // 垂直角。±約89°にクランプする
};

// 操作対象フラグ（タグ）。今このエンティティが入力を受ける。
// 付け替えることで操作対象を切り替えられる（データメンバは持たない）。
struct ControlTarget {};

}  // namespace sq::scene
