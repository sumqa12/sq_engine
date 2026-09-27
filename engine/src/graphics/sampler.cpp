#include "sq/graphics/sampler.hpp"

#include <stdexcept>

namespace sq::graphics {

Sampler::Sampler(VkPhysicalDevice physical_device, VkDevice device, const SamplerConfig& config)
    : device_(device) {

    // ★ 異方性はデバイス対応との AND を取ること。config.anisotropy だけを見ると
    //   非対応環境で vkCreateSampler がバリデーションエラーになる。
    // ★ compareEnable = VK_TRUE のとき anisotropyEnable も VK_TRUE にすると、
    //   環境によっては不正になる。**比較サンプラでは異方性を切る**こと
    //   （シャドウマップに異方性フィルタを掛ける意味も無い）。
    // ★ vkCreateSampler の戻り値を見ていないのも現状のまま。ここで VK_SUCCESS の確認と
    //   throw を入れておくと、設定の組み合わせを間違えたときに即座に分かる。
    //
    // ★ 既定構築の config では**現在とまったく同じサンプラ**ができること（⓪-5）。
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = config.filter;
    sampler_info.minFilter = config.filter;
    sampler_info.addressModeU = config.address_mode;
    sampler_info.addressModeV = config.address_mode;
    sampler_info.addressModeW = config.address_mode;
    sampler_info.borderColor = config.border_color;
    sampler_info.compareEnable = config.compare_enable;
    sampler_info.compareOp = config.compare_op;
    // mipmapMode = LINEAR はレベル間も補間する（トライリニア）。
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.minLod = 0;
    sampler_info.maxLod = config.max_lod;
    sampler_info.mipLodBias = 0;
    sampler_info.unnormalizedCoordinates = VK_FALSE;

    VkPhysicalDeviceProperties sampler_properties;
    vkGetPhysicalDeviceProperties(physical_device, &sampler_properties);
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_device, &supported);
    if (config.anisotropy && supported.samplerAnisotropy) {
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy    = sampler_properties.limits.maxSamplerAnisotropy;
    } else {
        sampler_info.anisotropyEnable = VK_FALSE;
        sampler_info.maxAnisotropy    = 1.0f;
    }

    if (VkResult result = vkCreateSampler(device_, &sampler_info, nullptr, &sampler_)
        ; result != VK_SUCCESS) {
        throw std::runtime_error("Sampler::Sampler : サンプラーの作成に失敗しました。");
    }
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
