#pragma once

#include <array>
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
#include "sq/graphics/environment_map.hpp"
#include "sq/graphics/graphics_pipeline.hpp"
#include "sq/graphics/material_registry.hpp"
#include "sq/graphics/mesh_registry.hpp"
#include "sq/graphics/physical_device.hpp"
#include "sq/graphics/render_pass.hpp"
#include "sq/graphics/sampler.hpp"
#include "sq/graphics/shadow_map.hpp"
#include "sq/graphics/shadow_push_constants.hpp"
#include "sq/graphics/swapchain.hpp"
#include "sq/graphics/sync_objects.hpp"
#include "sq/graphics/texture_registry.hpp"
#include "sq/graphics/uniform_buffer.hpp"
#include "sq/graphics/vulkan_instance.hpp"
#include "sq/graphics/window.hpp"
#include "sq/scene/shadow_constants.hpp"

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

    // カスケードシャドウの段の色分け表示（phase17 ②-9）。
    //   有効にすると、本パスの frag が段ごとに色を掛ける（0 段目から 赤・緑・青・黄）。
    //   CameraUBO::light_count.y の scene::kDebugFlagCascadeColors ビットとして渡す。
    // ★ bias の調整（②-10）より**先に**使えるようにしておくこと。どの段で影を引いているかが
    //   見えないまま値をいじると、「どの段の問題か」が分からない。
    void set_shadow_debug(bool enabled);
    [[nodiscard]] bool shadow_debug() const;

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

    // ---- phase17 ⓪-4: 描画リストの一般化（D-1） ----
    //
    // instances_ は「区間の並び」になる。区間1本 = DrawList 1つ:
    //   [ opaque | transparent | cascade 0..3 | point 0 face 0 .. point 3 face 5 ]
    //   ↑ 本パス              ↑ ② CSM（4本）  ↑ ③ 点光源（4灯 × 6面 = 24本）
    //
    // ★ ⓪-4 の時点で使うのは opaque_ / transparent_ / cascade_lists_[0]（= phase16 の shadow_items_）だけ。
    //   残りは空のまま並びに入れておく（空の区間は instances_ に何も足さない）。

    // 影を落とせる点光源の最大数（phase17 D-9。③ で使う。⓪-4 では区間の本数を決めるためだけに要る）。
    static constexpr std::uint32_t kMaxPointShadows = 4;

    // 区間の総数 = 本パス2本 + カスケード + 点光源の面。
    static constexpr std::size_t kDrawListCount = 2 + scene::kCascadeCount + kMaxPointShadows * 6;

    // 区間1本（phase17 ⓪-4 / D-1）。
    struct DrawList {
        std::vector<DrawItem> items;
        // InstanceData 配列（instances_）における items[0] の位置。upload_instances が埋める。
        // ★ 収集フェーズでは触らないこと。並び順（draw_lists_in_order）からしか決まらない値。
        std::uint32_t first_instance = 0;
    };

    // instances_ に詰める順 = kMaxInstances 超過時に**残る**優先順位（先頭ほど残る）。
    //   opaque_ → transparent_ → cascade_lists_[0..3]（近い段から）→ point_shadow_lists_[0..23]
    // ★ 並びはこの関数以外に書かないこと。2か所に書くと first_instance がずれる。
    // ★ ポインタを返すのは、upload_instances が first_instance を書き込み、
    //   超過時に items を削るため（読むだけなら const 参照でよかった）。
    [[nodiscard]] std::array<DrawList*, kDrawListCount> draw_lists_in_order();

    // DrawList 版（phase17 ⓪-4）。list.first_instance を firstInstance の基準にする。
    //   instanced: false なら1件ずつ描く（半透明パス用。今までどおり）。
    void record_draw_items(VkCommandBuffer command_buffer,
                           const GraphicsPipeline& pipeline,
                           const DrawList& list,
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

    // 「影を落とすライトが無い」を表す値。FrameContext::shadow_light_indexとの比較で使用する。
    static constexpr std::uint32_t kNoShadowLight = ~0u;

    // 1フレームぶんの「CPU 側で決めた値」。collect_frame_data が作り、記録側が読む。
    // ★ メンバ変数にせず戻り値で渡す（フレームをまたいで残る状態にしないため）。
    struct FrameContext {
        glm::mat4 view_projection{ 1.0f };
        glm::vec3 camera_position{ 0.0f };  // 半透明ソートの距離基準 / カメラUBO の camera_position

        // ---- phase17 ②-1: カメラのパラメータ（resolve_camera が埋める） ----
        // カスケードを切るには「カメラの視錐台を距離で輪切りにする」必要があり、
        // 合成済みの view_projection だけでは足りない（②-3 で near/far を差し替えた projection を作り直す）。
        // ★ resolve_camera の3段フォールバックの**どの段でも**埋めること（既定ビューでも）。
        glm::mat4 view{ 1.0f };
        glm::vec3 camera_forward{ 0.0f, 0.0f, -1.0f };  // 正規化済み（normalize(target - position)）
        float fov_y = 0.0f;       // ラジアン
        float aspect = 1.0f;
        float near_plane = 0.1f;
        float far_plane = 500.0f;

        // phase16 ①-3: 影を落とすライトの情報（collect_lights が埋める）。
        //   shadow_light_index == kNoShadowLight なら、このフレームは影を落とすライトが無い。
        std::uint32_t shadow_light_index = kNoShadowLight;  // lights_ 内の添字（D-4）

        // ---- phase17 ②-3: カスケード（compute_cascades が埋める） ----
        //   shadow_light_index == kNoShadowLight のフレームでは意味を持たない（誰も読まない）。
        glm::vec3 shadow_light_direction{ 0.0f, -1.0f, 0.0f };  // 光の**進む**向き（collect_lights が埋める）
        std::array<glm::mat4, scene::kCascadeCount> cascade_view_projection{};
        glm::vec4 cascade_splits{ 0.0f };       // 各段の遠い側のビュー深度（CameraUBO と同じ形）
        glm::vec4 cascade_texel_sizes{ 0.0f };  // 各段の1テクセルのワールド幅（D-8）
    };

    // ---- phase17 ②: カスケードシャドウ（D-7） ----

    // 1段ぶんの計算結果。
    struct Cascade {
        glm::mat4 view_projection{ 1.0f };  // スナップ済みの光源 view-projection（CameraUBO::cascade_view_projection[c]）
        float texel_size = 0.0f;            // 1テクセルのワールド幅 = 2 * radius / kCascadeSize（D-8）
    };

    // 分割距離（practical split。②-2）。
    //   戻り値[i] = i 段目の**遠い側**のビュー深度。i 段目の近い側は [i - 1]（0 段目は near_plane）。
    //   far_plane は呼び出し側で min(カメラの far, kShadowMaxDistance) にしてから渡すこと。
    // ★ 純粋な計算なので static。式は plan ②-2（λ = kCascadeSplitLambda で対数分割と一様分割を混ぜる）。
    [[nodiscard]] static std::array<float, scene::kCascadeCount>
        compute_cascade_splits(float near_plane, float far_plane);

    // 1段ぶんの光源行列（②-3）。輪切り [split_near, split_far] を境界球で包み、テクセル単位でスナップする。
    //   light_direction: 光の**進む**向き（正規化済み）
    //   context:         カメラのパラメータ（view / fov_y / aspect）を読む
    // ★ 手順は plan ②-3 の 1〜5。特に 4（スナップ）は proj の平行移動成分だけを動かす。
    // ★ up の縮退（light_direction がほぼ ±Y）は compute_light_view_projection と同じ分岐が要る。
    [[nodiscard]] static Cascade compute_cascade(const glm::vec3& light_direction, const FrameContext& context,
                                                 float split_near, float split_far);

    // 全段を計算して context の cascade_* を埋める（②-2 + ②-3）。
    //   ★ 呼ぶのは collect_lights の**後**（shadow_light_direction が要る）、collect_draw_items の**前**
    //     （段ごとの視錐台でキャスタを振り分けるため）。collect_frame_data の中が素直。
    //   ★ 影を落とすライトが無いフレームでは呼ばなくてよい。
    static void compute_cascades(FrameContext& context);

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

    // カメラ視錐台でカリングして opaque_ / transparent_ / cascade_lists_ へ振り分け、ソートする。
    void collect_draw_items(const ecs::Registry& registry, const FrameContext& context);

    // draw_lists_in_order() の順に詰め、各 first_instance を埋め、超過したら並びの末尾から削る。
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

    // シャドウマップ一式（phase16 ①-2）。shadow_render_pass_ の後、パイプライン生成の後に呼ぶ。
    //   shadow_maps_ を kFramesInFlight 個作る（D-2）。
    // ★ recreate_swapchain からは**呼ばない**（解像度がウィンドウと無関係）。
    void create_shadow_maps();

    // set=2（environment_sets_）の中身を書く（phase16 ①-6）。
    //   binding=0 : shadow_maps_[i] + shadow_sampler_（DEPTH_STENCIL_READ_ONLY_OPTIMAL）
    //   binding=1..3 : ② までの仮置き。白テクスチャ + sampler_（SHADER_READ_ONLY_OPTIMAL）
    // ★ create_shadow_maps() と白テクスチャの登録（textures_->create_white_texture()）の
    //   **両方より後**に呼ぶこと。コンストラクタの最後が素直。
    void write_environment_sets();

    static constexpr std::size_t kFramesInFlight = 2;

    // 全グラフィックスパイプラインに共通のプッシュ定数レンジ（phase17 ⓪-3 / D-3）。
    //   中身は ShadowPushConstants（shadow_push_constants.hpp）。使うのはシャドウパスだけ。
    //
    // ★ set_layouts（コンストラクタの「14. パイプラインの作成」）と同じく、**全パイプラインにこの1つを渡す**。
    //   本パス・半透明・スカイボックスは push しないが、レンジが違うとレイアウト互換が崩れ、
    //   パイプラインを切り替えた瞬間にバインド済みのディスクリプタセットが外れる（phase16 D-3）。
    // ★ vkCmdPushConstants の stageFlags はこのレンジと**一致**させること
    //   （VERTEX だけで push すると「レンジと一致しない」でバリデーションエラー）。
    static constexpr VkPushConstantRange kPushConstantRange{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,  // stageFlags
        0,                                                           // offset
        sizeof(ShadowPushConstants),                                 // size
    };

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

    // ---- phase17 ②: カスケードシャドウの定数（D-7） ----
    //
    // 1段（1層）の一辺。4層 × kFramesInFlight で、D32 なら 16 MiB × 8 = 128 MiB。
    //   重いと感じたら 1024 に下げる（それでも 0 段目のテクセル密度は phase16 の数倍ある。②-11）。
    static constexpr std::uint32_t kCascadeSize = 2048;
    // 影を出す最大のビュー深度。カメラの far（500）まで4段で割ると近い段まで粗くなるので、ここで打ち切る。
    //   これより遠い場所には影が出ない（仕様。②-11）。
    static constexpr float kShadowMaxDistance = 100.0f;
    // practical split の混ぜ具合。1 = 純対数（近い段が狭すぎる）、0 = 一様（近い段が広すぎる）。
    static constexpr float kCascadeSplitLambda = 0.75f;
    // 正射影の near を光源側へ伸ばす量（②-3 の 3。D-7 で depth clamp の代わりに採った方法）。
    //   輪切りの外（光源側）にある物体も、輪切りの中へ影を落とす。小さすぎると
    //   「高い物体の影が、カメラを根元に近づけた途端に消える」。
    static constexpr float kCascadeCasterMargin = 50.0f;

    // depth bias（phase16 ①-8）。vkCmdSetDepthBias で毎フレーム渡す（動的ステート）。
    // phase17 ②-10 では法線オフセットだけで足りたので 0。仕組みは残してある
    static constexpr float kDepthBiasConstant = 0;
    static constexpr float kDepthBiasSlope    = 0;

    // 深度専用のレンダーパス（RenderPassConfig{ .has_color = false,
    //   .depth_final_layout = DEPTH_STENCIL_READ_ONLY_OPTIMAL, .depth_store_op = STORE }）。
    // ★ render_pass_ と違い、recreate_swapchain で作り直さない。
    std::unique_ptr<RenderPass> shadow_render_pass_;
    // 深度専用パイプライン（頂点シェーダのみ。D-6）。set_layouts は本パスと共通（D-3）。
    std::unique_ptr<GraphicsPipeline> pipeline_shadow_;

    // ---- phase16 ②: IBL ----
    //
    // 環境マップ（実行時CWD基準）。Poly Haven などの CC0 の .hdr を assets/textures/env/ に置く
    // （assets/textures は丸ごと実行ファイルの隣へコピーされるので CMake の変更は不要）。
    // ★ 見つからなければ灰色の一様キューブで続行する（HdrTexture の仕様）。
    static constexpr auto kEnvironmentMapPath = "textures/env/puresky_2k.hdr";

    // スカイボックス用パイプライン（②-9）。本パスの render_pass_ を使う。
    //   PipelineConfig{ .depth_write_enable = false, .blend_enable = false,
    //                   .cull_mode = VK_CULL_MODE_NONE,          ★ フルスクリーン三角形は巻き順を気にしない
    //                   .depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL,  ★ z = 1.0 で描くため
    //                   .has_vertex_input = false }
    //   set_layouts は本パスと共通（D-3）。
    // ★ render_pass_ と同様、スワップチェーンに依存しない（ビューポートは動的）ので作り直さない。
    std::unique_ptr<GraphicsPipeline> pipeline_skybox_;

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

    // シャドウマップ本体（phase16 ①-2）。kFramesInFlight 個（D-2）。
    // ★ フレーム N のシャドウパスが書く相手を、GPU 上でまだ実行中のフレーム N-1 の
    //   本パスが読んでいるかもしれない。カメラUBO・インスタンス SSBO と同じ理由で複製する。
    // ★ environment_sets_[i] の binding=0 が shadow_maps_[i] の view を指す。
    //   作り直すとその view が死ぬので、recreate_swapchain では触らないこと。
    std::vector<std::unique_ptr<ShadowMap>> shadow_maps_;

    // phase13 ②: per-instance データ（model / base_color / texture_index）の SSBO。
    // 「フレームごとに書き換わるもの」なのでカメラUBOと同じ set=0 に binding=1 として同居させる。
    // ★ kFramesInFlight 個持つ（GPU が読んでいる最中に上書きしないため。D-5）。
    // phase17 ⓪-4 / D-1: 4096 → 16384。
    //   カスケードの境界をまたぐ物体・床のような大きな物体は、複数の影の区間に**重複して**入る。
    //   1000 体の格子だけで影の区間が数千に達するため、本パスの体数より大きく取る。
    //   80 バイト × 16384 = 1.25 MiB × kFramesInFlight。
    static constexpr std::size_t kMaxInstances = 16384;  // 1フレームに描ける最大インスタンス数（全区間の合計）
    std::vector<std::unique_ptr<InstanceBuffer>> instance_buffers_;
    static constexpr std::size_t kMaxLights = 64;       // 1フレームに描ける最大光源数
    std::vector<std::unique_ptr<LightBuffer>> light_buffers_;

    // InstanceData の組み立て用バッファ（毎フレームのヒープ確保を避けるためメンバに持つ）。
    // 並び順は「draw_lists_in_order() の順」に全区間を連結し、DrawItem の並び順と1対1に対応させる。
    std::vector<InstanceData> instances_;

    // LightData の組み立て用バッファ（毎フレームのヒープ確保を避けるためメンバに持つ）。
    std::vector<LightData> lights_;

    // サンプラーは全テクスチャで共有する（スワップチェーン非依存）。
    std::unique_ptr<Sampler> sampler_;

    // シャドウマップ用の比較サンプラ（phase16 ①-2 / ⓪-3）。
    //   SamplerConfig{ .address_mode = CLAMP_TO_BORDER, .border_color = FLOAT_OPAQUE_WHITE,
    //                  .anisotropy = false, .compare_enable = true, .compare_op = LESS_OR_EQUAL }
    // ★ REPEAT のままだと影がシーン全体にタイル状に繰り返す。
    std::unique_ptr<Sampler> shadow_sampler_;

    // キューブマップ・BRDF LUT 用のサンプラ（phase16 ②-9 / ⓪-3）。
    //   SamplerConfig{ .address_mode = CLAMP_TO_EDGE, .anisotropy = false }
    //   （filter = LINEAR / compare なし / max_lod = VK_LOD_CLAMP_NONE は既定のまま）
    // ★ キューブは CLAMP_TO_EDGE にすること。REPEAT だと面の境界でバイリニアが
    //   反対側の端を拾い、スカイボックスに継ぎ目の線が出る。
    // ★ max_lod を 0 にしないこと。手順6の prefiltered は mip 0..4 を textureLod で読む。
    // ★ EnvironmentMap の前計算で equirect を読むのにもこれを使う。
    std::unique_ptr<Sampler> cube_sampler_;

    // IBL の前計算とその成果物（phase16 ②）。
    // ★ recreate_swapchain で作り直さないこと（set=2 に書いた view が死ぬ）。
    std::unique_ptr<EnvironmentMap> environment_;

    // phase12 手順4: 単一の共有テクスチャをやめ、TextureId で引くレジストリに置き換えた。
    // set=1 のディスクリプタセットはテクスチャ全体で1個
    std::unique_ptr<TextureRegistry> textures_;

    // phase14 ①: マテリアル本体（MaterialData の配列）を持つ SSBO のレジストリ。
    // set=1 の binding=1 に同居する（set=1 = 「起動後は不変なアセット」。D-2）。
    // ★ textures_ の後に生成し、textures_ より先に破棄すること
    //   （bindless_set() を借りており、フォールバック判定でも参照しているため）。
    std::unique_ptr<MaterialRegistry> materials_;

    // 既定アセットの永続参照（phase16 手順8 / ③-5）。
    //
    // 誰も参照しなくなっても解放してはいけないものを、Renderer が AssetRef で持ち続ける:
    //   default_texture_ref_     … textures/default.png（市松。読み込み失敗の可視化）
    //   white_texture_ref_       … 白 1×1（テクスチャ無しスロットの中立値）
    //   flat_normal_texture_ref_ … フラット法線 1×1（法線マップ無しの中立値）
    //   default_material_ref_    … 既定マテリアル（Material を持たない／無効IDのエンティティ用）
    // ★ 忘れると「最後のモデルを破棄した瞬間に既定テクスチャが消え、以後すべての
    //   フォールバックが死ぬ」。最初のモデルを持っている間は正常に見えるので、
    //   解放を実装するまで発覚しない。
    //
    // ★ plan ③-5 は「宣言順をレジストリより前にすれば AssetRef が先に死ぬ」としているが、
    //   **このクラスでは成り立たない**。
    //   - 宣言が前 = 破棄は**後**（破棄は宣言の逆順）なので、そもそも向きが逆
    //   - しかも ~Renderer() は本体の中で materials_.reset() / textures_.reset() を
    //     明示的に呼んでいる。メンバの自動破棄はその**後**に走るので、宣言位置をどこにしても
    //     「レジストリが先に死に、AssetRef が死んだレジストリへ release する」になる
    //   → ~Renderer() の本体で、レジストリの reset より前に明示的に reset() すること
    //     reset し忘れると、終了時に解放済みのレジストリへ release して落ちる。
    TextureRef default_texture_ref_;
    TextureRef white_texture_ref_;
    TextureRef flat_normal_texture_ref_;
    MaterialRef default_material_ref_;

    // ---- phase17 ⓪-4: 区間（D-1） ----
    // ★ 毎フレームの先頭（collect_draw_items）で**全区間の items を** clear() すること。
    //   ⓪-4 の時点で使わない区間も含める。② ③ で使い始めたときに clear し忘れると、
    //   前フレームのキャスタが残って影の残像になる。draw_lists_in_order() で回せば漏れない。
    DrawList opaque_;
    DrawList transparent_;
    // カスケードごとのシャドウキャスタ（②）。⓪-4 では [0] が phase16 の shadow_items_ の代わり。
    std::array<DrawList, scene::kCascadeCount> cascade_lists_;
    // 点光源の面ごとのシャドウキャスタ（③）。添字 = slot * 6 + face（キューブ配列の層番号と同じ）。
    std::array<DrawList, kMaxPointShadows * 6> point_shadow_lists_;

    std::size_t current_frame_ = 0;

    // カスケードの色分け表示（phase17 ②-9）。collect_frame_data が CameraUBO::light_count.y へ反映する。
    bool shadow_debug_ = false;
};

}  // namespace sq::graphics
