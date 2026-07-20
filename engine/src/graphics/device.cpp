#include "sq/graphics/device.hpp"

#include "sq/graphics/vulkan_instance.hpp"

#include <vulkan/vulkan_extension_inspection.hpp>

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

        std::vector<const char*> device_extensions;
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    #ifdef VK_USE_PLATFORM_WIN32_KHR
        if (is_supported_full_screen_extension()) {
            printf("フルスクリーン対応\n");
            device_extensions.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
            fullscreen_exclusive_supported_ = true;
        }
    #endif
        printf("デバイス拡張機能数: %llu\n", device_extensions.size());
        device_create_info.ppEnabledExtensionNames = device_extensions.data();
        device_create_info.enabledExtensionCount = device_extensions.size();
        device_create_info.pNext = nullptr;
        device_create_info.ppEnabledLayerNames = nullptr;
        device_create_info.enabledLayerCount = 0;

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

    bool Device::is_supported_full_screen_extension() const {
        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extension_count, nullptr);

        std::vector<VkExtensionProperties> ex(extension_count);
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extension_count, ex.data());

        for (uint32_t i = 0; i < extension_count; i++) {
            if (strcmp(ex[i].extensionName, "VK_EXT_full_screen_exclusive") == 0) {
                return true;
            }
        }

        return false;
    }

    bool Device::is_fullscreen_exclusive_supported() const {
        return fullscreen_exclusive_supported_;
    }
}  // namespace sq::graphics
