#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace sq::graphics {

// Vulkan用に設定された単一のGLFWウィンドウ（GLコンテキストなし）をラップするRAIIラッパー。
// GLFWwindow*を保持し、glfwInit/glfwTerminateの処理を担当します。
class Window {
public:
    Window(std::uint32_t width, std::uint32_t height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;


    static void poll_events() ;
    void wait_while_minimized() const;
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
    [[nodiscard]] bool consume_resized_flag();
    [[nodiscard]] bool should_close() const;
    [[nodiscard]] GLFWwindow* handle() const;
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;

    // このプラットフォーム上でGLFWがサーフェスを作成するために必要なVulkanインスタンス拡張機能。
    [[nodiscard]] static const char ** required_instance_extensions();

    // フルスクリーン切り替え enabled = true でフルスクリーン化、
    // false で元のウィンドウの位置・サイズに復元。
    void set_fullscreen(bool enabled);
    [[nodiscard]] bool is_fullscreen() const;

    // ウィンドウがフォーカスされているか
    [[nodiscard]] bool is_focused() const;

    // キー入力の状態を返す (glfwGetKey) 。 main側でのF11トグル用
    [[nodiscard]] bool is_key_pressed(int key) const;

    // -- 入力コールバックの転送（InputManager への配線用） --
    // GLFWのコールバックはWindowが登録し、受け取ったイベントを以下の std::function へ転送する。
    // action は GLFW の action（GLFW_PRESS / RELEASE / REPEAT）をそのまま渡す。
    using KeyCallback = std::function<void(int key, int action)>;
    using CursorPosCallback = std::function<void(double x, double y)>;
    using MouseButtonCallback = std::function<void(int button, int action)>;
    using ScrollCallback = std::function<void(double x_offset, double y_offset)>;
    void set_key_callback(KeyCallback callback);
    void set_cursor_pos_callback(CursorPosCallback callback);
    void set_mouse_button_callback(MouseButtonCallback callback);
    void set_scroll_callback(ScrollCallback callback);

    // マウス視線用にカーソルをキャプチャする（true: 非表示＋無制限デルタ / false: 通常）。
    void set_cursor_captured(bool captured);

    // Win32 ネイティブハンドル (HWND) 。 swapchain の HMONITOR 取得に使う。
    // windows.h のヘッダ汚染を避けるためvoid*で返す。
    [[nodiscard]] void* win32_window() const;

private:
    static void focus_callback(GLFWwindow* window, int focused);  // glfwSetWindowFocusCallback で登録

    // 入力コールバック（user pointer 経由で Window* を取り出し、登録済み std::function へ転送）。
    static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void cursor_pos_callback(GLFWwindow* window, double x, double y);
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
    static void scroll_callback(GLFWwindow* window, double x_offset, double y_offset);

    GLFWwindow* window_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool resized_ = false;
    bool fullscreen_ = false;
    int windowed_x_ = 0, windowed_y_ = 0;        // 復元用のウィンドウ位置
    int windowed_width_ = 0, windowed_height_ = 0;  // 復元用のサイズ
    bool focused_ = true;

    // 転送先コールバック（未設定なら転送しない）。
    KeyCallback key_callback_;
    CursorPosCallback cursor_pos_callback_;
    MouseButtonCallback mouse_button_callback_;
    ScrollCallback scroll_callback_;
};

}  // namespace sq::graphics
