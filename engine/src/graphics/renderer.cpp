#include "sq/graphics/renderer.hpp"

namespace sq::graphics {

Renderer::Renderer(std::uint32_t width, std::uint32_t height, const std::string& app_name) {
    // TODO, in this order:
    //  1. window_ = make_unique<Window>(width, height, app_name);
    //  2. instance_ = make_unique<VulkanInstance>(app_name, Window::required_instance_extensions());
    //  3. debug_messenger_ = make_unique<DebugMessenger>(instance_->handle(), instance_->validation_enabled());
    //  4. create_surface();  // glfwCreateWindowSurface(instance_->handle(), window_->handle(), ...)
    //  5. physical_device_ = PhysicalDeviceSelector::select(instance_->handle(), surface_);
    //     queue_family_indices_ = PhysicalDeviceSelector::find_queue_families(physical_device_, surface_);
    //  6. device_ = make_unique<Device>(physical_device_, queue_family_indices_, instance_->validation_enabled());
    //  7. swapchain_ = make_unique<Swapchain>(instance_->handle(), physical_device_, device_->handle(), surface_, width, height);
    //  8. render_pass_ = make_unique<RenderPass>(device_->handle(), swapchain_->image_format());
    //  9. pipeline_ = make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(), "shaders/triangle.vert.spv", "shaders/triangle.frag.spv");
    // 10. create_framebuffers();
    // 11. command_buffers_ = make_unique<CommandBuffers>(device_->handle(), *queue_family_indices_.graphics_family, framebuffers_.size());
    // 12. sync_objects_ = make_unique<SyncObjects>(device_->handle(), kFramesInFlight);
    (void)width;
    (void)height;
    (void)app_name;
}

Renderer::~Renderer() {
    // TODO: vkDeviceWaitIdle(device_->handle()) before members unwind in reverse
    // declaration order; destroy_framebuffers() and vkDestroySurfaceKHR(surface_)
    // need explicit calls since they aren't owned by a wrapper class.
}

void Renderer::run() {
    while (!should_close()) {
        window_->poll_events();
        draw_frame();
    }
}

bool Renderer::should_close() const {
    return window_ == nullptr || window_->should_close();
}

void Renderer::draw_frame() {
    // TODO:
    //  1. vkWaitForFences on sync_objects_->in_flight_fence(current_frame_).
    //  2. vkAcquireNextImageKHR using sync_objects_->image_available(current_frame_);
    //     on VK_ERROR_OUT_OF_DATE_KHR call recreate_swapchain() and return.
    //  3. command_buffers_->record(image_index, lambda that begins render_pass_,
    //     binds pipeline_, issues vkCmdDraw(3, 1, 0, 0), ends render pass).
    //  4. vkQueueSubmit on device_->graphics_queue(), waiting on image_available,
    //     signaling render_finished, fencing in_flight_fence.
    //  5. vkQueuePresentKHR on device_->present_queue(), waiting on render_finished.
    //  6. current_frame_ = (current_frame_ + 1) % kFramesInFlight;
}

void Renderer::create_surface() {
    // TODO: glfwCreateWindowSurface(instance_->handle(), window_->handle(), nullptr, &surface_);
}

void Renderer::create_framebuffers() {
    // TODO: one VkFramebuffer per swapchain image view, all sharing render_pass_->handle().
}

void Renderer::destroy_framebuffers() {
    // TODO: vkDestroyFramebuffer for each entry in framebuffers_, then framebuffers_.clear().
}

void Renderer::recreate_swapchain() {
    // TODO: vkDeviceWaitIdle, destroy_framebuffers(), swapchain_->recreate(new width, new height),
    // then create_framebuffers() again (extent may have changed).
}

}  // namespace sq::graphics
