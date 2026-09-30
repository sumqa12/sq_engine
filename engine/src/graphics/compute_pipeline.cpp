#include "sq/graphics/compute_pipeline.hpp"

#include <stdexcept>

#include "sq/graphics/shader_module.hpp"

namespace sq::graphics {

ComputePipeline::ComputePipeline(VkDevice device, const std::string& comp_spv_path,
                                 const std::vector<VkDescriptorSetLayout>& set_layouts,
                                 std::uint32_t push_constant_size)
    : device_(device) {

    VkShaderModule module = load_shader_module(device_, comp_spv_path);

    if (module == VK_NULL_HANDLE) {
        throw std::runtime_error("ComputePipeline::ComputePipeline : シェーダーモジュールの読み込みに失敗しました。");
    }

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<std::uint32_t>(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = push_constant_size > 0 ? static_cast<std::uint32_t>(1) : static_cast<std::uint32_t>(0)
    };

    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = push_constant_size
    };
    if (push_constant_size > 0) {
        pipeline_layout_info.pPushConstantRanges = &push_constant_range;
    }

    if (vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, module, nullptr);
        throw std::runtime_error("ComputePipeline::ComputePipeline : パイプラインレイアウトの作成に失敗しました。");
    }

    //   3. VkComputePipelineCreateInfo
    //        stage = { COMPUTE_BIT, module, pName = "main" }
    //        layout = layout_
    //      → vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, ...)（失敗で throw）
    VkComputePipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main"
        },
        .layout = layout_
    };

    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, module, nullptr);
        throw std::runtime_error("ComputePipeline::ComputePipeline : パイプラインの作成に失敗しました。");
    }

    //   4. vkDestroyShaderModule(module)   ★ 例外経路でもリークしないよう、throw の前に破棄すること
    vkDestroyShaderModule(device_, module, nullptr);
}

ComputePipeline::~ComputePipeline() {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    device_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
}

VkPipeline ComputePipeline::handle() const {
    return pipeline_;
}

VkPipelineLayout ComputePipeline::layout() const {
    return layout_;
}

}  // namespace sq::graphics
