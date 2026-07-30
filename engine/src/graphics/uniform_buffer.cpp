#include "sq/graphics/uniform_buffer.hpp"

#include <cstring>

namespace sq::graphics {

UniformBuffer::UniformBuffer(GpuAllocator& allocator, VkDevice device, VkDeviceSize size)
    : Buffer(allocator, device, size,
             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    // 永続マップ先を取得する（phase11 ③: GpuAllocator がブロックを persistent map 済みなので map() はそのポインタを返すだけ）
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
