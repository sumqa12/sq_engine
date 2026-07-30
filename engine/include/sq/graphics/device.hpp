#pragma once

#include <memory>

#include <vulkan/vulkan.h>

#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/physical_device.hpp"

namespace sq::graphics {

// このエンジンが使用する論理デバイス（VkDevice）およびキューを所有しています。
class Device {
public:
    Device(VkPhysicalDevice physical_device, const QueueFamilyIndices& indices,
           bool enable_validation);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkDevice handle() const;
    [[nodiscard]] VkPhysicalDevice physical_device() const;
    [[nodiscard]] VkQueue graphics_queue() const;
    [[nodiscard]] VkQueue present_queue() const;
    [[nodiscard]] bool is_supported_full_screen_extension() const;
    [[nodiscard]] bool is_fullscreen_exclusive_supported() const;

    // GPUメモリのサブアロケータ（Buffer 生成時に渡す。phase11 ③）。
    [[nodiscard]] GpuAllocator& allocator();

private:
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    bool fullscreen_exclusive_supported_ = false;

    // 論理デバイス生成後に作り、デストラクタで device_ 破棄より前に破棄する（破棄順序に注意）。
    std::unique_ptr<GpuAllocator> allocator_;
};

}  // namespace sq::graphics
