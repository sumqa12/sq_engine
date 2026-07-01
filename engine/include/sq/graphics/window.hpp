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

private:
    GLFWwindow* window_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool resized_ = false;
};

}  // namespace sq::graphics
