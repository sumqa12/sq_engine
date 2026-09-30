#include "sq/graphics/graphics_pipeline.hpp"

#include <array>
#include <filesystem>

#include "sq/graphics/mesh.hpp"
#include "sq/graphics/shader_module.hpp"

namespace sq::graphics {

GraphicsPipeline::GraphicsPipeline(VkDevice device, VkRenderPass render_pass, VkExtent2D viewport_extent,
                                    const std::string& vert_spv_path, const std::string& frag_spv_path,
                                    const std::vector<VkDescriptorSetLayout>& set_layouts,
                                    const PipelineConfig& config)
    : device_(device) {

    const bool has_frag = !frag_spv_path.empty();

    // シェーダーモジュールをロードする
    VkShaderModule vert_shader_module = load_shader_module(device_, vert_spv_path);
    VkShaderModule frag_shader_module = VK_NULL_HANDLE;
    if (has_frag) {
        frag_shader_module = load_shader_module(device_, frag_spv_path);
    }

    if (vert_shader_module == VK_NULL_HANDLE || (has_frag && frag_shader_module == VK_NULL_HANDLE)) {
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
    VkVertexInputBindingDescription binding_description = {};
    binding_description.binding = 0;
    binding_description.stride = sizeof(Vertex);
    binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // UV座標を第3の頂点属性として追加する（phase8プラン 項目8）
    // tangent（glm::vec4）を4番目の頂点属性として追加する。(phase15)
    std::array<VkVertexInputAttributeDescription, 4> attribute_descriptions = {};
    attribute_descriptions[0].binding = 0;
    attribute_descriptions[0].location = 0;
    attribute_descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3 position
    attribute_descriptions[0].offset = offsetof(Vertex, position);
    attribute_descriptions[1].binding = 0;
    attribute_descriptions[1].location = 1;
    attribute_descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3 normal
    attribute_descriptions[1].offset = offsetof(Vertex, normal);
    attribute_descriptions[2].binding = 0;
    attribute_descriptions[2].location = 2;
    attribute_descriptions[2].format = VK_FORMAT_R32G32_SFLOAT; // vec2 uv
    attribute_descriptions[2].offset = offsetof(Vertex, uv);
    attribute_descriptions[3].binding = 0;
    attribute_descriptions[3].location = 3;
    attribute_descriptions[3].format = VK_FORMAT_R32G32B32A32_SFLOAT; // vec4 tangent
    attribute_descriptions[3].offset = offsetof(Vertex, tangent);

    VkPipelineVertexInputStateCreateInfo vertex_input_info = {};
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (config.has_vertex_input) {
        vertex_input_info.vertexBindingDescriptionCount = 1;
        vertex_input_info.pVertexBindingDescriptions = &binding_description;
        vertex_input_info.vertexAttributeDescriptionCount = attribute_descriptions.size();
        vertex_input_info.pVertexAttributeDescriptions = attribute_descriptions.data();
    } else {
        vertex_input_info.vertexBindingDescriptionCount = 0;
        vertex_input_info.pVertexBindingDescriptions = nullptr;
        vertex_input_info.vertexAttributeDescriptionCount = 0;
        vertex_input_info.pVertexAttributeDescriptions = nullptr;
    }

    // 入力アセンブリ状態の作成情報を設定する
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    // ビューポート／シザー状態の作成情報を設定する
    //   ★ 値は記録時に vkCmdSetDepthBias で渡す（①-6 手順5）。
    //     動的ステートに加えたのに呼び忘れると未定義動作になる。
    std::vector<VkDynamicState> dynamic_states;
    if (config.depth_bias_enable) {
        dynamic_states.push_back(VK_DYNAMIC_STATE_VIEWPORT);
        dynamic_states.push_back(VK_DYNAMIC_STATE_SCISSOR);
        dynamic_states.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    } else {
        dynamic_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    }
    VkPipelineDynamicStateCreateInfo dynamic_state_info = {};
    dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_info.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic_state_info.pDynamicStates = dynamic_states.data();

    VkPipelineViewportStateCreateInfo viewport_state_info = {};
    viewport_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state_info.viewportCount = 1;
    viewport_state_info.scissorCount = 1;

    // ラスタライズ、マルチサンプリング、カラーブレンド状態の作成情報を設定する。
    VkPipelineRasterizationStateCreateInfo rasterization_state_info = {};
    rasterization_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state_info.depthClampEnable = VK_FALSE;
    rasterization_state_info.rasterizerDiscardEnable = VK_FALSE;
    rasterization_state_info.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state_info.lineWidth = 1.0f;
    rasterization_state_info.cullMode = config.cull_mode;
    //   glTF の仕様は**反時計回り（CCW）が表**。現状の CLOCKWISE のままだと
    //   読み込んだモデルが裏返り、背面カリングで**面が全部消える**（透明になったように見える）。
    rasterization_state_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    //   ★ frontFace は変えないこと。表裏の定義（glTF は CCW が表）はモデル側の話で、
    //     パスごとに変わるものではない。シャドウパスで「裏面を描く」のは
    //     cullMode を FRONT にして表を捨てるという意味であって、frontFace の反転ではない。
    rasterization_state_info.depthBiasEnable = config.depth_bias_enable ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample_state_info = {};
    multisample_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state_info.sampleShadingEnable = VK_FALSE;
    multisample_state_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // phase11 ①: blend の有無を config で切り替える（不透明=OFF / 半透明=ON）。
    // 有効時は標準的なオーバー合成（アルファブレンド）(phase8)。
    color_blend_attachment.blendEnable = config.blend_enable ? VK_TRUE : VK_FALSE;
    color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
    color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    color_blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;

    //   ★ レンダーパスの subpass.colorAttachmentCount と一致していなければならない。
    //     食い違うと vkCreateGraphicsPipelines がバリデーションエラーを出す。
    VkPipelineColorBlendStateCreateInfo color_blend_state_info = {};
    color_blend_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state_info.logicOpEnable = VK_FALSE;
    if (config.has_color_attachment) {
        color_blend_state_info.attachmentCount = 1;
        color_blend_state_info.pAttachments = &color_blend_attachment;
    } else {
        color_blend_state_info.attachmentCount = 0;
        color_blend_state_info.pAttachments = nullptr;
    }

    // 深度ステンシルステート
    VkPipelineDepthStencilStateCreateInfo depth_stencil_state_info = {};
    depth_stencil_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state_info.depthTestEnable = VK_TRUE;
    // phase11 ①: 深度書き込みを config で切り替える（不透明=TRUE / 半透明=FALSE）。
    depth_stencil_state_info.depthWriteEnable = config.depth_write_enable ? VK_TRUE : VK_FALSE;
    // depthCompareOp <- config.depth_compare_op（スカイボックスで LESS_OR_EQUAL。②-9）
    depth_stencil_state_info.depthCompareOp = config.depth_compare_op;  // 小さい深度=手前が勝つ
    depth_stencil_state_info.depthBoundsTestEnable = VK_FALSE;
    depth_stencil_state_info.stencilTestEnable = VK_FALSE;

    // パイプラインレイアウトを作成する
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.pushConstantRangeCount = 0;
    // phase12 手順3: set=0（カメラUBO）と set=1（マテリアル）の2つを組み込む。
    pipeline_layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    pipeline_layout_info.pSetLayouts = set_layouts.data();
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
    pipeline_info.pDepthStencilState = &depth_stencil_state_info;
    pipeline_info.pDynamicState = &dynamic_state_info;
    pipeline_info.layout = layout_;
    pipeline_info.stageCount = has_frag ? 2 : 1;
    pipeline_info.pStages = shader_stages;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);

    // 一時的なシェーダーモジュールを破棄する
    vkDestroyShaderModule(device_, vert_shader_module, nullptr);
    if (has_frag) {
        vkDestroyShaderModule(device_, frag_shader_module, nullptr);
    }

    (void)viewport_extent;
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

}  // namespace sq::graphics
