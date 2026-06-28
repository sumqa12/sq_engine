#pragma once

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/command_buffers.hpp"
#include "sq/graphics/debug_messenger.hpp"
#include "sq/graphics/device.hpp"
#include "sq/graphics/graphics_pipeline.hpp"
#include "sq/graphics/physical_device.hpp"
#include "sq/graphics/render_pass.hpp"
#include "sq/graphics/swapchain.hpp"
#include "sq/graphics/sync_objects.hpp"
#include "sq/graphics/vulkan_instance.hpp"
#include "sq/graphics/window.hpp"

namespace sq::graphics {

// すべてのVulkanサブシステムを管理し、シーケンスを決定します。構築順序は、
// 標準的なVulkanセットアップシーケンス（フェーズ2の計画を参照）を反映しています：
//   Window -> Instance -> DebugMessenger -> Surface -> PhysicalDevice -> Device
//   -> Swapchain -> RenderPass -> GraphicsPipeline -> Framebuffers
//   -> CommandBuffers -> SyncObjects
class Renderer {
public:
    Renderer(std::uint32_t width, std::uint32_t height, const std::string& app_name);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ウィンドウのクローズフラグが設定されるまで実行され、各反復ごとに draw_frame() を呼び出します。
    void run();

    [[nodiscard]] bool should_close() const;

    // 画像を取得し、フレームのコマンドバッファを記録して送信し、
    // 表示を行います。VK_ERROR_OUT_OF_DATE_KHR が発生した場合は、スワップチェーンを再作成することで対処します。
    void draw_frame();

private:
    void create_surface();
    void create_framebuffers();
    void destroy_framebuffers();
    void recreate_swapchain();

    static constexpr std::size_t kFramesInFlight = 2;

    std::unique_ptr<Window> window_;
    std::unique_ptr<VulkanInstance> instance_;
    std::unique_ptr<DebugMessenger> debug_messenger_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    QueueFamilyIndices queue_family_indices_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<RenderPass> render_pass_;
    std::unique_ptr<GraphicsPipeline> pipeline_;
    std::vector<VkFramebuffer> framebuffers_;
    std::unique_ptr<CommandBuffers> command_buffers_;
    std::unique_ptr<SyncObjects> sync_objects_;

    std::size_t current_frame_ = 0;
};

}  // namespace sq::graphics
