#include "sq/graphics/renderer.hpp"

#include <stdexcept>
#include <thread>
#include <GLFW/glfw3.h>

namespace sq::graphics {

Renderer::Renderer(std::uint32_t width, std::uint32_t height, const std::string& app_name) {

    // 1. ウィンドウの作成
    window_ = std::make_unique<Window>(width, height, app_name);

    // 2. ヴァルカンインスタンスの作成
    instance_ = std::make_unique<VulkanInstance>(app_name, Window::required_instance_extensions());

    // 3. デバッグメッセンジャーの作成（バリデーションレイヤーが有効な場合のみ）
    debug_messenger_ = std::make_unique<DebugMessenger>(instance_->handle(), instance_->validation_enabled());

    // 4. サーフェスの作成
    create_surface();  // glfwCreateWindowSurface(instance_->handle(), window_->handle(), ...)

    // 5. 物理デバイスの選択、キューの取得、論理デバイスの作成
    physical_device_ = PhysicalDeviceSelector::select(instance_->handle(), surface_);
    queue_family_indices_ = PhysicalDeviceSelector::find_queue_families(physical_device_, surface_);
    device_ = std::make_unique<Device>(physical_device_, queue_family_indices_, instance_->validation_enabled());

    // 6. スワップチェーンの作成
    swapchain_ = std::make_unique<Swapchain>(instance_->handle(),
                                            physical_device_, device_->handle(), surface_,
                                            width, height);

    // 7. レンダーパスの作成
    render_pass_ = std::make_unique<RenderPass>(device_->handle(), swapchain_->image_format());

    // 8. パイプラインの作成
    pipeline_ = std::make_unique<GraphicsPipeline>(device_->handle(),
                                        render_pass_->handle(), swapchain_->extent(),
                                        "shaders/triangle.vert.spv", "shaders/triangle.frag.spv");

    // 9. フレームバッファの作成
    create_framebuffers();

    // 10. コマンドバッファの作成
    command_buffers_ = std::make_unique<CommandBuffers>(device_->handle(), *queue_family_indices_.graphics_family, framebuffers_.size());

    // 11. 同期オブジェクトの作成
    sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight);

    (void)width;
    (void)height;
    (void)app_name;
}

Renderer::~Renderer() {
    // TODO: メンバーが逆順にアンワインドされる前に vkDeviceWaitIdle(device_->handle()) を実行する
    // 宣言順；destroy_framebuffers() および vkDestroySurfaceKHR(surface_)
    // これらはラッパークラスによって所有されていないため、明示的に呼び出す必要がある。

    vkDeviceWaitIdle(device_->handle());
    destroy_framebuffers();
    vkDestroySurfaceKHR(instance_->handle(), surface_, nullptr);
    sync_objects_.reset();
    command_buffers_.reset();
    pipeline_.reset();
    render_pass_.reset();
    swapchain_.reset();
    device_.reset();
    queue_family_indices_ = {};
    physical_device_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    debug_messenger_.reset();
    instance_.reset();
    window_.reset();
}

void Renderer::run() {
    while (!should_close()) {
        Window::poll_events();
        draw_frame();
    }
}

bool Renderer::should_close() const {
    return window_ == nullptr || window_->should_close();
}

void Renderer::draw_frame() {
    // 1. フェンスの待機
    VkFence in_flight_fence = sync_objects_->in_flight_fence(current_frame_);
    vkWaitForFences(device_->handle(), 1, &in_flight_fence, VK_TRUE, UINT64_MAX);

    // 2. 画像の取得
    uint32_t image_index = 0;
    VkSemaphore image_available = sync_objects_->image_available(current_frame_);
    vkAcquireNextImageKHR(device_->handle(), swapchain_->handle(), UINT64_MAX, image_available, in_flight_fence, &image_index);

    // 3. コマンドバッファの記録と送信
    command_buffers_->record(image_index, [this](VkCommandBuffer command_buffer, std::size_t index) {
        VkRenderPassBeginInfo render_pass_begin_info{};
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.renderPass = render_pass_->handle();
        render_pass_begin_info.framebuffer = framebuffers_[index];
        render_pass_begin_info.renderArea = { {0, 0}, swapchain_->extent() };
        VkClearValue clear_color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        render_pass_begin_info.clearValueCount = 1;
        render_pass_begin_info.pClearValues = &clear_color;

        // レンダーパスの開始
        vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());
        vkCmdDraw(command_buffer, 3, 1, 0, 0);

        // レンダーパスの終了
        vkCmdEndRenderPass(command_buffer);
    });

    // コマンドバッファの送信
    VkSemaphore render_finished = sync_objects_->render_finished(current_frame_);
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = command_buffers_->at(image_index);
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished;
    submit_info.pWaitDstStageMask = new VkPipelineStageFlags{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

    vkQueueSubmit(device_->graphics_queue(), 1, &submit_info, in_flight_fence);

    // 4. 画像の提示
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished;
    present_info.pImageIndices = &image_index;

    vkQueuePresentKHR(device_->graphics_queue(), &present_info);

    vkWaitForFences(device_->handle(), 1, &in_flight_fence, VK_TRUE, UINT64_MAX);

    current_frame_ = (current_frame_ + 1) % kFramesInFlight;
}

void Renderer::create_surface() {
    glfwCreateWindowSurface(instance_->handle(), window_->handle(), nullptr, &surface_);
}

void Renderer::create_framebuffers() {
    for (const auto& image_view : swapchain_->image_views()) {
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = render_pass_->handle();
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &image_view;
        framebuffer_info.width = swapchain_->extent().width;
        framebuffer_info.height = swapchain_->extent().height;
        framebuffer_info.layers = 1;

        VkFramebuffer framebuffer;
        if (vkCreateFramebuffer(device_->handle(), &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) {
            throw std::runtime_error("フレームバッファの作成に失敗しました！");
        }
        framebuffers_.push_back(framebuffer);
    }
}

void Renderer::destroy_framebuffers() {
    for (const auto& framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_->handle(), framebuffer, nullptr);
    }

    framebuffers_.clear();
}

void Renderer::recreate_swapchain() {
    vkDeviceWaitIdle(device_->handle());
    destroy_framebuffers();
    swapchain_->recreate(window_->width(), window_->height());
    create_framebuffers();
}

}  // namespace sq::graphics
