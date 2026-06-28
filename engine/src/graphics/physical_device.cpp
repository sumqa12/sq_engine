#include "sq/graphics/physical_device.hpp"

#include <stdexcept>
#include <vector>

namespace sq::graphics {

VkPhysicalDevice PhysicalDeviceSelector::select(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0) {
        throw std::runtime_error("適切なGPUが見つかりませんでした。");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    for (const auto& device : devices) {
        if (is_suitable(device, surface)) {
            return device;
        }
    }

    throw std::runtime_error("適切なGPUが見つかりませんでした。");
}

QueueFamilyIndices PhysicalDeviceSelector::find_queue_families(VkPhysicalDevice device,
                                                                  VkSurfaceKHR surface) {
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

    QueueFamilyIndices indices;
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics_family = i;
        }
    }

    VkBool32 present_support = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, indices.graphics_family.value(), surface, &present_support);
    indices.present_family = present_support ? indices.graphics_family : std::nullopt;

    (void)device;
    (void)surface;
    return indices;
}

bool PhysicalDeviceSelector::is_suitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
    if (auto queue_families = find_queue_families(device, surface); !queue_families.is_complete()) {
        return false;
    }

    (void)device;
    (void)surface;
    return true;
}

}  // namespace sq::graphics
