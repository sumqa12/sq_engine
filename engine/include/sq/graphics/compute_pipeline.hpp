#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// コンピュートパイプラインの RAII ラッパー（phase16 ②-3 / D-7。このエンジンで初）。
//
// GraphicsPipeline に比べて劇的に短い。要るのは:
//   - シェーダモジュール1本（VK_SHADER_STAGE_COMPUTE_BIT）
//   - パイプラインレイアウト（ディスクリプタセットレイアウト + プッシュ定数）
//   - vkCreateComputePipelines
// ラスタライズ状態・ビューポート・ブレンド・深度・頂点入力が**すべて無い**。
class ComputePipeline {
public:
    // set_layouts: 呼び出し側が所有・破棄する（GraphicsPipeline と同じ）。
    // push_constant_size: 0 なら pushConstantRangeCount = 0。
    //   ★ ここではプッシュ定数を使ってよい（D-3 で本パスに使わないと決めたのとは別の話。
    //     本パスはセットのレイアウト互換を保つために避けたが、前計算のコンピュートは
    //     本パスとセットを共有しない）。
    //     prefiltered specular（手順6）が「今どの mip を焼いているか（= roughness）」を渡すのに要る。
    //   ★ stageFlags は VK_SHADER_STAGE_COMPUTE_BIT、offset = 0。
    //     size は 4 の倍数でなければならない（float 1 個なら 4）。
    ComputePipeline(VkDevice device, const std::string& comp_spv_path,
                    const std::vector<VkDescriptorSetLayout>& set_layouts,
                    std::uint32_t push_constant_size);
    ~ComputePipeline();  // pipeline -> layout の順で破棄

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const;
    [[nodiscard]] VkPipelineLayout layout() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
