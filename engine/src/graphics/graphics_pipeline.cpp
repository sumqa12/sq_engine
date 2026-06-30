#include "sq/graphics/graphics_pipeline.hpp"

#include <filesystem>
#include <fstream>

namespace sq::graphics {

GraphicsPipeline::GraphicsPipeline(VkDevice device, VkRenderPass render_pass,
                                    VkExtent2D viewport_extent, const std::string& vert_spv_path,
                                    const std::string& frag_spv_path)
    : device_(device) {
    // TODO:
    //  1. 両方のステージに対して load_shader_module() を実行し、VkPipelineShaderStageCreateInfo にラップする。
    //  2. VkPipelineVertexInputStateCreateInfo（最初のハードコーディングされた三角形については空）。
    //  3. VkPipelineInputAssemblyStateCreateInfo（VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST）。
    //  4. viewport_extent からのビューポート／シザー、ラスタライズ、マルチサンプリング、
    //     カラーブレンド状態の構造体。
    //  5. vkCreatePipelineLayout(...) を layout_ に設定（この時点では記述子セットはまだ存在しない）。
    //  6. vkCreateGraphicsPipelines(...) を pipeline_ に設定し、render_pass を参照させる。
    //  7. 一時的なシェーダーモジュールを破棄する（これらは作成時にのみ必要である）。

    // シェーダーモジュールをロードする
    VkShaderModule vert_shader_module = load_shader_module(device_, vert_spv_path);
    VkShaderModule frag_shader_module = load_shader_module(device_, frag_spv_path);

    if (vert_shader_module == VK_NULL_HANDLE || frag_shader_module == VK_NULL_HANDLE) {
        throw std::runtime_error("シェーダーモジュールの読み込みに失敗しました。");
    }

    // シェーダーステージの作成情報を設定する
    VkPipelineShaderStageCreateInfo shader_stages[2]{};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].pName = "main";
    shader_stages[0].module = vert_shader_module;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].pName = "main";
    shader_stages[1].module = frag_shader_module;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;

    // 頂点入力状態の作成情報を設定する
    VkPipelineVertexInputStateCreateInfo vertex_input_info = {};
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_info.vertexAttributeDescriptionCount = 0;
    vertex_input_info.pVertexAttributeDescriptions = nullptr;
    vertex_input_info.vertexBindingDescriptionCount = 0;
    vertex_input_info.pVertexBindingDescriptions = nullptr;

    // 入力アセンブリ状態の作成情報を設定する
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    // ビューポート／シザー状態の作成情報を設定する
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(viewport_extent.width);
    viewport.height = static_cast<float>(viewport_extent.height);

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = viewport_extent;

    VkPipelineViewportStateCreateInfo viewport_state_info = {};
    viewport_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state_info.viewportCount = 1;
    viewport_state_info.pViewports = &viewport;
    viewport_state_info.scissorCount = 1;
    viewport_state_info.pScissors = &scissor;

    // ラスタライズ、マルチサンプリング、カラーブレンド状態の作成情報を設定する。
    VkPipelineRasterizationStateCreateInfo rasterization_state_info = {};
    rasterization_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state_info.depthClampEnable = VK_FALSE;
    rasterization_state_info.rasterizerDiscardEnable = VK_FALSE;
    rasterization_state_info.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state_info.lineWidth = 1.0f;
    rasterization_state_info.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization_state_info.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization_state_info.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample_state_info = {};
    multisample_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state_info.sampleShadingEnable = VK_FALSE;
    multisample_state_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blend_state_info = {};
    color_blend_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state_info.logicOpEnable = VK_FALSE;
    color_blend_state_info.attachmentCount = 1;
    color_blend_state_info.pAttachments = &color_blend_attachment;

    // パイプラインレイアウトを作成する
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 0;
    pipeline_layout_info.pSetLayouts = nullptr;
    vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &layout_);

    // グラフィックスパイプラインを作成する
    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pViewportState = &viewport_state_info;
    pipeline_info.pVertexInputState = &vertex_input_info;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pRasterizationState = &rasterization_state_info;
    pipeline_info.pMultisampleState = &multisample_state_info;
    pipeline_info.pColorBlendState = &color_blend_state_info;
    pipeline_info.layout = layout_;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);

    // 一時的なシェーダーモジュールを破棄する
    vkDestroyShaderModule(device_, vert_shader_module, nullptr);
    vkDestroyShaderModule(device_, frag_shader_module, nullptr);

    (void)render_pass;
    (void)viewport_extent;
    (void)vert_spv_path;
    (void)frag_spv_path;
}

GraphicsPipeline::~GraphicsPipeline() {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    device_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
}

VkPipeline GraphicsPipeline::handle() const {
    return pipeline_;
}

VkPipelineLayout GraphicsPipeline::layout() const {
    return layout_;
}

VkShaderModule GraphicsPipeline::load_shader_module(VkDevice device, const std::string& spv_path) {
    // シェーダーファイルを読み込んで、シェーダーモジュールを作成する
    uintmax_t spv_file_size = 0;
    try {
        std::filesystem::path p = spv_path;
        spv_file_size = std::filesystem::file_size(p);
        printf("File size: %ju bytes\n", spv_file_size);
    } catch (const std::filesystem::filesystem_error& e) {
        printf("File size: %s bytes\n", e.what());
        printf("Path: %ls\n", e.path1().c_str());
    }

    std::ifstream spv_file(spv_path, std::ios::binary);

    std::vector<char> spv(spv_file_size);
    spv_file.read(spv.data(), static_cast<std::streamsize>(spv_file_size));

    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = spv_file_size;
    create_info.pCode = reinterpret_cast<const uint32_t*>(spv.data());

    VkShaderModule shader_module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &create_info, nullptr, &shader_module);

    (void)device;
    (void)spv_path;
    return shader_module;
}

}  // namespace sq::graphics
