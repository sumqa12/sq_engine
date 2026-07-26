#include "sq/graphics/single_time_commands.hpp"

#include <stdexcept>

#include <spdlog/spdlog.h>

namespace sq::graphics {

SingleTimeCommands::SingleTimeCommands(VkDevice device, std::uint32_t queue_family, VkQueue queue)
    : device_(device), queue_(queue) {
    // 一時コマンドプールを作る（phase10プラン E-1）
    VkCommandPoolCreateInfo pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family,
    };
    if (VkResult result = vkCreateCommandPool(device, &pool_create_info, nullptr, &pool_)
        ; result != VK_SUCCESS) {
        throw std::runtime_error("SingleTimeCommands : vkCreateCommandPool : コマンドプールの作成に失敗しました。");
    }

    // コマンドバッファを1本確保する
    VkCommandBufferAllocateInfo buffer_allocate_info = {};
    buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    buffer_allocate_info.commandBufferCount = 1;
    buffer_allocate_info.commandPool = pool_;
    buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    if (VkResult result = vkAllocateCommandBuffers(device, &buffer_allocate_info, &buffer_); result != VK_SUCCESS) {
        throw std::runtime_error("SingleTimeCommands : vkAllocateCommandBuffers : コマンドバッファの確保に失敗しました。");
    }

    // TODO: 記録を開始する
    VkCommandBufferBeginInfo buffer_begin_info = {};
    buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (VkResult result = vkBeginCommandBuffer(buffer_, &buffer_begin_info); result != VK_SUCCESS) {
        throw std::runtime_error("SingleTimeCommands : vkBeginCommandBuffer : コマンドバッファの記録開始に失敗しました。");
    }

    // 各VkResultをチェックし、失敗時は throw する。
    // 重要（RAIIの学習ポイント）: コンストラクタが例外で抜けるとデストラクタは呼ばれない。
    //   プール作成後にthrowする経路では、投げる前に自分で vkDestroyCommandPool すること。
}

SingleTimeCommands::~SingleTimeCommands() {
    if (!submitted_) {
        submit_and_wait();
    }

    if (buffer_ != VK_NULL_HANDLE) { vkFreeCommandBuffers(device_, pool_, 1, &buffer_); }
    if (pool_ != VK_NULL_HANDLE)   { vkDestroyCommandPool(device_, pool_, nullptr); }

    buffer_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

VkCommandBuffer SingleTimeCommands::handle() const {
    return buffer_;
}

void SingleTimeCommands::submit_and_wait() {
    if (submitted_) { return; }

    vkEndCommandBuffer(buffer_);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffer_;
    if (VkResult result = vkQueueSubmit(queue_, 1, &submit_info, VK_NULL_HANDLE)
        ; result != VK_SUCCESS) {
        spdlog::error("SingleTimeCommands : vkQueueSubmit : コマンド送信に失敗しました。");
    }

    vkQueueWaitIdle(queue_);
    submitted_ = true;
}

}  // namespace sq::graphics
