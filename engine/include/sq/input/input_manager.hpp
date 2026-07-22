#pragma once

#include <array>
#include <cstddef>

namespace sq::input {

// このフレームのカーソル移動量（ピクセル）。
struct MouseDelta {
    double dx = 0.0;
    double dy = 0.0;
};

// キー・マウスの入力状態を保持するスナップショット管理クラス。
// GLFWヘッダには依存せず、Window から転送された (key/button/cursor/scroll) イベントを受け取るだけにする。
// キー定数（GLFW_KEY_* 等）の意味付けは呼び出し側（入力システム・main）が行う。
//
// フレームモデル: 毎フレーム先頭で new_frame() を呼び、その後 Window::poll_events() で
// コールバックが current_ を更新し、その後に問い合わせる。この順序により
// is_pressed（押した瞬間）と cursor_delta（このフレームの移動量）が正しく取れる。
class InputManager {
public:
    InputManager() = default;

    // -- Window のコールバックから呼ばれる（イベント入力） --

    // pressed = 押下 or リピート（= action != GLFW_RELEASE を呼び出し側で判定して渡す）。
    void on_key(int key, bool pressed);
    void on_mouse_button(int button, bool pressed);
    void on_cursor_pos(double x, double y);   // 前回位置との差を累積する
    void on_scroll(double x_offset, double y_offset);  // y を累積する

    // 毎フレーム先頭で呼ぶ。key/button の previous_ ← current_ スナップショットと、
    // cursor/scroll の累積デルタの 0 リセットを行う。
    void new_frame();

    // -- 問い合わせ（ループ・入力システムから） --

    [[nodiscard]] bool is_down(int key) const;      // 押されている（継続）
    [[nodiscard]] bool is_pressed(int key) const;   // 押した瞬間（current && !previous）
    [[nodiscard]] bool is_released(int key) const;  // 離した瞬間（!current && previous）

    [[nodiscard]] bool is_mouse_down(int button) const;
    [[nodiscard]] bool is_mouse_pressed(int button) const;
    [[nodiscard]] bool is_mouse_released(int button) const;

    [[nodiscard]] MouseDelta cursor_delta() const;  // このフレームのカーソル移動量
    [[nodiscard]] double scroll_delta() const;      // このフレームのホイール量（y）

private:
    static constexpr std::size_t kMaxKeys = 512;      // GLFW_KEY_LAST(348) を包含。GLFW非依存にするため固定値。
    static constexpr std::size_t kMaxButtons = 8;     // GLFW_MOUSE_BUTTON_LAST(7) を包含。

    std::array<bool, kMaxKeys> current_{};
    std::array<bool, kMaxKeys> previous_{};
    std::array<bool, kMaxButtons> mouse_current_{};
    std::array<bool, kMaxButtons> mouse_previous_{};

    // カーソル: 絶対位置の直近値と、このフレームの累積デルタ。
    double last_cursor_x_ = 0.0;
    double last_cursor_y_ = 0.0;
    bool cursor_initialized_ = false;  // 最初のイベントはデルタ 0 にするためのフラグ
    double cursor_dx_ = 0.0;
    double cursor_dy_ = 0.0;

    double scroll_y_ = 0.0;  // このフレームの累積ホイール量
};

}  // namespace sq::input
