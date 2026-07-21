#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "sq/graphics/buffer.hpp"

namespace sq::graphics {

    // 1頂点が持つデータ。位置（2D）と色のみを持つ最小構成。
    struct Vertex {
        glm::vec3 position;
        glm::vec3 color;
    };

    // 頂点データをGPUメモリに保持するRAIIラッパー。
    // バッファ生成・解放は基底クラスBufferが担い、本クラスは
    // 「構築時に頂点データを一度書き込む」「描画時にバインドする」責務のみ持つ。
    // （コピー禁止・handle()/size()は基底クラスから継承）
    class VertexBuffer : public Buffer {
    public:
        VertexBuffer(VkPhysicalDevice physical_device, VkDevice device, const std::vector<Vertex>& vertices);
        // デストラクタは基底クラスに任せる（追加で解放するリソースは無い）

        // このバッファをコマンドバッファにバインドする（vkCmdBindVertexBuffers）。
        void bind(VkCommandBuffer command_buffer) const;

        [[nodiscard]] std::uint32_t vertex_count() const;

    private:
        std::uint32_t vertex_count_ = 0;
    };

    // インデックスデータをGPUメモリに保持するRAIIラッパー。Buffer基底の3例目。
    class IndexBuffer : public Buffer {
    public:
        IndexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                    const std::vector<std::uint16_t>& indices);  // usage=INDEX_BUFFER_BIT

        void bind(VkCommandBuffer command_buffer) const;  // vkCmdBindIndexBuffer(..., VK_INDEX_TYPE_UINT16)
        [[nodiscard]] std::uint32_t index_count() const;

    private:
        std::uint32_t index_count_ = 0;
    };

}  // namespace sq::graphics
