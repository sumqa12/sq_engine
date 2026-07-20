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

    // recreate前に呼ぶ設定用（メンバに保持）
    void set_fullscreen_exclusive(bool enabled, void* hwnd);

    // 排他モードの取得/解放（vkGetDeviceProcAddrで関数ポインタを取得して呼ぶ）
    // recreate 後切り替わってから呼ぶ
    VkResult acquire_full_screen_exclusive();
    VkResult release_full_screen_exclusive();

    [[nodiscard]] bool exclusive_acquired() const;
    [[nodiscard]] bool created_with_fse() const;

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

    bool fullscreen_exclusive_ = false;   // 次のcreateでpNextチェーンを付けるか
    void* hwnd_ = nullptr;                // HMONITOR導出用
    bool exclusive_acquired_ = false;     // 取得済みフラグ（二重acquire/release防止）
    bool created_with_fse_ = false;  // 現在のスワップチェーンがFSEチェーン付きで作られたか

#ifdef _WIN32
    PFN_vkAcquireFullScreenExclusiveModeEXT acquire_full_screen_exclusive_fn_ = nullptr;
    PFN_vkReleaseFullScreenExclusiveModeEXT release_full_screen_exclusive_fn_ = nullptr;
#endif

};

}  // namespace sq::graphics
