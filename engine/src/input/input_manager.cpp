#include "sq/input/input_manager.hpp"

namespace sq::input {

void InputManager::on_key(int key, bool pressed) {
    // GLFW は未知キーに -1 を送ることがあるので範囲外は無視する。
    if (0 <= key && key <= kMaxKeys) {
        current_[key] = pressed;
    }
}

void InputManager::on_mouse_button(int button, bool pressed) {
    if (0 <= button && button <= kMaxButtons) {
        mouse_current_[button] = pressed;
    }
}

void InputManager::on_cursor_pos(double x, double y) {
    // 前回位置との差を累積する
    if (!cursor_initialized_) {
        last_cursor_x_ = x;
        last_cursor_y_ = y;
        cursor_dx_ = 0;
        cursor_dy_ = 0;
        cursor_initialized_ = true;
    } else {
        cursor_dx_ += x - last_cursor_x_;
        cursor_dy_ += y - last_cursor_y_;
        last_cursor_x_ = x;
        last_cursor_y_ = y;
    }
}

void InputManager::on_scroll(double x_offset, double y_offset) {
    // scroll_x_ += x_offset;
    scroll_y_ += y_offset;
    (void)x_offset;
}

void InputManager::new_frame() {
    previous_ = current_;
    mouse_previous_ = mouse_current_;

    // 累積デルタのリセット
    cursor_dx_ = cursor_dy_ = 0.0;
    scroll_y_ = 0.0;
}

bool InputManager::is_down(int key) const {
    return 0 <= key && key <= kMaxKeys &&
        current_[key];
}

bool InputManager::is_pressed(int key) const {
    return 0 <= key && key <= kMaxKeys &&
        current_[key] && !previous_[key];
}

bool InputManager::is_released(int key) const {
    return 0 <= key && key <= kMaxKeys &&
        !current_[key] && previous_[key];
}

bool InputManager::is_mouse_down(int button) const {
    return 0 <= button && button <= kMaxButtons &&
        mouse_current_[button];
}

bool InputManager::is_mouse_pressed(int button) const {
    return 0 <= button && button <= kMaxButtons &&
        mouse_current_[button] && !mouse_previous_[button];
}

bool InputManager::is_mouse_released(int button) const {
    return 0 <= button && button <= kMaxButtons &&
        !mouse_current_[button] && mouse_previous_[button];
}

MouseDelta InputManager::cursor_delta() const {
    return { cursor_dx_, cursor_dy_ };
}

double InputManager::scroll_delta() const {
    return scroll_y_;
}

}  // namespace sq::input
