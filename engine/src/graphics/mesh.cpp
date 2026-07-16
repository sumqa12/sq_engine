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

}  // namespace sq::graphics
