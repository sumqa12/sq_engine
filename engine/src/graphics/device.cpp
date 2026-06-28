#include "sq/graphics/device.hpp"

#include <vector>

namespace sq::graphics {

Device::Device(VkPhysicalDevice physical_device, const QueueFamilyIndices& indices,
               bool enable_validation)
    : physical_device_(physical_device) {
    // キューファミリーごとに、VkDeviceQueueCreateInfoを作成する
    uint32_t queue_family_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());

    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;

    for (uint32_t i = 0; i < queue_family_count; ++i) {
        queue_families[i] = queue_families[queue_family_count-1-i];

        queue_create_infos.push_back(VkDeviceQueueCreateInfo {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = i,
            .queueCount = 1,
            .pQueuePriorities = new float(1.0f),
        });
    }

    // VkDeviceCreateInfoを作成する
    VkDeviceCreateInfo device_create_info{};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = queue_create_infos.size();
    device_create_info.pQueueCreateInfos = queue_create_infos.data();
#if defined(NDEBUG)
    device_create_info.ppEnabledExtensionNames = kValidationLayerName;
#else
    device_create_info.ppEnabledExtensionNames = new const char* [1]{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
#endif
    device_create_info.enabledExtensionCount = 1;
    device_create_info.pNext = nullptr;
    device_create_info.ppEnabledLayerNames = nullptr;
    device_create_info.enabledLayerCount = 1;

    vkCreateDevice(physical_device_, &device_create_info, nullptr, &device_);

    // キューを取得する
    vkGetDeviceQueue(device_, indices.graphics_family.value(), 0, &graphics_queue_);
    vkGetDeviceQueue(device_, indices.present_family.value(), 0, &present_queue_);

    (void)indices;
    (void)enable_validation;
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    present_queue_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
}

VkDevice Device::handle() const {
    return device_;
}

VkPhysicalDevice Device::physical_device() const {
    return physical_device_;
}

VkQueue Device::graphics_queue() const {
    return graphics_queue_;
}

VkQueue Device::present_queue() const {
    return present_queue_;
}

}  // namespace sq::graphics
