#pragma once

#include <optional>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// このエンジンが必要とするキューファミリのインデックス。1つのGPUは、
// 同じファミリ上、あるいは異なるファミリ上で、グラフィックスおよびプレゼンテーションのサポートを提供する場合がある。
struct QueueFamilyIndices {
    std::optional<std::uint32_t> graphics_family;
    std::optional<std::uint32_t> present_family;

    [[nodiscard]] bool is_complete() const {
        return graphics_family.has_value() && present_family.has_value();
    }
};

// 適切な VkPhysicalDevice (GPU) を選択し、そのキューファミリーを調べます。
// ステートレスなヘルパー関数：ここでは GPU ハンドルは保持されず、論理デバイスは Device が所有します。
class PhysicalDeviceSelector {
public:
    [[nodiscard]] static VkPhysicalDevice select(VkInstance instance, VkSurfaceKHR surface);

    [[nodiscard]] static QueueFamilyIndices find_queue_families(VkPhysicalDevice device,
                                                                  VkSurfaceKHR surface);

private:
    [[nodiscard]] static bool is_suitable(VkPhysicalDevice device, VkSurfaceKHR surface);
};

}  // namespace sq::graphics
