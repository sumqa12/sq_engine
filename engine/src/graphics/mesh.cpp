#include "sq/graphics/mesh.hpp"

namespace sq::graphics {
    VertexBuffer::VertexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                                const std::vector<Vertex>& vertices)
        : Buffer(physical_device, device,
                 sizeof(Vertex) * vertices.size(),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            vertex_count_(static_cast<std::uint32_t>(vertices.size())) {
        // 基底クラスが確保したメモリへ頂点データを書き込む:
        //  1. map() でマップし、std::memcpy(<マップ先>, vertices.data(), size()) で書き込む（<cstring>が必要）
        //  2. 書き込みは構築時の一度きりなので unmap() する（HOST_COHERENTなのでflush不要）
        void* mapped = map();
        std::memcpy(mapped, vertices.data(), size());
        unmap();
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

    IndexBuffer::IndexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                            const std::vector<std::uint16_t> &indices)
        : Buffer(physical_device, device,
            sizeof(std::uint16_t) * indices.size(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            index_count_(static_cast<std::uint32_t>(indices.size())) {

        void* mapped = map();
        std::memcpy(mapped, indices.data(), size());
        unmap();
    }

    void IndexBuffer::bind(VkCommandBuffer command_buffer) const {
        vkCmdBindIndexBuffer(command_buffer, buffer_, 0, VK_INDEX_TYPE_UINT16);
    }

    std::uint32_t IndexBuffer::index_count() const {
        return index_count_;
    }
}  // namespace sq::graphics
