#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace sq::graphics {

// 1頂点が持つデータ。位置（2D）と色のみを持つ最小構成。
struct Vertex {
    glm::vec2 position;
    glm::vec3 color;
};

// 頂点データをGPUメモリに保持するRAIIラッパー。
// コンストラクタでVkBuffer/VkDeviceMemoryを確保し、デストラクタで解放する。
class VertexBuffer {
public:
    VertexBuffer(VkPhysicalDevice physical_device, VkDevice device, const std::vector<Vertex>& vertices);
    ~VertexBuffer();

    VertexBuffer(const VertexBuffer&) = delete;
    VertexBuffer& operator=(const VertexBuffer&) = delete;

    // このバッファをコマンドバッファにバインドする（vkCmdBindVertexBuffers）。
    void bind(VkCommandBuffer command_buffer) const;

    [[nodiscard]] std::uint32_t vertex_count() const;

private:
    // `type_filter`に合致し、かつ`properties`を満たすメモリタイプのインデックスを探す。
    [[nodiscard]] static std::uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                                          std::uint32_t type_filter,
                                                          VkMemoryPropertyFlags properties);

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    std::uint32_t vertex_count_ = 0;
};

}  // namespace sq::graphics
