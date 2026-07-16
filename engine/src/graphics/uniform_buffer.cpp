#include "sq/graphics/uniform_buffer.hpp"

#include <cstring>

namespace sq::graphics {

UniformBuffer::UniformBuffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size)
    : Buffer(physical_device, device, size,
             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    // 永続マップする
    mapped_ = map();
}

UniformBuffer::~UniformBuffer() {
    if (mapped_) {
        unmap();
        mapped_ = nullptr;
    }
}

void UniformBuffer::update(const void* data, std::size_t size) {
    std::memcpy(mapped_, data, size);
}

}  // namespace sq::graphics
