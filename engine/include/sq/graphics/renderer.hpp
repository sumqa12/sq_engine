#pragma once

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "depth_image.hpp"
#include "sq/ecs/registry.hpp"
#include "sq/graphics/command_buffers.hpp"
#include "sq/graphics/debug_messenger.hpp"
#include "sq/graphics/device.hpp"
#include "sq/graphics/graphics_pipeline.hpp"
#include "sq/graphics/mesh.hpp"
#include "sq/graphics/mesh_registry.hpp"
#include "sq/graphics/physical_device.hpp"
#include "sq/graphics/render_pass.hpp"
#include "sq/graphics/sampler.hpp"
#include "sq/graphics/swapchain.hpp"
#include "sq/graphics/sync_objects.hpp"
#include "sq/graphics/texture.hpp"
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

    [[nodiscard]] bool should_close() const;

    // 画像を取得し、registry内のTransformを持つ各エンティティについてコマンドバッファに描画命令を記録して送信し、
    // 表示を行います。VK_ERROR_OUT_OF_DATE_KHR が発生した場合は、スワップチェーンを再作成することで対処します。
    void draw_frame(const sq::ecs::Registry& registry);

    // キー入力の状態を返す（Windowへの委譲）。keyはGLFW_KEY_*
    [[nodiscard]] bool is_key_pressed(int key) const;

    // 入力コールバックの配線（Windowへの委譲）。アプリ側のInputManagerへ接続するために使う。
    void set_key_callback(Window::KeyCallback callback);
    void set_cursor_pos_callback(Window::CursorPosCallback callback);
    void set_mouse_button_callback(Window::MouseButtonCallback callback);
    void set_scroll_callback(Window::ScrollCallback callback);
    // マウス視線用のカーソルキャプチャ（Windowへの委譲）。
    void set_cursor_captured(bool captured);

    // -- windowの委譲メソッド --

    // フルスクリーン切替の公開API。Window切替 + 次フレームのrecreateで排他モードを取得する。
    void set_fullscreen(bool enabled);
    [[nodiscard]] bool is_fullscreen() const;

    [[nodiscard]]bool is_focused() const;

    // メッシュの登録・参照（phase12 手順2）。アプリ側が起動時に
    // graphics::add_cube_mesh(renderer.meshes()) 等で登録し、得た MeshId を
    // scene::MeshHandle コンポーネントに入れる。
    [[nodiscard]] MeshRegistry& meshes();

private:
    void create_surface();
    void create_framebuffers();
    void destroy_framebuffers();
    void recreate_swapchain();
    // カメラUBO用ディスクリプタ一式（パイプライン作成の前にレイアウトが必要）。
    void create_descriptor_set_layout();  // vkCreateDescriptorSetLayout（set=0, binding=0, UNIFORM_BUFFER, VERTEX）
    void create_uniform_buffers();        // camera_ubos_をkFramesInFlight個作成
    void create_descriptor_pool();        // vkCreateDescriptorPool（UNIFORM_BUFFER + COMBINED_IMAGE_SAMPLER）
    void create_descriptor_sets();        // vkAllocateDescriptorSets + vkUpdateDescriptorSetsで各UBO/テクスチャと結びつける
    void create_texture();                // textures/ の画像を読み込み Texture を生成（descriptor_sets の前に呼ぶ）
    void create_sampler();                // 全テクスチャで共有する VkSampler を生成

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
    // phase11 ①: 不透明/半透明で depthWrite・blend が異なるためパイプラインを 2 本持つ
    // （depthWriteEnable はコア Vulkan では動的化できないため）。レイアウトは共通。
    std::unique_ptr<GraphicsPipeline> pipeline_opaque_;       // depthWrite=TRUE,  blend=OFF
    std::unique_ptr<GraphicsPipeline> pipeline_transparent_;  // depthWrite=FALSE, blend=ON
    std::unique_ptr<DepthImage> depth_image_;
    VkFormat depth_format_;
    std::vector<VkFramebuffer> framebuffers_;
    std::unique_ptr<CommandBuffers> command_buffers_;
    std::unique_ptr<SyncObjects> sync_objects_;
    // phase12 手順2: 共有の単一メッシュをやめ、MeshId で引くレジストリに置き換えた。
    // 描画対象は scene::MeshHandle を持つエンティティのみ。
    std::unique_ptr<MeshRegistry> meshes_;

    // カメラUBO用ディスクリプタ（すべてkFramesInFlight個。スワップチェーン画像枚数には非依存）。
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<UniformBuffer>> camera_ubos_;
    std::vector<VkDescriptorSet> descriptor_sets_;  // プールから確保（個別破棄は不要、プール破棄でまとめて解放）

    // テクスチャ一式（スワップチェーン非依存。全エンティティ・全フレームで共有）。
    std::unique_ptr<Texture> texture_;
    std::unique_ptr<Sampler> sampler_;

    std::size_t current_frame_ = 0;
};

}  // namespace sq::graphics
