#include "sq/input/input_manager.hpp"

namespace sq::input {

void InputManager::on_key(int key, bool pressed) {
    // TODO: 範囲チェック（0 <= key < kMaxKeys）の上で current_[key] = pressed。
    //   GLFW は未知キーに -1 を送ることがあるので範囲外は無視する。
    (void)key;
    (void)pressed;
}

void InputManager::on_mouse_button(int button, bool pressed) {
    // TODO: 範囲チェックの上で mouse_current_[button] = pressed。
    (void)button;
    (void)pressed;
}

void InputManager::on_cursor_pos(double x, double y) {
    // TODO: 前回位置との差を累積する:
    //   初回（!cursor_initialized_）は last=xy にしてデルタ 0、cursor_initialized_ = true。
    //   以降は cursor_dx_ += x - last_cursor_x_; cursor_dy_ += y - last_cursor_y_; last=xy。
    (void)x;
    (void)y;
}

void InputManager::on_scroll(double x_offset, double y_offset) {
    // TODO: scroll_y_ += y_offset;（x_offset は今回未使用）
    (void)x_offset;
    (void)y_offset;
}

void InputManager::new_frame() {
    // TODO: previous_ = current_; mouse_previous_ = mouse_current_;
    //   cursor_dx_ = cursor_dy_ = 0.0; scroll_y_ = 0.0;（累積デルタのリセット）
}

bool InputManager::is_down(int key) const {
    // TODO: 範囲チェックの上で return current_[key];
    (void)key;
    return false;
}

bool InputManager::is_pressed(int key) const {
    // TODO: return current_[key] && !previous_[key];（範囲チェック込み）
    (void)key;
    return false;
}

bool InputManager::is_released(int key) const {
    // TODO: return !current_[key] && previous_[key];（範囲チェック込み）
    (void)key;
    return false;
}

bool InputManager::is_mouse_down(int button) const {
    (void)button;
    return false;  // TODO: return mouse_current_[button];
}

bool InputManager::is_mouse_pressed(int button) const {
    (void)button;
    return false;  // TODO: return mouse_current_[button] && !mouse_previous_[button];
}

bool InputManager::is_mouse_released(int button) const {
    (void)button;
    return false;  // TODO: return !mouse_current_[button] && mouse_previous_[button];
}

MouseDelta InputManager::cursor_delta() const {
    return {};  // TODO: return { cursor_dx_, cursor_dy_ };
}

double InputManager::scroll_delta() const {
    return 0.0;  // TODO: return scroll_y_;
}

}  // namespace sq::input
