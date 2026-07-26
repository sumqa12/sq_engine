#include "sq/input/input_map.hpp"

// 既定バインドの定義にのみGLFW定数を使う。ヘッダ側はGLFW非依存のまま保つ。
#include <GLFW/glfw3.h>

#include "sq/input/input_manager.hpp"

namespace sq::input {

InputMap InputMap::default_map() {
    InputMap map;
    // 既定のキーコンフィグを設定する（phase10プラン B-2）。
    map.set(Action::MoveForward,      InputDevice::Keyboard, GLFW_KEY_W);
    map.set(Action::MoveBackward,     InputDevice::Keyboard, GLFW_KEY_S);
    map.set(Action::MoveLeft,         InputDevice::Keyboard, GLFW_KEY_A);
    map.set(Action::MoveRight,        InputDevice::Keyboard, GLFW_KEY_D);
    map.set(Action::MoveUp,           InputDevice::Keyboard, GLFW_KEY_SPACE);
    map.set(Action::MoveDown,         InputDevice::Keyboard, GLFW_KEY_LEFT_CONTROL);
    map.set(Action::CursorEnable,       InputDevice::Keyboard,    GLFW_KEY_LEFT_ALT);
    map.set(Action::ToggleFullscreen, InputDevice::Keyboard, GLFW_KEY_F11);
    map.set(Action::Quit,             InputDevice::Keyboard, GLFW_KEY_ESCAPE);
    return map;
}

void InputMap::bind(Action action, InputDevice device, int code) {
    // bindings_[action_index(action)] を走査し、最初の未割り当て（code == -1）へ書き込む。
    // 空きが無ければ何もしない（学習用途なのでthrowしない方針）。
    for (auto& binding : bindings_[action_index(action)]) {
        if (binding.code == -1) {
            binding = InputBinding{ .device = device, .code = code };
            return;
        }
    }
}

void InputMap::set(Action action, InputDevice device, int code) {
    clear(action);
    bind(action, device, code);
}

void InputMap::clear(Action action) {
    for (auto& binding : bindings_[action_index(action)]) {
        binding = InputBinding{ .code = -1 };
    }
}

bool InputMap::is_down(const InputManager& input, Action action) const {
    // bindings_of(action) を走査し、code >= 0 のバインドについて
    // device == Keyboard なら input.is_down(code)、Mouse なら input.is_mouse_down(code)
    // を呼び、いずれかが true なら true を返す。
    //
    // 補足: is_down / is_pressed / is_released は3つとも同型の走査になる。
    // 述語を受ける private ヘルパを1本用意して共通化するとよい（重複を避ける学習ポイント）。
    for (const auto& binding : bindings_of(action)) {
        if (binding.code < 0) {
            break;
        }

        if (binding.device == InputDevice::Keyboard) {
            if (input.is_down(binding.code)) {
                return true;
            }
        } else if (binding.device == InputDevice::Mouse) {
            if (input.is_mouse_down(binding.code)) {
                return true;
            }
        }
    }

    return false;
}

bool InputMap::is_pressed(const InputManager& input, Action action) const {
    for (const auto& binding : bindings_of(action)) {
        if (binding.code < 0) {
            break;
        }

        if (binding.device == InputDevice::Keyboard) {
            if (input.is_pressed(binding.code)) {
                return true;
            }
        } else if (binding.device == InputDevice::Mouse) {
            if (input.is_mouse_pressed(binding.code)) {
                return true;
            }
        }
    }
    return false;
}

bool InputMap::is_released(const InputManager& input, Action action) const {
    for (const auto& binding : bindings_of(action)) {
        if (binding.code < 0) {
            break;
        }

        if (binding.device == InputDevice::Keyboard) {
            if (input.is_released(binding.code)) {
                return true;
            }
        } else if (binding.device == InputDevice::Mouse) {
            if (input.is_mouse_released(binding.code)) {
                return true;
            }
        }
    }
    return false;
}

std::span<const InputBinding> InputMap::bindings_of(Action action) const {
    return bindings_[action_index(action)];
}

}  // namespace sq::input
