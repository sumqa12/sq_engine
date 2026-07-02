#include "sq/graphics/uniform_buffer.hpp"

#include <stdexcept>

#include "sq/graphics/physical_device.hpp"

namespace sq::graphics {

UniformBuffer::UniformBuffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size)
    : device_(device), size_(size) {
    VkBufferCreateInfo buffer_create_info = {};
    buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer_create_info.size = size_;
    if (vkCreateBuffer(device_, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
        throw std::runtime_error("ユニフォームバッファの作成に失敗しました。");
    }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);

    auto memory_type = find_memory_type(physical_device, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo memory_allocate_info = {};
    memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_allocate_info.allocationSize = requirements.size;
    memory_allocate_info.memoryTypeIndex = memory_type;
    vkAllocateMemory(device_, &memory_allocate_info, nullptr, &memory_);

    if (vkBindBufferMemory(device_, buffer_, memory_, 0) != VK_SUCCESS) {
        throw std::runtime_error("バッファメモリのバインドに失敗しました。");
    }

    if (vkMapMemory(device_, memory_, 0, buffer_create_info.size, 0, &mapped_) != VK_SUCCESS) {
        throw std::runtime_error("バッファメモリのマッピングに失敗しました。");
    }

    (void)physical_device;
}

UniformBuffer::~UniformBuffer() {
    if (mapped_) vkUnmapMemory(device_, memory_);
    vkDestroyBuffer(device_, buffer_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    device_ = VK_NULL_HANDLE;
    mapped_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    memory_ = nullptr;
}

void UniformBuffer::update(const void* data, std::size_t size) {
    std::memcpy(mapped_, data, size);

    (void)data;
    (void)size;
}

VkBuffer UniformBuffer::handle() const {
    return buffer_;
}

VkDeviceSize UniformBuffer::size() const {
    return size_;
}

std::uint32_t UniformBuffer::find_memory_type(VkPhysicalDevice physical_device,
                                               std::uint32_t type_filter,
                                               VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

    std::uint32_t memory_type_index = -1;
    for (std::uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            memory_type_index = i;
            break;
        }
    }

    if (memory_type_index == -1) {
        throw std::runtime_error("適切なメモリタイプが見つかりませんでした。");
    }

    (void)physical_device;
    (void)type_filter;
    (void)properties;
    return memory_type_index;
}

}  // namespace sq::graphics
