#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

    [[nodiscard]] bool should_close() const;
    static void poll_events() ;

    [[nodiscard]] GLFWwindow* handle() const;
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;

    // このプラットフォーム上でGLFWがサーフェスを作成するために必要なVulkanインスタンス拡張機能。
    [[nodiscard]] static const char** required_instance_extensions();

private:
    GLFWwindow* window_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

}  // namespace sq::graphics
