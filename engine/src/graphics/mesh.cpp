#include "sq/graphics/mesh.hpp"

#include <stdexcept>

namespace sq::graphics {

VertexBuffer::VertexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                            const std::vector<Vertex>& vertices)
    : device_(device), vertex_count_(static_cast<std::uint32_t>(vertices.size())) {
    VkBufferCreateInfo buffer_create_info = {};
    buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_create_info.size = sizeof(Vertex) * vertices.size();
    if (vkCreateBuffer(device_, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
        throw std::runtime_error("頂点バッファの作成に失敗しました。");
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

    void* map_memory;
    if (vkMapMemory(device_, memory_, 0, buffer_create_info.size, 0, &map_memory) == VK_SUCCESS) {
        std::memcpy(map_memory, vertices.data(), buffer_create_info.size);
        vkUnmapMemory(device_, memory_);
    }

    (void)physical_device;
    (void)vertices;
}

VertexBuffer::~VertexBuffer() {
    vkDestroyBuffer(device_, buffer_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    device_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
}

void VertexBuffer::bind(VkCommandBuffer command_buffer) const {
    VkBuffer buffers[] = {buffer_};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(command_buffer, 0, 1, buffers, offsets);

    (void)command_buffer;
}

std::uint32_t VertexBuffer::vertex_count() const {
    return vertex_count_;
}

std::uint32_t VertexBuffer::find_memory_type(VkPhysicalDevice physical_device,
                                              std::uint32_t type_filter,
                                              VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

    std::uint32_t memory_type_count = -1;
    for (std::uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            memory_type_count = i;
            break;
        }
    }

    if (memory_type_count == -1) {
        throw std::runtime_error("適切なメモリタイプが見つかりませんでした。");
    }

    (void)physical_device;
    (void)type_filter;
    (void)properties;
    return memory_type_count;
}

}  // namespace sq::graphics
