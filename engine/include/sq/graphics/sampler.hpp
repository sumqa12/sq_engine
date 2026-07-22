#pragma once

#include <vulkan/vulkan.h>

namespace sq::graphics {

// VkSampler を所有するRAIIラッパー。テクスチャのサンプリング方法（フィルタリング・
// アドレッシング・異方性）を定義する。本エンジンでは全テクスチャで1つを共有する。
class Sampler {
public:
    // physical_device は異方性フィルタリングの上限値（maxSamplerAnisotropy）の取得に使う。
    Sampler(VkPhysicalDevice physical_device, VkDevice device);
    ~Sampler();  // vkDestroySampler

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    [[nodiscard]] VkSampler handle() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
