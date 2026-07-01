#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// 表示中／レンダリング中のスワップチェーン画像への上書きを防ぐために、
// フレーム単位のセマフォ（GPU間同期）およびフェンス（CPU-GPU同期）が使用される。
class SyncObjects {
public:
    SyncObjects(VkDevice device, std::size_t frames_in_flight, std::size_t swapchain_image_count);
    ~SyncObjects();

    SyncObjects(const SyncObjects&) = delete;
    SyncObjects& operator=(const SyncObjects&) = delete;

    [[nodiscard]] VkSemaphore image_available(std::size_t frame_index) const;
    [[nodiscard]] VkSemaphore render_finished(std::size_t frame_index) const;
    [[nodiscard]] VkFence in_flight_fence(std::size_t frame_index) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence> in_flight_fences_;
};

}  // namespace sq::graphics
