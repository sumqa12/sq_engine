#include "sq/graphics/render_pass.hpp"

#include <vector>

namespace sq::graphics {

RenderPass::RenderPass(VkDevice device, VkFormat color_format, VkFormat depth_format,
                       const RenderPassConfig& config) : device_(device) {
    // アタッチメントの説明を作成する
    VkAttachmentDescription color_attachment{};
    color_attachment.format = color_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = depth_format;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = config.depth_store_op;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = config.depth_final_layout;

    std::vector<VkAttachmentDescription> attachments;
    if (config.has_color) {
        attachments.push_back(color_attachment);
    }
    attachments.push_back(depth_attachment);

    // サブパスのカラーアタッチメント参照を作成する
    VkAttachmentReference color_attachment_reference{};
    color_attachment_reference.attachment = 0;
    color_attachment_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_attachment_reference{};
    depth_attachment_reference.attachment = config.has_color ? 1 : 0;
    depth_attachment_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // サブパスの説明を作成する
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (config.has_color) {
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_attachment_reference;
    } else {
        subpass.colorAttachmentCount = 0;
        subpass.pColorAttachments = nullptr;
    }
    subpass.pDepthStencilAttachment = &depth_attachment_reference;

    //   サブパス依存を config.has_color で切り替える（深度専用のときは2本）:
    //        [0] EXTERNAL -> 0 : src = FRAGMENT_SHADER / SHADER_READ
    //                            dst = EARLY_FRAGMENT_TESTS / DEPTH_STENCIL_ATTACHMENT_WRITE
    //            （前フレームの読み取りが終わってから書き始める）
    //        [1] 0 -> EXTERNAL : src = LATE_FRAGMENT_TESTS / DEPTH_STENCIL_ATTACHMENT_WRITE
    //                            dst = FRAGMENT_SHADER / SHADER_READ
    //            （書き終わってから本パスの frag が読む）
    // サブパス間の依存関係を作成する
    std::vector<VkSubpassDependency> dependencies{};
    if (config.has_color) {
        dependencies.resize(1);
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    } else {
        dependencies.resize(2);
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    // レンダーパスの作成情報を設定する
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    render_pass_info.pDependencies = dependencies.data();

    // レンダーパスを作成する
    vkCreateRenderPass(device_, &render_pass_info, nullptr, &render_pass_);
}

RenderPass::~RenderPass() {
    if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
    render_pass_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

VkRenderPass RenderPass::handle() const {
    return render_pass_;
}

}  // namespace sq::graphics
