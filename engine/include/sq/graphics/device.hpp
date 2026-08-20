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

    // bindless（descriptor indexing）に必要な機能が揃っているか（phase13 ①-1 / D-4）。
    // Vulkan 1.3 ではコア機能だが、**使うには明示的な有効化が必要**で、
    // かつ実装が対応しているとは限らないので生成前に問い合わせる。
    //
    // vkGetPhysicalDeviceFeatures2 に VkPhysicalDeviceVulkan12Features を pNext で繋いで取得し、
    // 以下がすべて VK_TRUE かを返す:
    //   runtimeDescriptorArray                       サイズ未指定の配列 sampler2D textures[]
    //   descriptorBindingPartiallyBound              配列の一部しか埋めなくてよい
    //   shaderSampledImageArrayNonUniformIndexing    同一ドロー内で添字が変わってよい（②で必須）
    //   descriptorBindingSampledImageUpdateAfterBind バインド済みセットへ後から書き込める
    [[nodiscard]] static bool is_supported_descriptor_indexing(VkPhysicalDevice physical_device);

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
