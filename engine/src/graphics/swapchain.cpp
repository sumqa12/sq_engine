#include "sq/graphics/swapchain.hpp"

#include <stdexcept>

namespace sq::graphics {

Swapchain::Swapchain(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                      VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height)
    : device_(device), physical_device_(physical_device), surface_(surface) {

    (void)instance;
    create(width, height);
}

Swapchain::~Swapchain() {
    destroy();
}

void Swapchain::recreate(std::uint32_t width, std::uint32_t height) {
    destroy();
    create(width, height);
}

VkSwapchainKHR Swapchain::handle() const {
    return swapchain_;
}

VkFormat Swapchain::image_format() const {
    return image_format_;
}

VkExtent2D Swapchain::extent() const {
    return extent_;
}

const std::vector<VkImageView>& Swapchain::image_views() const {
    return image_views_;
}

void Swapchain::create(std::uint32_t width, std::uint32_t height) {
    // サーフェス形式を選択する
    uint32_t count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, formats.data());

    VkSurfaceFormatKHR surface_format = formats[0];
    image_format_ = surface_format.format;
    for (const auto &surfaceFormat : formats)
    {
        if (surfaceFormat.colorSpace != VK_COLORSPACE_SRGB_NONLINEAR_KHR)
        {
            continue;
        }
        if (surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM || surfaceFormat.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            surface_format = surfaceFormat;
            image_format_ = surfaceFormat.format;
            break;
        }
    }

    if (image_format_ == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("サーフェス形式の選択に失敗しました。");
    }

    // プレゼンテーションモードを選択する（VK_PRESENT_MODE_MAILBOX_KHR を優先、フォールバックは VK_PRESENT_MODE_FIFO_KHR）
    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &present_mode_count, present_modes.data());

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // デフォルトは FIFO
    for (const auto &presentMode : present_modes) {
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = presentMode;
            break;
        }
    }

    // (幅、高さ) を VkSurfaceCapabilitiesKHR の最小/最大範囲 -> extent_ にクリップする
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    if (extent_ = caps.currentExtent; extent_.width == UINT32_MAX) {
        extent_.width = width;
        extent_.height = height;
    } else {
        extent_ = caps.currentExtent;
    }

    // 画像数の決定
    uint32_t image_count = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    // スワップチェーンの作成
    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent_;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = swapchain_;

    if (vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_) != VK_SUCCESS) {
        throw std::runtime_error("スワップチェーンの作成に失敗しました。");
    }

    // 画像の生成
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, images_.data());

    for (const auto &image : images_) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_format_;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        VkImageView imageView;
        if (vkCreateImageView(device_, &view_info, nullptr, &imageView) != VK_SUCCESS) {
            throw std::runtime_error("スワップチェーン画像ビューの作成に失敗しました。");
        }
        image_views_.push_back(imageView);
    }

    (void)width;
    (void)height;
}

void Swapchain::destroy() {
    // GPUがアイドル状態になってから破棄
    vkDeviceWaitIdle(device_);
    for (const auto &imageView : image_views_) {
        vkDestroyImageView(device_, imageView, nullptr);
    }
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);

    image_views_.clear();
    images_.clear();
    swapchain_ = VK_NULL_HANDLE;
}

}  // namespace sq::graphics
