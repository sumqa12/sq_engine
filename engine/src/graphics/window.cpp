#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "sq/graphics/window.hpp"

#include <stdexcept>

#ifdef GLFW_EXPOSE_NATIVE_WIN32
    #include <windows.h>
#endif

namespace sq::graphics {
    // GLFWの初期化と終了を管理し、Vulkan用に設定されたウィンドウを作成します。
    Window::Window(std::uint32_t width, std::uint32_t height, const std::string& title) {

        constexpr int kMinWidth = 810;
        constexpr int kMinHeight = 540;

        // glfwの初期化
        if (!glfwInit()) {
            throw std::runtime_error("GLFW の初期化に失敗しました。");
        }

        // glfwWindowの初期化
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        if (width >= kMinWidth && height >= kMinHeight) {
            window_ = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);
        } else {
            window_ = glfwCreateWindow(kMinWidth, kMinHeight, title.c_str(), nullptr, nullptr);
        }

        if (!window_) {
            glfwTerminate();
            throw std::runtime_error("GLFW ウィンドウの作成に失敗しました。");
        }

        glfwSetWindowSizeLimits(window_, kMinWidth, kMinHeight, GLFW_DONT_CARE, GLFW_DONT_CARE);

        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, &Window::framebuffer_size_callback);
        glfwSetWindowFocusCallback(window_, &Window::focus_callback);
        // 入力コールバックを登録する（user pointer は上で設定済みなので共用できる）(phase9)
        glfwSetKeyCallback(window_, &Window::key_callback);
        glfwSetCursorPosCallback(window_, &Window::cursor_pos_callback);
        glfwSetMouseButtonCallback(window_, &Window::mouse_button_callback);
        glfwSetScrollCallback(window_, &Window::scroll_callback);
    }

    // GLFWの終了とウィンドウの破棄を行います。
    Window::~Window() {
        if (window_) {
            glfwDestroyWindow(window_);
        }
        glfwTerminate();
    }

    // ウィンドウリサイズ時の処理
    void Window::framebuffer_size_callback(GLFWwindow* window, int width, int height) {
        if (auto win = static_cast<Window*>(glfwGetWindowUserPointer(window))) {
            win->width_= width;
            win->height_ = height;
            win->resized_ = true;
        }
    }

    // ウィンドウのリサイズフラグを消費し、リサイズが発生していたかどうかを返します。
    bool Window::consume_resized_flag() {
        bool was_resized = resized_;
        resized_ = false;
        return was_resized;
    }


    void Window::wait_while_minimized() const {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {
            glfwWaitEvents();  // 最小化中のみ: イベントが来るまで待つ
            glfwGetFramebufferSize(window_, &width, &height);
        }
    }

    // ウィンドウが閉じるべきかどうかを返します。GLFWのウィンドウクローズフラグをチェックします。
    bool Window::should_close() const {
        return glfwWindowShouldClose(window_) != 0;
    }

    // GLFWのイベントをポーリングします。ウィンドウの入力やイベントを処理するために呼び出されます。
    void Window::poll_events() {
        glfwPollEvents();
    }

    // GLFWのウィンドウハンドルを返します。Vulkanのサーフェス作成などで使用されます。
    GLFWwindow* Window::handle() const {
        return window_;
    }

    // ウィンドウの幅を返します。
    std::uint32_t Window::width() const {
        return width_;
    }

    // ウィンドウの高さを返します。
    std::uint32_t Window::height() const {
        return height_;
    }

    // GLFWがVulkanインスタンスを作成するために必要な拡張機能のリストを返します。
    const char ** Window::required_instance_extensions() {
        uint32_t extension_count = 0;
        return glfwGetRequiredInstanceExtensions(&extension_count);
    }

    // フルスクリーン切り替え
    void Window::set_fullscreen(bool enabled) {
        if (enabled) {
            fullscreen_ = true;

            // 現在のウィンドウ状態を記録する
            glfwGetWindowPos(window_, &windowed_x_, &windowed_y_);
            glfwGetWindowSize(window_, &windowed_width_, &windowed_height_);

            glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_FALSE);

            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            int mx, my;
            glfwGetMonitorPos(monitor, &mx, &my);

            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window_, nullptr, mx, my, mode->width, mode->height, 0);
        } else {
            fullscreen_ = false;
            glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowMonitor(window_, nullptr, windowed_x_, windowed_y_, windowed_width_, windowed_height_, 0);
        }
    }

    bool Window::is_fullscreen() const {
        return fullscreen_;
    }

    bool Window::is_focused() const {
        return focused_;
    }

    bool Window::is_key_pressed(int key) const {
        return glfwGetKey(window_, key);
    };

#ifdef GLFW_EXPOSE_NATIVE_WIN32
    void* Window::win32_window() const {
        HWND hwnd = glfwGetWin32Window(window_);
        return hwnd;
    };
#endif

    void Window::focus_callback(GLFWwindow *window, int focused) {
        if (auto win = static_cast<Window*>(glfwGetWindowUserPointer(window))) {
            win->focused_ = focused;
        }
    }

    // -- 入力コールバックの設定（転送先 std::function を保存する） --

    void Window::set_key_callback(KeyCallback callback) {
        key_callback_ = std::move(callback);
    }

    void Window::set_cursor_pos_callback(CursorPosCallback callback) {
        cursor_pos_callback_ = std::move(callback);
    }

    void Window::set_mouse_button_callback(MouseButtonCallback callback) {
        mouse_button_callback_ = std::move(callback);
    }

    void Window::set_scroll_callback(ScrollCallback callback) {
        scroll_callback_ = std::move(callback);
    }

    void Window::set_cursor_captured(bool captured) {
        glfwSetInputMode(window_, GLFW_CURSOR,
            captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }

    // -- GLFW からの静的コールバック（user pointer 経由で Window* を取り出し転送する。focus_callback と同方式） --

    void Window::key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        if (auto win = static_cast<Window*>(glfwGetWindowUserPointer(window))
            ; win && win->key_callback_) {
            win->key_callback_(key, action);
        }
        (void)scancode; (void)mods;
    }

    void Window::cursor_pos_callback(GLFWwindow* window, double x, double y) {
        if (auto win = static_cast<Window*>(glfwGetWindowUserPointer(window))
            ; win && win->cursor_pos_callback_) {
            win->cursor_pos_callback_(x, y);
        }
    }

    void Window::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
        if (auto win = static_cast<Window*>(glfwGetWindowUserPointer(window))
            ; win && win->mouse_button_callback_) {
            win->mouse_button_callback_(button, action);
        }
        (void)mods;
    }

    void Window::scroll_callback(GLFWwindow* window, double x_offset, double y_offset) {
        if (auto win = static_cast<Window*>(glfwGetWindowUserPointer(window))
            ; win && win->scroll_callback_) {
            win->scroll_callback_(x_offset, y_offset);
        }
    }
}  // namespace sq::graphics
