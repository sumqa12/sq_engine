#include "sq/graphics/material_buffer.hpp"

namespace sq::graphics {

MaterialBuffer::MaterialBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_materials)
    : Buffer(allocator, device, max_materials * sizeof(MaterialData),
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    // InstanceBuffer と同じく永続マップを取り、capacity_ を保存する。
    mapped_ = map();
    capacity_ = max_materials;
}

MaterialBuffer::~MaterialBuffer() {
    if (mapped_) {
        unmap();
        mapped_ = nullptr;
    }
}

void MaterialBuffer::write(std::size_t index, const MaterialData& data) {
    //   1. index >= capacity_ なら何もせず return する（範囲外書き込みは別バッファを踏み潰す）
    //   2. static_cast<MaterialData*>(mapped_)[index] = data;  もしくは memcpy
    //      HOST_COHERENT なので vkFlushMappedMemoryRanges は不要。

    if (index >= capacity_) {
        return;
    }

    static_cast<MaterialData*>(mapped_)[index] = data;
}

std::size_t MaterialBuffer::capacity() const {
    return capacity_;
}

}  // namespace sq::graphics
