#pragma once

#include <cstdint>
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

    // Win32 ネイティブハンドル (HWND) 。 swapchain の HMONITOR 取得に使う。
    // windows.h のヘッダ汚染を避けるためvoid*で返す。
    [[nodiscard]] void* win32_window() const;

private:
    static void focus_callback(GLFWwindow* window, int focused);  // glfwSetWindowFocusCallback で登録

    GLFWwindow* window_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool resized_ = false;
    bool fullscreen_ = false;
    int windowed_x_ = 0, windowed_y_ = 0;        // 復元用のウィンドウ位置
    int windowed_width_ = 0, windowed_height_ = 0;  // 復元用のサイズ
    bool focused_ = true;
};

}  // namespace sq::graphics
