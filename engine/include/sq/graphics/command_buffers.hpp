#pragma once

#include <functional>
#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// VkCommandPool と、固定サイズのプライマリ VkCommandBuffer のセットを保持しており、これらは
// スワップチェーンの画像／処理中のフレームのスロットごとに1つずつ割り当てられる。
class CommandBuffers {
public:
    using RecordFn = std::function<void(VkCommandBuffer, std::size_t /*index*/)>;

    CommandBuffers(VkDevice device, std::uint32_t graphics_queue_family, std::size_t count);
    ~CommandBuffers();

    CommandBuffers(const CommandBuffers&) = delete;
    CommandBuffers& operator=(const CommandBuffers&) = delete;

    // `index` にあるバッファをリセットし、`record` を通じて再記録します。
    // これにより、vkCmdBeginRenderPass / 描画コール / vkCmdEndRenderPass が呼び出されることが想定されています。
    void record(std::size_t index, const RecordFn& record);

    [[nodiscard]] VkCommandBuffer at(std::size_t index) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> buffers_;
};

}  // namespace sq::graphics
