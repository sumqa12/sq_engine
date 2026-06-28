#include "sq/graphics/sync_objects.hpp"

#include <stdexcept>

namespace sq::graphics {

SyncObjects::SyncObjects(VkDevice device, std::size_t frames_in_flight) : device_(device) {
    for (std::size_t i = 0; i < frames_in_flight; ++i) {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkSemaphore image_available_semaphore;
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &image_available_semaphore) != VK_SUCCESS) {
            throw std::runtime_error("「image available」セマフォの作成に失敗しました。");
        }

        VkSemaphore render_finished_semaphore;
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &render_finished_semaphore) != VK_SUCCESS) {
            throw std::runtime_error("「render finished」セマフォの作成に失敗しました。");
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.pNext = nullptr;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        image_available_semaphores_.push_back(image_available_semaphore);
        render_finished_semaphores_.push_back(render_finished_semaphore);

        in_flight_fences_.push_back({});
        if (vkCreateFence(device_, &fence_info, nullptr, &in_flight_fences_.back()) != VK_SUCCESS) {
            throw std::runtime_error("「in flight」フェンスの作成に失敗しました。");
        }
    }

    (void)frames_in_flight;
}

SyncObjects::~SyncObjects() {
    for (const auto& semaphore : image_available_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (const auto& semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (const auto& fence : in_flight_fences_) {
        vkDestroyFence(device_, fence, nullptr);
    }
    image_available_semaphores_.clear();
    render_finished_semaphores_.clear();
    in_flight_fences_.clear();
    device_ = VK_NULL_HANDLE;
}

VkSemaphore SyncObjects::image_available(std::size_t frame_index) const {
    return image_available_semaphores_.empty() ? VK_NULL_HANDLE
                                                 : image_available_semaphores_[frame_index];
}

VkSemaphore SyncObjects::render_finished(std::size_t frame_index) const {
    return render_finished_semaphores_.empty() ? VK_NULL_HANDLE
                                                 : render_finished_semaphores_[frame_index];
}

VkFence SyncObjects::in_flight_fence(std::size_t frame_index) const {
    return in_flight_fences_.empty() ? VK_NULL_HANDLE : in_flight_fences_[frame_index];
}

}  // namespace sq::graphics
