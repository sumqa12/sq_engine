#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "sq/graphics/window.hpp"

#include <stdexcept>

namespace sq::graphics {

// GLFWの初期化と終了を管理し、Vulkan用に設定されたウィンドウを作成します。
Window::Window(std::uint32_t width, std::uint32_t height, const std::string& title) {

    // glfwの初期化
    if (!glfwInit()) {
        throw std::runtime_error("GLFW の初期化に失敗しました。");
    }

    // glfwWindowの初期化
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("GLFW ウィンドウの作成に失敗しました。");
    }

    (void)width;
    (void)height;
    (void)title;
}

// GLFWの終了とウィンドウの破棄を行います。
Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
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
const char** Window::required_instance_extensions() {
    return glfwGetRequiredInstanceExtensions(nullptr);
}

}  // namespace sq::graphics
