#include "sq/graphics/buffer.hpp"

#include <stdexcept>

namespace sq::graphics {

Buffer::Buffer(VkPhysicalDevice physical_device, VkDevice device,
               VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
    : device_(device), size_(size) {

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

    // 3. メモリタイプの取得
    std::uint32_t memory_type_index = find_memory_type(physical_device, memory_requirements.memoryTypeBits, properties);

    // 4. メモリの割り当て
    VkMemoryAllocateInfo memory_allocate_info = {};
    memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_allocate_info.allocationSize = memory_requirements.size;
    memory_allocate_info.memoryTypeIndex = memory_type_index;
    if (int result = vkAllocateMemory(device_, &memory_allocate_info, nullptr, &memory_); result != VK_SUCCESS) {
        throw std::runtime_error("メモリの割り当てに失敗しました。");
    }

    // 5. メモリのバインド
    if (int result = vkBindBufferMemory(device_, buffer_, memory_, 0); result != VK_SUCCESS) {
        throw std::runtime_error("バッファのメモリバインドに失敗しました。");
    }

    (void)physical_device;
    (void)usage;
    (void)properties;
}

Buffer::~Buffer() {
    vkDestroyBuffer(device_, buffer_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    device_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
}

VkBuffer Buffer::handle() const {
    return buffer_;
}

VkDeviceSize Buffer::size() const {
    return size_;
}

void* Buffer::map() {
    void* mapped = nullptr;
    if (int result = vkMapMemory(device_, memory_, 0, size_, 0, &mapped); result != VK_SUCCESS) {
        throw std::runtime_error("バッファのマッピングに失敗しました。");
    }
    return mapped;
}

void Buffer::unmap() {
    vkUnmapMemory(device_, memory_);
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
