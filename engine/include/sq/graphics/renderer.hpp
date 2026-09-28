#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "depth_image.hpp"
#include "instance_buffer.hpp"
#include "light_buffer.hpp"
#include "sq/ecs/registry.hpp"
#include "sq/graphics/command_buffers.hpp"
#include "sq/graphics/debug_messenger.hpp"
#include "sq/graphics/deletion_queue.hpp"
#include "sq/graphics/device.hpp"
#include "sq/graphics/graphics_pipeline.hpp"
#include "sq/graphics/material_registry.hpp"
#include "sq/graphics/mesh_registry.hpp"
#include "sq/graphics/physical_device.hpp"
#include "sq/graphics/render_pass.hpp"
#include "sq/graphics/sampler.hpp"
#include "sq/graphics/swapchain.hpp"
#include "sq/graphics/sync_objects.hpp"
#include "sq/graphics/texture_registry.hpp"
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

    // テクスチャの登録・参照（phase12 手順4）。アプリ側が起動時に
    // renderer.textures().load("textures/foo.png") で登録し、得た TextureId を
    // MaterialData::albedo_index に入れる（phase14 ① 以降。以前は Material::albedo）。
    [[nodiscard]] TextureRegistry& textures();

    // マテリアルの登録・参照（phase14 ①）。アプリ側が起動時に
    // renderer.materials().add(MaterialData{ ... }) で登録し、得た MaterialId を
    // scene::Material::id に入れる。
    // ★ 同じマテリアルを共有する体が何百あっても、GPU 上の実体は1件で済む。
    [[nodiscard]] MaterialRegistry& materials();

private:
    // 描画1件の情報（phase12 手順6）。収集フェーズで作り、ソートしてから記録する。
    struct DrawItem {
        InstanceData instance_data;
        scene::MeshId mesh{};
        float distance_sq{};          // カメラからの2乗距離（半透明ソート用）
    };

    //   first_instance: InstanceData 配列における items[0] の位置。
    //   instanced: false なら範囲でまとめず1件ずつ描く（半透明パス用。順序が正しさそのもの）。
    void record_draw_items(VkCommandBuffer command_buffer,
                           const GraphicsPipeline& pipeline,
                           const std::vector<DrawItem>& items,
                           std::uint32_t first_instance,
                           bool instanced);

    // ---- phase16 ①-1: draw_frame の分解 ----
    //
    // draw_frame を「フレームのデータを作る」と「コマンドを記録する」の2段に分ける。
    // シャドウパスは本パスの**前**に記録する必要があるので、データ収集を
    // レンダーパスの外（= record ラムダより前）へ出しておく。
    //
    //   draw_frame
    //     ├─ フェンス待機 / acquire / vkResetFences         （今までどおり）
    //     ├─ collect_frame_data()                            ← レンダーパスの外
    //     │    ├─ resolve_camera()
    //     │    ├─ collect_lights()
    //     │    ├─ カメラUBO の更新
    //     │    ├─ collect_draw_items()
    //     │    └─ upload_instances()
    //     ├─ command_buffers_->record(...)
    //     │    ├─ record_shadow_pass()                       ← ①-1 では空
    //     │    └─ record_main_pass()
    //     └─ submit / present                                （今までどおり）
    //
    // ★ ①-1 は**絵が変わらないリファクタ**。①-2 以降（影の実装）とは別コミットにすること。

    // 1フレームぶんの「CPU 側で決めた値」。collect_frame_data が作り、記録側が読む。
    // ★ メンバ変数にせず戻り値で渡す（フレームをまたいで残る状態にしないため）。
    struct FrameContext {
        glm::mat4 view_projection{ 1.0f };
        glm::vec3 camera_position{ 0.0f };  // 半透明ソートの距離基準 / カメラUBO の camera_position

        // ①-3 で足すもの:
        //   glm::mat4     light_view_projection{ 1.0f };
        //   std::uint32_t shadow_light_index = ~0u;   // lights_ 内の添字。無ければ ~0u（D-4）
    };

    // フレームのデータを作ってGPUへ転送する（記録はしない）。
    //   ★ 呼ぶ位置は「vkAcquireNextImageKHR が成功した後」。
    //     aspect を swapchain_->extent() から取るので、recreate_swapchain より前に
    //     呼ぶと古い extent で計算してしまう。
    //   ★ current_frame_ のバッファ（camera_ubos_ / light_buffers_ / instance_buffers_）へ
    //     書き込むので、フェンス待機より後であること（今の record ラムダ内と同じ条件）。
    [[nodiscard]] FrameContext collect_frame_data(const ecs::Registry& registry);

    // アクティブカメラの3段フォールバック（phase10 D-2）で view_projection と位置を決める。
    [[nodiscard]] FrameContext resolve_camera(const ecs::Registry& registry, float aspect_ratio) const;

    // Light を lights_ に集め、上限でクランプして light_buffers_ へ転送する。
    //   ①-3 でシャドウキャスタライトの選択もここに入る（context.shadow_light_index を埋める）。
    void collect_lights(const ecs::Registry& registry, FrameContext& context);

    // カメラ視錐台でカリングして opaque_items_ / transparent_items_ へ振り分け、ソートする。
    //   ①-5 でライト視錐台による shadow_items_ の振り分けも**同じループ**に入る。
    void collect_draw_items(const ecs::Registry& registry, const FrameContext& context);

    // ソート済みの DrawItem から instances_ を組み立て、kMaxInstances でクランプして転送する。
    //   ①-5 で3区間（不透明 | 半透明 | シャドウキャスタ）になる。
    void upload_instances();

    // シャドウパスの記録（①-6）。
    //   ★ ①-1 の時点では**何も記録しない空の関数**のまま呼んでよい（絵は変わらない）。
    //     呼び出し位置だけ先に確定させておく。
    void record_shadow_pass(VkCommandBuffer command_buffer, const FrameContext& context);

    // 本パスの記録: BeginRenderPass → viewport/scissor → set=0/1 のバインド
    //   → 不透明 → 半透明 → EndRenderPass。
    //   ②-9 でスカイボックスが「不透明の後・半透明の前」に入る。
    void record_main_pass(VkCommandBuffer command_buffer, std::uint32_t image_index);

    void create_surface();
    void create_framebuffers();
    void destroy_framebuffers();
    void recreate_swapchain();
    // ディスクリプタ一式（パイプライン作成の前にレイアウトが必要）。
    // phase12 手順3: set=0（カメラUBO）と set=1（マテリアル）の2つのレイアウトを作る。
    // phase16 ⓪-4: set=2（ライティング環境）を足して3つになった。
    void create_descriptor_set_layout();
    void create_uniform_buffers();        // camera_ubos_をkFramesInFlight個作成
    void create_instance_buffers();
    void create_light_buffers();
    void create_descriptor_pool();        // vkCreateDescriptorPool（UNIFORM_BUFFER + COMBINED_IMAGE_SAMPLER）
    void create_descriptor_sets();        // vkAllocateDescriptorSets + vkUpdateDescriptorSetsで各UBO/テクスチャと結びつける
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

    // GPUリソースの遅延解放キュー（phase14 ②-4）。
    // ★ 各レジストリより**先に**生成し、**後に**破棄すること
    //   （レジストリが参照を持つため）。宣言順 = 構築順、破棄は逆順。
    std::unique_ptr<DeletionQueue> deletions_;
    // phase12 手順2: 共有の単一メッシュをやめ、MeshId で引くレジストリに置き換えた。
    // 描画対象は scene::MeshHandle を持つエンティティのみ。
    std::unique_ptr<MeshRegistry> meshes_;

    static constexpr std::uint32_t kMaxTextures = 64;  // 登録できるテクスチャ数の上限（プールの容量）

    // 登録できるマテリアル数の上限（phase14 ①。MaterialBuffer の容量）。
    // ★ テクスチャと違いディスクリプタ枠を消費しないので、多めに取ってもコストは
    //   sizeof(MaterialData) * kMaxMaterials = 48 * 256 = 12KiB だけ。
    //   glTF（③）が1ファイルで数十件を登録し得るので、テクスチャより余裕を持たせる。
    static constexpr std::uint32_t kMaxMaterials = 256;

    VkDescriptorSetLayout camera_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;

    // set=2「ライティング環境」のレイアウト（phase16 ⓪-4 / D-1）。
    //   binding=0  シャドウマップ           COMBINED_IMAGE_SAMPLER  FRAGMENT  （①）
    //   binding=1  irradiance キューブ      COMBINED_IMAGE_SAMPLER  FRAGMENT  （②）
    //   binding=2  prefiltered キューブ     COMBINED_IMAGE_SAMPLER  FRAGMENT  （②）
    //   binding=3  BRDF LUT                 COMBINED_IMAGE_SAMPLER  FRAGMENT  （②）
    //
    // なぜ set=1（bindless テクスチャ配列）に入れないか。理由は3つあり、どれも単独で決定的:
    //   1. キューブマップは sampler2D[] に混ぜられない（samplerCube は別の型）
    //   2. シャドウマップは比較サンプラ（compareEnable = VK_TRUE）が要る。
    //      set=1 の配列は全要素が sampler_（compareEnable = VK_FALSE）を共有している
    //   3. シャドウマップはフレームごとに実体が変わる（D-2）。set=1 は
    //      「起動後は不変」という前提で全体に1個しか作っていない
    //
    // ★ UPDATE_AFTER_BIND も PARTIALLY_BOUND も**付けない**。
    //   4 binding すべてを、バインドするより前に書き終える約束にする。
    VkDescriptorSetLayout environment_set_layout_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<UniformBuffer>> camera_ubos_;
    std::vector<VkDescriptorSet> descriptor_sets_;  // set=0。プールから確保（個別破棄は不要）

    // set=2（phase16 ⓪-4）。シャドウマップだけがフレームごとに変わるので、
    // set=0 と同じく kFramesInFlight 個複製する（D-1 / D-2）。
    // IBL の3枚はどのフレームのセットからも同じ実体を指す。
    // ★ プールから確保するので個別破棄は不要。
    std::vector<VkDescriptorSet> environment_sets_;

    // phase13 ②: per-instance データ（model / base_color / texture_index）の SSBO。
    // 「フレームごとに書き換わるもの」なのでカメラUBOと同じ set=0 に binding=1 として同居させる。
    // ★ kFramesInFlight 個持つ（GPU が読んでいる最中に上書きしないため。D-5）。
    static constexpr std::size_t kMaxInstances = 4096;  // 1フレームに描ける最大体数
    std::vector<std::unique_ptr<InstanceBuffer>> instance_buffers_;
    static constexpr std::size_t kMaxLights = 64;       // 1フレームに描ける最大光源数
    std::vector<std::unique_ptr<LightBuffer>> light_buffers_;

    // InstanceData の組み立て用バッファ（毎フレームのヒープ確保を避けるためメンバに持つ）。
    // 並び順は「不透明→半透明」で連結し、DrawItem の並び順と1対1に対応させる。
    std::vector<InstanceData> instances_;

    // LightData の組み立て用バッファ（毎フレームのヒープ確保を避けるためメンバに持つ）。
    std::vector<LightData> lights_;

    // サンプラーは全テクスチャで共有する（スワップチェーン非依存）。
    std::unique_ptr<Sampler> sampler_;

    // phase12 手順4: 単一の共有テクスチャをやめ、TextureId で引くレジストリに置き換えた。
    // set=1 のディスクリプタセットはテクスチャ全体で1個
    std::unique_ptr<TextureRegistry> textures_;

    // phase14 ①: マテリアル本体（MaterialData の配列）を持つ SSBO のレジストリ。
    // set=1 の binding=1 に同居する（set=1 = 「起動後は不変なアセット」。D-2）。
    // ★ textures_ の後に生成し、textures_ より先に破棄すること
    //   （bindless_set() を借りており、フォールバック判定でも参照しているため）。
    std::unique_ptr<MaterialRegistry> materials_;

    // 描画アイテムの収集バッファ（phase12 手順6）。
    // 毎フレームのヒープ確保を避けるためメンバに持ち、draw_frame の先頭で clear() して再利用する
    // （clear() は capacity を保つ）。
    std::vector<DrawItem> opaque_items_;
    std::vector<DrawItem> transparent_items_;

    std::size_t current_frame_ = 0;
};

}  // namespace sq::graphics
