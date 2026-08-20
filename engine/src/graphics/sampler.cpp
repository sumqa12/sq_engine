#include "sq/graphics/sampler.hpp"

namespace sq::graphics {

Sampler::Sampler(VkPhysicalDevice physical_device, VkDevice device)
    : device_(device) {

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    // mipmapMode = LINEAR はレベル間も補間する（トライリニア）。
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.minLod = 0;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.mipLodBias = 0;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler_info.unnormalizedCoordinates = VK_FALSE;

    VkPhysicalDeviceProperties sampler_properties;
    vkGetPhysicalDeviceProperties(physical_device, &sampler_properties);
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_device, &supported);
    if (supported.samplerAnisotropy) {
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy    = sampler_properties.limits.maxSamplerAnisotropy;
    } else {
        sampler_info.anisotropyEnable = VK_FALSE;
        sampler_info.maxAnisotropy    = 1.0f;
    }

    vkCreateSampler(device_, &sampler_info, nullptr, &sampler_);
}

Sampler::~Sampler() {
    vkDestroySampler(device_, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

VkSampler Sampler::handle() const {
    return sampler_;
}

}  // namespace sq::graphics
