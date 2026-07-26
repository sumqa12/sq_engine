#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <span>

#include "sq/input/action.hpp"

namespace sq::input {

class InputManager;

// バインド先のデバイス種別。キーとマウスボタンは番号空間が別なので区別が必要。
enum class InputDevice {
    Keyboard,
    Mouse,
};

// 1つの割り当て。code は GLFW_KEY_* / GLFW_MOUSE_BUTTON_*。-1 は未割り当て。
struct InputBinding {
    InputDevice device = InputDevice::Keyboard;
    int code = -1;
};

// Action → 物理キー/ボタンの対応表（キーコンフィグ層。phase10プラン B-2）。
//
// InputManager と同じくこのヘッダはGLFW非依存を保つ。GLFW定数が現れるのは
// input_map.cpp の default_map() の中だけで、そこが「GLFW → エンジン抽象」の唯一の変換点になる。
//
// 1つのActionに複数バインドでき（例: LookEnable = 右ボタン or 左Alt）、いずれかが成立すれば真を返す。
// 固定長配列で保持し、問い合わせ経路で動的確保は行わない。
class InputMap {
public:
    static constexpr std::size_t kMaxBindingsPerAction = 4;

    InputMap() = default;

    // 既定のキーコンフィグ（WASD移動 / Space・LeftCtrl 昇降 / 右ボタン視線 / F11 / ESC）。
    [[nodiscard]] static InputMap default_map();

    // 空きスロットへ追加する（埋まっている場合は何もしない）。
    void bind(Action action, InputDevice device, int code);
    // 既存の割り当てを消してから1つだけ設定する。
    void set(Action action, InputDevice device, int code);
    // 割り当てをすべて外す。
    void clear(Action action);

    // 解決付きの問い合わせ。いずれかのバインドが成立すれば真。
    [[nodiscard]] bool is_down(const InputManager& input, Action action) const;
    [[nodiscard]] bool is_pressed(const InputManager& input, Action action) const;
    [[nodiscard]] bool is_released(const InputManager& input, Action action) const;

    // 現在の割り当ての参照（キーコンフィグ表示・保存処理で使う想定。未割り当てスロットを含む）。
    [[nodiscard]] std::span<const InputBinding> bindings_of(Action action) const;

private:
    // 添字は action_index(action)。未割り当てスロットは code = -1。
    std::array<std::array<InputBinding, kMaxBindingsPerAction>, kActionCount> bindings_{};
};

}  // namespace sq::input
