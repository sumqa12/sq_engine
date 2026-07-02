#include "sq/graphics/renderer.hpp"

#include <stdexcept>
#include <thread>
#include <GLFW/glfw3.h>

#include "sq/scene/camera.hpp"
#include "sq/scene/transform.hpp"

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

    // 8. ディスクリプタセットレイアウトの作成（カメラUBO用、set=0）
    create_descriptor_set_layout();

    // 9. UBOの作成
    create_uniform_buffers();

    // 10. ディスクリプタプールの作成
    create_descriptor_pool();
    create_descriptor_sets();

    // 11. パイプラインの作成
    pipeline_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
                                        "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
                                        descriptor_set_layout_);

    // 12. フレームバッファの作成
    create_framebuffers();

    // 13. コマンドバッファの作成
    command_buffers_ = std::make_unique<CommandBuffers>(device_->handle(), *queue_family_indices_.graphics_family, framebuffers_.size());

    // 14. 同期オブジェクトの作成
    sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight, framebuffers_.size());

    // 15. デモ用三角形メッシュの作成（ECS連携）
    create_triangle_mesh();

    (void)width;
    (void)height;
    (void)app_name;
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(device_->handle());
    destroy_framebuffers();
    triangle_mesh_.reset();
    sync_objects_.reset();
    command_buffers_.reset();
    vkDestroyDescriptorPool(device_->handle(), descriptor_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_->handle(), descriptor_set_layout_, nullptr);
    camera_ubos_.clear();
    descriptor_sets_.clear();
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    pipeline_.reset();
    render_pass_.reset();
    swapchain_.reset();
    vkDestroySurfaceKHR(instance_->handle(), surface_, nullptr);
    device_.reset();
    queue_family_indices_ = {};
    physical_device_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    debug_messenger_.reset();
    instance_.reset();
    window_.reset();
}

void Renderer::run(sq::ecs::Registry& registry) {
    while (!should_close()) {
        Window::poll_events();
        draw_frame(registry);
    }
}

bool Renderer::should_close() const {
    return window_ == nullptr || window_->should_close();
}

void Renderer::draw_frame(const ecs::Registry& registry) {
    // 1. フェンスの待機
    VkFence in_flight_fence = sync_objects_->in_flight_fence(current_frame_);
    vkWaitForFences(device_->handle(), 1, &in_flight_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_->handle(), 1, &in_flight_fence);

    // 2. 画像の取得
    uint32_t image_index = 0;
    VkSemaphore image_available = sync_objects_->image_available(current_frame_);
    if (VkResult result = vkAcquireNextImageKHR(device_->handle(), swapchain_->handle(), UINT64_MAX, image_available, VK_NULL_HANDLE, &image_index); result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }

    // 3. コマンドバッファの記録と送信
    command_buffers_->record(image_index, [this, &registry](VkCommandBuffer command_buffer, std::size_t index) {
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

        // メッシュのバインド
        triangle_mesh_->bind(command_buffer);

        // カメラの取得
        ecs::Entity camera_entity = registry.view<scene::Camera>().front();
        const scene::Camera& camera = registry.get<scene::Camera>(camera_entity);

        // アスペクト比の計算
        float aspect_ratio = static_cast<float>(swapchain_->extent().width) / static_cast<float>(swapchain_->extent().height);

        // カメラUBOの更新
        scene::CameraUBO camera_ubo{};
        camera_ubo.view_projection = camera.view_projection(aspect_ratio);
        camera_ubos_[current_frame_]->update(&camera_ubo, sizeof(camera_ubo));

        // ディスクリプタセットのバインド
        VkDescriptorSet descriptor_set = descriptor_sets_[current_frame_];
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0, 1, &descriptor_set, 0, nullptr);

        // 描画
        registry.view<scene::Transform>().each([&](ecs::Entity, scene::Transform& t) {
            vkCmdPushConstants(command_buffer, pipeline_->layout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &t.model);
            vkCmdDraw(command_buffer, triangle_mesh_->vertex_count(), 1, 0, 0);
        });

        // レンダーパスの終了
        vkCmdEndRenderPass(command_buffer);
    });

    // コマンドバッファの送信
    VkSemaphore render_finished = sync_objects_->render_finished(image_index);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = command_buffers_->at(image_index);
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished;
    submit_info.pWaitDstStageMask = &wait_stage;

    vkQueueSubmit(device_->graphics_queue(), 1, &submit_info, in_flight_fence);

    // 4. 画像の提示
    VkSwapchainKHR swapchain_handle = swapchain_->handle();
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished;
    present_info.pImageIndices = &image_index;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_handle;

    if (VkResult result =
        vkQueuePresentKHR(device_->graphics_queue(), &present_info);
        result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        window_->consume_resized_flag()) {
        recreate_swapchain();
    }

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

// ディスクリプタセットレイアウトの作成
void Renderer::create_descriptor_set_layout() {
    VkDescriptorSetLayoutBinding ubo_layout_binding{};
    ubo_layout_binding.binding = 0;
    ubo_layout_binding.descriptorCount = 1;
    ubo_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info{};
    descriptor_set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_set_layout_info.bindingCount = 1;
    descriptor_set_layout_info.pBindings = &ubo_layout_binding;

    vkCreateDescriptorSetLayout(device_->handle(), &descriptor_set_layout_info, nullptr, &descriptor_set_layout_);
}

// UBOの作成
void Renderer::create_uniform_buffers() {
    for (std::size_t i = 0; i < kFramesInFlight; ++i) {
        camera_ubos_.push_back(std::make_unique<UniformBuffer>(physical_device_, device_->handle(), sizeof(scene::CameraUBO)));
    }
}

// ディスクリプタプールの作成
void Renderer::create_descriptor_pool() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_size.descriptorCount = static_cast<uint32_t>(kFramesInFlight);

    VkDescriptorPoolCreateInfo descriptor_pool_info{};
    descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_info.maxSets = kFramesInFlight;
    descriptor_pool_info.poolSizeCount = 1;
    descriptor_pool_info.pPoolSizes = &pool_size;

    vkCreateDescriptorPool(device_->handle(), &descriptor_pool_info, nullptr, &descriptor_pool_);
}

void Renderer::create_descriptor_sets() {
    descriptor_sets_.resize(kFramesInFlight);
    for (std::size_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &descriptor_set_layout_;

        vkAllocateDescriptorSets(device_->handle(), &alloc_info, &descriptor_sets_[i]);

        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = camera_ubos_[i]->handle();
        buffer_info.offset = 0;
        buffer_info.range = sizeof(scene::CameraUBO);

        VkWriteDescriptorSet descriptor_write{};
        descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_write.dstSet = descriptor_sets_[i];
        descriptor_write.dstBinding = 0;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pBufferInfo = &buffer_info;

        vkUpdateDescriptorSets(device_->handle(), 1, &descriptor_write, 0, nullptr);;
    }
}

// vec2 positions[3] = vec2[](
//     vec2(0.0, -0.5),
//     vec2(0.5, 0.5),
//     vec2(-0.5, 0.5)
// );
//
// vec3 colors[3] = vec3[](
//     vec3(1.0, 0.0, 0.0),
//     vec3(0.0, 1.0, 0.0),
//     vec3(0.0, 0.0, 1.0)
// );
void Renderer::create_triangle_mesh() {
    std::vector<Vertex> vertices = {
        {{0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}},
        {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
        {{0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}}
    };
    triangle_mesh_ = std::make_unique<VertexBuffer>(physical_device_, device_->handle(), vertices);
}

void Renderer::recreate_swapchain() {
    window_->wait_while_minimized();
    vkDeviceWaitIdle(device_->handle());
    destroy_framebuffers();
    swapchain_->recreate(window_->width(), window_->height());
    create_framebuffers();

    // 同期オブジェクトの再作成
    sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight, framebuffers_.size());
}

}  // namespace sq::graphics
