#pragma once

#include <vulkan/vulkan.h>

namespace sq::graphics {

// VkDebugUtilsMessengerEXT をラップし、検証レイヤーの警告やエラーを
// コールバック（ここでは spdlog 経由でログ出力）に転送します。検証が無効になっている場合は何もしません。
class DebugMessenger {
public:
    DebugMessenger(VkInstance instance, bool enabled);
    ~DebugMessenger();

    DebugMessenger(const DebugMessenger&) = delete;
    DebugMessenger& operator=(const DebugMessenger&) = delete;

    [[nodiscard]] static VkDebugUtilsMessengerCreateInfoEXT make_create_info();

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callback_data, const void* user_data);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    bool enabled_ = false;
};

}  // namespace sq::graphics
