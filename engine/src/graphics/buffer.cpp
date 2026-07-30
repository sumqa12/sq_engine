#include "sq/graphics/buffer.hpp"

#include <cstring>
#include <stdexcept>

#include "sq/graphics/single_time_commands.hpp"

namespace sq::graphics {

Buffer::Buffer(GpuAllocator& allocator, VkDevice device,
               VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
    : allocator_(&allocator), device_(device), size_(size) {

    // 1. バッファの作成
    VkBufferCreateInfo buffer_create_info = {};
    buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.size = size;
    buffer_create_info.usage = usage;

    if (int result = vkCreateBuffer(device_, &buffer_create_info, nullptr, &buffer_); result != VK_SUCCESS) {
        throw std::runtime_error("バッファの作成に失敗しました。");
    }

    // 2. メモリ要件の取得
    VkMemoryRequirements memory_requirements;
    vkGetBufferMemoryRequirements(device_, buffer_, &memory_requirements);

    // 3. メモリの確保（phase11 ③: 個別 vkAllocateMemory → GpuAllocator のサブアロケート）
    allocation_ = allocator_->allocate(memory_requirements, properties);

    // 4. メモリのバインド（ブロック内 offset を渡す点に注意）
    if (int result = vkBindBufferMemory(device_, buffer_, allocation_.memory, allocation_.offset);
        result != VK_SUCCESS) {
        throw std::runtime_error("バッファのメモリバインドに失敗しました。");
    }

    (void)usage;
    (void)properties;
}

Buffer::~Buffer() {
    vkDestroyBuffer(device_, buffer_, nullptr);
    if (allocator_ != nullptr) {
        allocator_->free(allocation_);  // phase11 ③: 個別 vkFreeMemory → ブロックへ返却
    }
    device_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = {};
    allocator_ = nullptr;
}

VkBuffer Buffer::handle() const {
    return buffer_;
}

VkDeviceSize Buffer::size() const {
    return size_;
}

void* Buffer::map() {
    // GpuAllocator がブロックを persistent map 済み。allocation_.mapped をそのまま返す。
    // DEVICE_LOCAL では nullptr（HOST_VISIBLE でないバッファに対して呼ばないこと）。
    return allocation_.mapped;
}

void Buffer::unmap() {
    // persistent mapping のため何もしない（ブロックの unmap は GpuAllocator デストラクタが行う）。
}

void Buffer::upload_with_staging(GpuAllocator& allocator,
                                 std::uint32_t queue_family, VkQueue queue,
                                 const void* data, VkDeviceSize size) {
    // (phase11 ②）
    StagingBuffer staging(allocator, device_, data, size);  // TRANSFER_SRC/HOST_VISIBLE、ctorで書き込み済み
    SingleTimeCommands cmd(device_, queue_family, queue);
    VkBufferCopy region{ .srcOffset = 0, .dstOffset = 0, .size = size };
    vkCmdCopyBuffer(cmd.handle(), staging.handle(), buffer_, 1, &region);
    cmd.submit_and_wait();  // staging はスコープ末尾で破棄される
}

// ----- StagingBuffer -----

StagingBuffer::StagingBuffer(GpuAllocator& allocator, VkDevice device,
                             const void* data, VkDeviceSize size)
    : Buffer(allocator, device, size,
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    void* mapped = map();
    std::memcpy(mapped, data, size);
    unmap();  // no-op（persistent mapping）。HOST_COHERENT なので flush も不要
    (void)data;
}

std::uint32_t Buffer::find_memory_type(VkPhysicalDevice physical_device,
                                        std::uint32_t type_filter,
                                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("適切なメモリタイプが見つかりませんでした。");
}

}  // namespace sq::graphics
