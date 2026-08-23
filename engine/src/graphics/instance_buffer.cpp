#include "sq/graphics/instance_buffer.hpp"

namespace sq::graphics {

InstanceBuffer::InstanceBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_instances)
    : Buffer(allocator, device, max_instances * sizeof(InstanceData),
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    // UniformBuffer と同じ形で永続マップを取り、capacity_ を保存する。
    mapped_   = map();
    capacity_ = max_instances;
}

InstanceBuffer::~InstanceBuffer() {
    // UniformBuffer::~UniformBuffer と同じ後始末を行う。
    if (mapped_) {
        unmap();
        mapped_ = nullptr;
    }
}

void InstanceBuffer::update(const InstanceData* data, std::size_t count) {
    // count 要素を memcpy する。
    //   ★ 先に count を capacity_ で切り詰めること。上限を超えて書くと、
    //     GpuAllocator の同じブロックに載っている**別のバッファを踏み潰す**（原因の分かりにくい破壊になる）。
    count = std::min(count, capacity_);
    std::memcpy(mapped_, data, count * sizeof(InstanceData));
}

std::size_t InstanceBuffer::capacity() const {
    return capacity_;
}

}  // namespace sq::graphics
