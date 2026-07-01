#pragma once

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/ecs/registry.hpp"
#include "sq/graphics/command_buffers.hpp"
#include "sq/graphics/debug_messenger.hpp"
#include "sq/graphics/device.hpp"
#include "sq/graphics/graphics_pipeline.hpp"
#include "sq/graphics/mesh.hpp"
#include "sq/graphics/physical_device.hpp"
#include "sq/graphics/render_pass.hpp"
#include "sq/graphics/swapchain.hpp"
#include "sq/graphics/sync_objects.hpp"
#include "sq/graphics/uniform_buffer.hpp"
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

    // ウィンドウのクローズフラグが設定されるまで実行され、各反復ごとに draw_frame(registry) を呼び出します。
    // 描画対象のエンティティはregistryから都度問い合わせる（Rendererはエンティティを所有しない）。
    void run(sq::ecs::Registry& registry);

    [[nodiscard]] bool should_close() const;

    // 画像を取得し、registry内のTransformを持つ各エンティティについてコマンドバッファに描画命令を記録して送信し、
    // 表示を行います。VK_ERROR_OUT_OF_DATE_KHR が発生した場合は、スワップチェーンを再作成することで対処します。
    void draw_frame(const sq::ecs::Registry& registry);

private:
    void create_surface();
    void create_framebuffers();
    void destroy_framebuffers();
    void recreate_swapchain();
    void create_triangle_mesh();
    // カメラUBO用ディスクリプタ一式（パイプライン作成の前にレイアウトが必要）。
    void create_descriptor_set_layout();  // vkCreateDescriptorSetLayout（set=0, binding=0, UNIFORM_BUFFER, VERTEX）
    void create_uniform_buffers();        // camera_ubos_をkFramesInFlight個作成
    void create_descriptor_pool();        // vkCreateDescriptorPool（UNIFORM_BUFFERをkFramesInFlight個）
    void create_descriptor_sets();        // vkAllocateDescriptorSets + vkUpdateDescriptorSetsで各UBOと結びつける

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
    std::unique_ptr<VertexBuffer> triangle_mesh_;  // デモ用の共有メッシュ（全エンティティがこれを描画する）

    // カメラUBO用ディスクリプタ（すべてkFramesInFlight個。スワップチェーン画像枚数には非依存）。
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<UniformBuffer>> camera_ubos_;
    std::vector<VkDescriptorSet> descriptor_sets_;  // プールから確保（個別破棄は不要、プール破棄でまとめて解放）

    std::size_t current_frame_ = 0;
};

}  // namespace sq::graphics
