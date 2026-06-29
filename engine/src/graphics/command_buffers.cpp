#include "sq/graphics/command_buffers.hpp"

namespace sq::graphics {

CommandBuffers::CommandBuffers(VkDevice device, std::uint32_t graphics_queue_family,
                                std::size_t count)
    : device_(device) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = graphics_queue_family;

    vkCreateCommandPool(device_, &pool_info, nullptr, &pool_);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(count);

    buffers_.resize(count);
    vkAllocateCommandBuffers(device_, &alloc_info, buffers_.data());

    (void)graphics_queue_family;
    (void)count;
}

CommandBuffers::~CommandBuffers() {
    vkDestroyCommandPool(device_, pool_, nullptr);
    buffers_.clear();
}

void CommandBuffers::record(std::size_t index, const RecordFn& record) {
    // コマンドバッファをリセットして、再記録する
    vkResetCommandBuffer(buffers_[index], VK_COMMAND_BUFFER_RESET_FLAG_BITS_MAX_ENUM);
    vkResetCommandPool(device_, pool_, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(buffers_[index], &begin_info);
    record(buffers_[index], index);
    vkEndCommandBuffer(buffers_[index]);

    (void)index;
    (void)record;
}

const VkCommandBuffer *CommandBuffers::at(std::size_t index) const {
    return buffers_.empty() ? nullptr : &buffers_[index];
}

}  // namespace sq::graphics
