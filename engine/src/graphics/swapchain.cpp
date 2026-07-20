#include "sq/graphics/swapchain.hpp"

#include <stdexcept>
#include <spdlog/spdlog.h>

#ifdef _WIN32
    #include <windows.h>
    #include <vulkan/vulkan_win32.h>
#endif

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
        if (exclusive_acquired_) {
            release_full_screen_exclusive();
        }

        vkDeviceWaitIdle(device_);

        // 旧image viewだけ破棄（swapchain本体はまだ破棄しない）
        for (const auto& view : image_views_) {
            vkDestroyImageView(device_, view, nullptr);
        }
        image_views_.clear();
        images_.clear();

        VkSwapchainKHR old_swapchain = swapchain_;
        create(width, height);   // create()内の oldSwapchain = swapchain_ が今度は本物の旧ハンドルを指す
        vkDestroySwapchainKHR(device_, old_swapchain, nullptr);  // 新規作成後にretired状態の旧を破棄
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

        if (formats.empty()) {
            throw std::runtime_error("サーフェス形式が見つかりませんでした。");
        }

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
            if (presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                // 第二希望: IMMEDIATE (垂直同期オフ、MAILBOXがない場合の低遅延用)
                present_mode = presentMode;
            }
        }

        // (幅、高さ) を VkSurfaceCapabilitiesKHR の最小/最大範囲 -> extent_ にクリップする
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
        if (extent_ = caps.currentExtent; extent_.width == UINT32_MAX) {
            extent_.width = width;
            extent_.height = height;
        }

        // 画像数の決定
        uint32_t image_count = caps.minImageCount + 1;
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

#ifdef _WIN32
        VkSurfaceFullScreenExclusiveWin32InfoEXT win32_info{};
        VkSurfaceFullScreenExclusiveInfoEXT fse_info{};
        if (fullscreen_exclusive_ && hwnd_) {
            win32_info.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
            win32_info.hmonitor = MonitorFromWindow(static_cast<HWND>(hwnd_), MONITOR_DEFAULTTONEAREST);

            fse_info.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
            fse_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
            fse_info.pNext = &win32_info;

            VkPhysicalDeviceSurfaceInfo2KHR surface_info{};
            surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
            surface_info.pNext = &fse_info;
            surface_info.surface = surface_;

            // 出力側: 回答を受け取る構造体
            VkSurfaceCapabilitiesFullScreenExclusiveEXT fse_caps{};
            fse_caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT;

            VkSurfaceCapabilities2KHR caps2{};
            caps2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
            caps2.pNext = &fse_caps;

            vkGetPhysicalDeviceSurfaceCapabilities2KHR(physical_device_, &surface_info, &caps2);

            if (fse_caps.fullScreenExclusiveSupported == VK_TRUE) {
                create_info.pNext = &fse_info;
                created_with_fse_ = true;
            }
        } else {
            printf("fullscreen_exclusive_: %d\n", fullscreen_exclusive_);
        }
#endif

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

        printf("Swapchain created: %u images, format: %d, extent: (%u, %u)\n",
               static_cast<unsigned int>(images_.size()), image_format_, extent_.width, extent_.height);

        (void)width;
        (void)height;
    }

#ifdef _WIN32
    void Swapchain::set_fullscreen_exclusive(bool enabled, void* hwnd) {
        fullscreen_exclusive_ = enabled;
        hwnd_ = enabled ? hwnd : nullptr;
    }

    VkResult Swapchain::acquire_full_screen_exclusive() {
        exclusive_acquired_ = true;

        acquire_full_screen_exclusive_fn_ =
            reinterpret_cast<PFN_vkAcquireFullScreenExclusiveModeEXT>(
                vkGetDeviceProcAddr(device_, "vkAcquireFullScreenExclusiveModeEXT")
            );

        // 非対応の場合 nullptr が返る
        if (acquire_full_screen_exclusive_fn_ == nullptr) {
            // 排他モードなしで続行（ログ推奨）
            spdlog::log(spdlog::level::info,"ウィンドウの排他モードが有効化されていないか、非対応です。");
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        return acquire_full_screen_exclusive_fn_(device_, swapchain_);
    }

    VkResult Swapchain::release_full_screen_exclusive() {
        exclusive_acquired_ = false;
        created_with_fse_ = false;

        release_full_screen_exclusive_fn_ =
            reinterpret_cast<PFN_vkReleaseFullScreenExclusiveModeEXT>(
                vkGetDeviceProcAddr(device_, "vkReleaseFullScreenExclusiveModeEXT")
            );

        if (release_full_screen_exclusive_fn_ == nullptr) {
            spdlog::log(spdlog::level::info, "ウィンドウの排他モードが有効化されていないか、非対応です。");
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        return release_full_screen_exclusive_fn_(device_, swapchain_);
    }
#endif

    bool Swapchain::exclusive_acquired() const {
        return exclusive_acquired_;
    }

    bool Swapchain::created_with_fse() const {
        return created_with_fse_;
    }

    void Swapchain::destroy() {
        if (exclusive_acquired_) {
            release_full_screen_exclusive();
        }

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
