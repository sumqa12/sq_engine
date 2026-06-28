#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// VkSurfaceKHR、VkSwapchainKHR、およびレンダリングターゲットとして使用される画像ごとの VkImageViews を所有しています。
// ウィンドウのサイズが変更された際には、これらを再作成する必要があります。
class Swapchain {
public:
    Swapchain(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
              VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void recreate(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] VkSwapchainKHR handle() const;
    [[nodiscard]] VkFormat image_format() const;
    [[nodiscard]] VkExtent2D extent() const;
    [[nodiscard]] const std::vector<VkImageView>& image_views() const;

private:
    void create(std::uint32_t width, std::uint32_t height);
    void destroy();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat image_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
};

}  // namespace sq::graphics
