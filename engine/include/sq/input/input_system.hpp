#pragma once

#include "sq/ecs/registry.hpp"
#include "sq/input/input_manager.hpp"

namespace sq::input {

// ControlTarget + Controller を持つカメラエンティティに入力を適用する（FreeFly操作）。
// dt は前回からの経過秒。GLFWキー/マウス定数（GLFW_KEY_W, GLFW_MOUSE_BUTTON_RIGHT 等）で問い合わせる。
// 正式な System 抽象が未整備のため、現状は free 関数として提供する（将来 System へ昇格）。
// registry は const 参照で受ける（本エンジンの慣習: view().each() は const registry 経由で
// コンポーネントを変更する。Renderer::draw_frame と同じ）。
void update_camera_control(const sq::ecs::Registry& registry, const InputManager& input, float dt);

}  // namespace sq::input
