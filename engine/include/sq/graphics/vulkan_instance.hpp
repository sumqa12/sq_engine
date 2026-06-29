#pragma once

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// VkInstance を保持しています。デバッグビルドでは、Khronos 検証レイヤーを要求し、その後、
// DebugMessenger がそれにアタッチできるようにします。
class VulkanInstance {
public:
    VulkanInstance(const std::string& app_name, const char** required_extensions);
    ~VulkanInstance();

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    [[nodiscard]] VkInstance handle() const;
    [[nodiscard]] bool validation_enabled() const;

    // Vulkan SDK に同梱されている Khronos 標準の検証レイヤーの名称。
    static const char* kValidationLayerName;

private:
    [[nodiscard]] static bool supports_validation_layer();

    VkInstance instance_ = VK_NULL_HANDLE;
    bool validation_enabled_ = false;
};

}  // namespace sq::graphics
