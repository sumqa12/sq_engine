#include "sq/graphics/mesh.hpp"

namespace sq::graphics {
    VertexBuffer::VertexBuffer(GpuAllocator& allocator, VkDevice device,
                                std::uint32_t queue_family, VkQueue queue,
                                const std::vector<Vertex>& vertices)
        : Buffer(allocator, device,
                 sizeof(Vertex) * vertices.size(),
                 // phase11 ②: TRANSFER_DST を追加し、properties を DEVICE_LOCAL に変更する。
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            vertex_count_(static_cast<std::uint32_t>(vertices.size())) {
        // DEVICE_LOCAL は map() できないので直書き（旧 map/memcpy/unmap）は不可。
        // staging 経由で転送する
        upload_with_staging(allocator, queue_family, queue, vertices.data(), size());
    }

    void VertexBuffer::bind(VkCommandBuffer command_buffer) const {
        VkBuffer buffers[] = {buffer_};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(command_buffer, 0, 1, buffers, offsets);
    }

    std::uint32_t VertexBuffer::vertex_count() const {
        return vertex_count_;
    }

    // ----- IndexBuffer -----

    IndexBuffer::IndexBuffer(GpuAllocator& allocator, VkDevice device,
                            std::uint32_t queue_family, VkQueue queue,
                            const std::vector<std::uint16_t> &indices)
        : Buffer(allocator, device,
            sizeof(std::uint16_t) * indices.size(),
            // phase11 ②: TRANSFER_DST を追加し、properties を DEVICE_LOCAL に変更する。
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            index_count_(static_cast<std::uint32_t>(indices.size())) {

        upload_with_staging(allocator, queue_family, queue, indices.data(), size());
    }

    void IndexBuffer::bind(VkCommandBuffer command_buffer) const {
        vkCmdBindIndexBuffer(command_buffer, buffer_, 0, VK_INDEX_TYPE_UINT16);
    }

    std::uint32_t IndexBuffer::index_count() const {
        return index_count_;
    }
}  // namespace sq::graphics
