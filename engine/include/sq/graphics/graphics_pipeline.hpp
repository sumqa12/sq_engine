#pragma once

#include <string>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// シェーダーステージ、頂点入力レイアウト、ラスタライズ、ブレンディングなどを、
// 単一の不変の VkPipeline に組み込みます。Vulkan には、GL とは異なり、グローバルなレンダリング状態はありません。
class GraphicsPipeline {
public:
    // descriptor_set_layout: パイプラインレイアウトに組み込むディスクリプタセット
    // レイアウト（カメラUBO用、set=0）。呼び出し側が所有・破棄する（ここでは破棄しない）。
    GraphicsPipeline(VkDevice device, VkRenderPass render_pass, VkExtent2D viewport_extent,
                      const std::string& vert_spv_path, const std::string& frag_spv_path,
                      VkDescriptorSetLayout descriptor_set_layout);
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const;
    [[nodiscard]] VkPipelineLayout layout() const;

private:
    [[nodiscard]] static VkShaderModule load_shader_module(VkDevice device,
                                                             const std::string& spv_path);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
