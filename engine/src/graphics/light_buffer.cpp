#include "sq/graphics/light_buffer.hpp"

namespace sq::graphics {
    LightBuffer::LightBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_lights)
        : Buffer(allocator, device,
                 max_lights * sizeof(LightData),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
        mapped_ = map();
        capacity_ = max_lights;
    }

    LightBuffer::~LightBuffer() {
        if (mapped_) {
            unmap();
            mapped_ = nullptr;
        }
    }

    void LightBuffer::update(const LightData* data, std::size_t count) {
        if (count == 0) {
            return;
        }

        count = std::min(count, capacity_);
        std::memcpy(mapped_, data, count * sizeof(LightData));
    }

    std::size_t LightBuffer::capacity() const {
        return capacity_;
    }
}
