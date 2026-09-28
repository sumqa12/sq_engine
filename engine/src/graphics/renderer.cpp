#include "sq/graphics/renderer.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <GLFW/glfw3.h>
#include <glm/ext/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "sq/scene/camera.hpp"
#include "sq/scene/frustum.hpp"
#include "sq/scene/hierarchy.hpp"  // WorldTransform（phase14 ④）
#include "sq/scene/light.hpp"
#include "sq/scene/material.hpp"
#include "sq/scene/mesh_handle.hpp"
#include "sq/scene/transform.hpp"

namespace sq::graphics {
    using namespace std::chrono;
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

        depth_format_ = PhysicalDeviceSelector::find_depth_format(physical_device_);

        depth_image_ = std::make_unique<DepthImage>(physical_device_, device_->handle(), device_->allocator(), swapchain_->extent(), depth_format_);

        // 7. レンダーパスの作成
        render_pass_ = std::make_unique<RenderPass>(device_->handle(), swapchain_->image_format(), depth_format_);

        // 深度専用のシャドウレンダーパスを作る。
        shadow_render_pass_ = std::make_unique<RenderPass>(device_->handle(), swapchain_->image_format(), depth_format_,
            RenderPassConfig{
                .has_color = false,
                .depth_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                .depth_store_op = VK_ATTACHMENT_STORE_OP_STORE
            }
        );
        //   color_format は使われないので VK_FORMAT_UNDEFINED でよい。
        //   ★ depth_store_op を STORE にし忘れると「影が出ない／ノイズになる」（①-11）。

        // 8. ディスクリプタセットレイアウトの作成（カメラUBO用、set=0）
        create_descriptor_set_layout();

        // 9. UBOの作成
        create_uniform_buffers();

        // 10. サンプラーを生成する（TextureRegistry へ渡すのでプールより前に必要）
        create_sampler();

        // 11. インスタンスバッファの作成
        create_instance_buffers();

        // 12. 光源バッファの作成
        create_light_buffers();
        lights_.reserve(kMaxLights); // サイズが小さいので、事前に確保しておく（push_back の再確保を避けるため）。

        // 13. ディスクリプタプールの作成
        create_descriptor_pool();
        create_descriptor_sets();

        // 14. パイプラインの作成（phase11 ①: 不透明用・半透明用の 2 本。SPV とレイアウトは共通）
        // phase12 手順3: index が set 番号に対応する（[0]=カメラ, [1]=マテリアル）。
        //
        // ★ phase16 D-3: この配列を**すべてのパイプラインで共通に使うこと**
        //   （① で足すシャドウ用、② で足すスカイボックス用も含む）。
        //   set_layouts かプッシュ定数レンジが違うパイプライン同士は「レイアウト互換」で
        //   なくなり、vkCmdBindPipeline した瞬間に**バインド済みのディスクリプタセットが
        //   黙って外れる**。シャドウパスが set=2 を参照しなくても、レイアウトには含めておく。
        // ★ レイアウトに含めるだけなら、そのセットをバインドしない限り何も起きない。
        //   set=2 を実際にバインドし始めるのは ①-6 から。
        const std::vector set_layouts = { camera_set_layout_, material_set_layout_, environment_set_layout_ };

        pipeline_opaque_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
            "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
            set_layouts,
            PipelineConfig{ .depth_write_enable = true,  .blend_enable = false });

        pipeline_transparent_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
            "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
            set_layouts,
            PipelineConfig{ .depth_write_enable = false, .blend_enable = true });

        pipeline_shadow_ = std::make_unique<GraphicsPipeline>(device_->handle(), shadow_render_pass_->handle(), swapchain_->extent(),
            "shaders/shadow.vert.spv", "",
            set_layouts,
            PipelineConfig{
                .depth_write_enable = true, .blend_enable = false,
                .has_color_attachment = false, .depth_bias_enable = true,
                .cull_mode = VK_CULL_MODE_FRONT_BIT
            }
        );

        // 15. シャドウマップの作成
        create_shadow_maps();

        // 16. フレームバッファの作成
        create_framebuffers();

        // 17. コマンドバッファの作成
        command_buffers_ = std::make_unique<CommandBuffers>(device_->handle(), *queue_family_indices_.graphics_family, kFramesInFlight);

        // 18. 同期オブジェクトの作成
        sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight, framebuffers_.size());

        // 19. レジストリより 先に 遅延解放キューを作る。
        deletions_ = std::make_unique<DeletionQueue>(static_cast<std::uint32_t>(kFramesInFlight));

        // 20. アセットレジストリの生成（phase12 手順2・4）
        // 各レジストリのコンストラクタに *deletions_ を渡す。
        meshes_ = std::make_unique<MeshRegistry>(
            device_->allocator(), device_->handle(),
            *queue_family_indices_.graphics_family, device_->graphics_queue(), *deletions_);

        textures_ = std::make_unique<TextureRegistry>(
            physical_device_, device_->handle(), device_->allocator(),
            *queue_family_indices_.graphics_family, device_->graphics_queue(),
            descriptor_pool_, material_set_layout_, sampler_->handle(), kMaxTextures, *deletions_);

        // 既定テクスチャを最初に登録する（読み込み・変換に失敗したときに使う市松模様）。
        // 最初に load したものが TextureRegistry::default_texture() になる。
        // 「色」として使うので VK_FORMAT_R8G8B8A8_SRGB を渡す。
        textures_->load("textures/default.png", VK_FORMAT_R8G8B8A8_SRGB);

        // 続けて中立な 1×1 白テクスチャを登録する。
        textures_->create_white_texture();
        //   ★ 必ず default.png の**後**に呼ぶこと。default_texture() は
        //     「最初に登録されたもの」で決まるので、順序を逆にすると白が既定になり、
        //     読み込み失敗が市松模様で可視化されなくなる。
        //   ★ 用途の違いは texture_registry.hpp の create_white_texture() のコメントを参照。

        // 続けて中立な 1×1 フラット法線テクスチャも登録する
        textures_->create_flat_normal_texture();

        // マテリアルレジストリを生成する。
        materials_ = std::make_unique<MaterialRegistry>(
            device_->allocator(), device_->handle(), *textures_,
            textures_->bindless_set(), kMaxMaterials);

        // 続けて既定マテリアルを1件登録する（Material を持たない／無効IDのエンティティ用）。
        materials_->add(MaterialData{}, { .albedo = textures_->default_texture() });

        write_environment_sets();
        //   ★ ここ（コンストラクタの最後）に置く理由: shadow_maps_ と白テクスチャの
        //     **両方**が出揃っているのはこの時点だけ。
    }

    Renderer::~Renderer() {
        vkDeviceWaitIdle(device_->handle());

        // ★ vkDeviceWaitIdle の後、レジストリ破棄の前に呼ぶ。
        if (deletions_) { deletions_->flush_all(); }

        destroy_framebuffers();
        // メッシュ・テクスチャ・マテリアル（GPUリソース）は GpuAllocator（= Device）より前に破棄する
        // （phase11 ③ の不変条件）。ディスクリプタセットはこの後のプール破棄でまとめて解放される。
        meshes_.reset();
        // ★ materials_ は textures_ への参照を持つので、textures_ より**先に**破棄する（phase14 ①）。
        materials_.reset();
        textures_.reset();
        deletions_.reset();  // ★ レジストリより後（レジストリが参照を持つため。phase14 ②）
        sync_objects_.reset();
        command_buffers_.reset();
        pipeline_transparent_.reset();
        pipeline_opaque_.reset();
        pipeline_shadow_.reset();
        shadow_maps_.clear();

        vkDestroyDescriptorPool(device_->handle(), descriptor_pool_, nullptr);
        // phase12 手順3: レイアウトは2つになった（セットはプール破棄でまとめて解放される）。
        vkDestroyDescriptorSetLayout(device_->handle(), camera_set_layout_, nullptr);
        vkDestroyDescriptorSetLayout(device_->handle(), material_set_layout_, nullptr);
        vkDestroyDescriptorSetLayout(device_->handle(), environment_set_layout_, nullptr);

        sampler_.reset();
        shadow_sampler_.reset();
        camera_ubos_.clear();
        instance_buffers_.clear();
        light_buffers_.clear();
        descriptor_sets_.clear();
        environment_sets_.clear();
        descriptor_pool_ = VK_NULL_HANDLE;
        camera_set_layout_ = VK_NULL_HANDLE;
        material_set_layout_ = VK_NULL_HANDLE;
        environment_set_layout_ = VK_NULL_HANDLE;

        render_pass_.reset();
        shadow_render_pass_.reset();
        swapchain_.reset();
        vkDestroySurfaceKHR(instance_->handle(), surface_, nullptr);
        depth_image_.reset();
        device_.reset();
        queue_family_indices_ = {};
        physical_device_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        debug_messenger_.reset();
        instance_.reset();
        window_.reset();
    }

    bool Renderer::should_close() const {
        return window_ == nullptr || window_->should_close();
    }

    void Renderer::draw_frame(const ecs::Registry& registry) {
        if (window_->is_fullscreen() &&
            !swapchain_->exclusive_acquired() && swapchain_->created_with_fse() &&
            window_->is_focused()) {
            swapchain_->acquire_full_screen_exclusive();

        }

        // 1. フェンスの待機
        VkFence in_flight_fence = sync_objects_->in_flight_fence(current_frame_);
        vkWaitForFences(device_->handle(), 1, &in_flight_fence, VK_TRUE, UINT64_MAX);

        // 遅延解放キューを1フレーム進める。
        deletions_->flush_expired();
        //   ★ 位置が重要。フェンス待機の 直後 に置くこと。
        //     待機より前に呼ぶと、まだ GPU が実行中のフレームを「終わった」と数えてしまい、
        //     kFramesInFlight 待っているつもりで実際には足りなくなる。
        //     acquire より後でもよいが、early return（OUT_OF_DATE 等）を跨ぐと
        //     フレームによって呼ばれたり呼ばれなかったりするので、ここが一番素直。

        // 2. 画像の取得
        uint32_t image_index = 0;
        VkSemaphore image_available = sync_objects_->image_available(current_frame_);
        if (VkResult result = vkAcquireNextImageKHR(device_->handle(), swapchain_->handle(), UINT64_MAX, image_available, VK_NULL_HANDLE, &image_index);
            result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            return;
        } else if (result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
            swapchain_->release_full_screen_exclusive();    // 排他なしに降格（スワップチェーンは維持）
            recreate_swapchain();
            return;
        }

        vkResetFences(device_->handle(), 1, &in_flight_fence);

        const FrameContext frame = collect_frame_data(registry);   // ← ラムダの外

        command_buffers_->record(current_frame_,
            [this, image_index, &frame](VkCommandBuffer command_buffer, std::size_t) {
                record_shadow_pass(command_buffer, frame);   // ①-1 では空
                record_main_pass(command_buffer, image_index);
            });

        // コマンドバッファの送信
        VkSemaphore render_finished = sync_objects_->render_finished(image_index);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_available;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = command_buffers_->at(current_frame_);
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished;
        submit_info.pWaitDstStageMask = &wait_stage;

        if (VkResult result =
            vkQueueSubmit(device_->graphics_queue(), 1, &submit_info, in_flight_fence);
            result != VK_SUCCESS) {
            printf("Failed to submit draw command buffer!\n");
        }

        // 4. 画像の提示
        VkSwapchainKHR swapchain_handle = swapchain_->handle();
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished;
        present_info.pImageIndices = &image_index;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_handle;

        const bool resized = window_->consume_resized_flag();  // 短絡評価に関係なく毎フレーム消費
        if (VkResult result = vkQueuePresentKHR(device_->graphics_queue(), &present_info);
            result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || resized) {
            recreate_swapchain();
        } else if (result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
            swapchain_->release_full_screen_exclusive();
        }

        current_frame_ = (current_frame_ + 1) % kFramesInFlight;
    }

    // 収集・ソート済みの描画アイテムを順に記録する（phase12 手順6）。
    void Renderer::record_draw_items(VkCommandBuffer command_buffer,
                                     const GraphicsPipeline& pipeline,
                                     const std::vector<DrawItem>& items,
                                     std::uint32_t first_instance,
                                     bool instanced) {
        std::size_t i = 0;
        while (i < items.size()) {
            // 1. 同じメッシュが続く範囲 [i, j) を数える
            std::size_t j = i;
            if (!instanced) {
                j = i + 1;
            } else {
                while (j < items.size() && items[j].mesh == items[i].mesh) { ++j; }
            }
            // ★ instanced == false（半透明）なら j = i + 1 に固定する（まとめない）

            // 2. 範囲の先頭で1回だけメッシュをバインドする
            const auto& [vertices, indices, bounds] = meshes_->get(items[i].mesh);
            vertices->bind(command_buffer);
            indices->bind(command_buffer);

            // 3. 範囲まるごとを1回のドローで描く
            vkCmdDrawIndexed(command_buffer, indices->index_count(),
                 static_cast<std::uint32_t>(j - i),               // instanceCount
                 0, 0,
                 first_instance + static_cast<std::uint32_t>(i)   // firstInstance
            );
            i = j;
        }
    }

    // ---- phase16 ①-1: draw_frame の分解 ----

    // フレームのデータを作ってGPUへ転送する（コマンドは記録しない）。
    Renderer::FrameContext Renderer::collect_frame_data(const ecs::Registry& registry) {
        // アスペクト比の計算
        float aspect_ratio = static_cast<float>(swapchain_->extent().width) / static_cast<float>(swapchain_->extent().height);

        // コンテキストの取得
        FrameContext context = resolve_camera(registry, aspect_ratio);

        // 光源の収集
        collect_lights(registry, context);

        // カメラUBOの更新
        scene::CameraUBO camera_ubo{};
        camera_ubo.view_projection = context.view_projection;
        camera_ubo.camera_position = glm::vec4(context.camera_position, 1.0f);
        camera_ubo.light_count = glm::uvec4(lights_.size(), context.shadow_light_index, 0, 0);
        camera_ubo.light_view_projection = context.light_view_projection;
        camera_ubos_[current_frame_]->update(&camera_ubo, sizeof(camera_ubo));

        // 描画アイテムの収集
        collect_draw_items(registry, context);

        // バッファの転送
        upload_instances();

        return context;
    }

    // アクティブカメラを解決する（phase10 D-2 の3段フォールバック）。
    Renderer::FrameContext Renderer::resolve_camera(const ecs::Registry& registry, float aspect_ratio) const {
        // カメラの取得
        // アクティブカメラの選択を3段フォールバックにする（phase10プラン D-2）
        glm::mat4 view_projection = scene::Camera::default_view_projection(aspect_ratio);
        glm::vec3 cam_pos{0.0f, 1.5f, 3.0f};  // 既定ビューの位置（half-transparent ソートの距離基準。phase11 ①）
        ecs::Entity camera_entity = registry.view<scene::Camera, scene::ActiveCamera>().front();

        if (camera_entity.is_null()) {
            camera_entity = registry.view<scene::Camera>().front();
        }

        if (!camera_entity.is_null()) {
            const scene::Camera& camera = registry.get<scene::Camera>(camera_entity);
            view_projection = camera.view_projection(aspect_ratio);
            cam_pos = camera.position;  // ソートの距離基準（phase10 で確定した ActiveCamera の位置）
        }

        return { .view_projection = view_projection, .camera_position = cam_pos };
    }

    // ライトを収集して light_buffers_[current_frame_] へ転送する。
    void Renderer::collect_lights(const ecs::Registry& registry, FrameContext& context) {
        // シャドウキャスタライトが選択済みかどうか
        bool is_selected = false;

        // 光源の収集
        lights_.clear();
        registry.view<scene::Light, scene::WorldTransform>().each(
        [&](ecs::Entity, scene::Light& light, scene::WorldTransform& wt) {
            // 強度 0 はスキップ
            if (light.intensity <= 0.0f) { return; }

            auto position = glm::vec3(wt.matrix[3]);
            auto direction = glm::normalize(-glm::vec3(wt.matrix[2]));

            LightData light_data = {
                .position_type = glm::vec4(position, static_cast<float>(static_cast<int>(light.type))),
                .direction_range = glm::vec4(direction, light.range),
                .color_intensity = glm::vec4(light.color, light.intensity)
            };

            if (light.type == scene::LightType::Directional && light.cast_shadows) {
                if (is_selected) {
                    spdlog::warn("Renderer::collect_lights : 2つ目以降のシャドウキャスタライトがあります。無視します。");
                } else {
                    context.shadow_light_index = lights_.size();
                    context.light_view_projection = compute_light_view_projection(direction);
                }
            }

            lights_.emplace_back(light_data);
        });

        if (lights_.size() > kMaxLights) {
            spdlog::warn("警告: 光源の個数 {} は上限 {} を超過しています。切り詰めます。\n",
                   lights_.size(), kMaxLights);
            lights_.resize(kMaxLights);
        }

        if (context.shadow_light_index >= lights_.size()) {
            context.shadow_light_index = kNoShadowLight;
        }

        // バッファの更新
        light_buffers_[current_frame_]->update(lights_.data(), lights_.size());
    }

    // 描画アイテムを収集・ソートする。
    void Renderer::collect_draw_items(const ecs::Registry& registry, const FrameContext& context) {
        // 1. 収集（不透明・半透明に振り分ける）
        // MeshHandle を持つエンティティのみが描画対象（カメラ等の非描画エンティティは除外される）。
        //
        // phase13 ⑤: この view-projection から視錐台を作り、収集の時点で画面外を落とす。
        // カメラ解決後・収集前に1回だけ作ればよい（フレーム中は不変）。
        const bool has_shadow = context.shadow_light_index != kNoShadowLight;
        const scene::Frustum light_frustum = scene::Frustum::from_view_projection(context.light_view_projection);
        const scene::Frustum frustum = scene::Frustum::from_view_projection(context.view_projection);

        opaque_items_.clear();
        transparent_items_.clear();
        shadow_items_.clear();

        // view を <scene::Transform, scene::MeshHandle> から
        //   <scene::WorldTransform, scene::MeshHandle> へ変える。
        //   ワールド行列の合成は TransformSystem::update が draw_frame より前に済ませている。
        registry.view<scene::WorldTransform, scene::MeshHandle>().each(
            [&](ecs::Entity e, scene::WorldTransform& wt, scene::MeshHandle& mh) {
                // 未登録のメッシュを指すハンドルはスキップする
                if (!meshes_->contains(mh.id)) { return; }

                // (phase13 ⑤-3): フラスタムカリング。ローカルの境界球をワールドへ移して判定する。
                const MeshRegistry::Entry& entry = meshes_->get(mh.id);
                const glm::mat4& model = wt.matrix;

                //   中心は model で変換する（平行移動を含めるため vec4 の w は 1）
                const auto world_center = glm::vec3(model * glm::vec4(entry.bounds.center, 1.0f));

                //   回転は球を変えない。非等方スケールは最大成分で保守的に見積もる
                // ★ t.scale の直参照をやめる。
                //   ワールド行列には**親のスケールも掛かっている**ので、自分のローカル
                //   スケールだけを見ると半径を過小評価する。ワールド行列の3本の基底
                //   ベクトルの長さから求め直すこと:
                const float sx = glm::length(glm::vec3(model[0]));
                const float sy = glm::length(glm::vec3(model[1]));
                const float sz = glm::length(glm::vec3(model[2]));
                //   ★ 直し忘れると、親でスケールした子が画面端で消える
                //     （phase13 ⑤ で踏んだのと同じ症状が、原因だけ変わって再発する）。

                // Material を1回だけ引く。持たないエンティティは既定マテリアル
                // （既定テクスチャ・白・不透明）として扱う（後方互換）。
                scene::Material material{};
                if (registry.has<scene::Material>(e)) {
                    material = registry.get<scene::Material>(e);
                }

                const float world_radius = entry.bounds.radius * std::max({ sx, sy, sz });
                const bool in_camera = frustum.intersects(world_center, world_radius);
                const bool in_light  = has_shadow && !material.transparent && light_frustum.intersects(world_center, world_radius);
                if (!in_camera && !in_light) {
                    return;
                }

                // マテリアルのフォールバック
                const scene::MaterialId material_id = materials_->contains(material.id)
                    ? material.id
                    : materials_->default_material();

                // 描画用データを作る（phase14 ①: model と material_index だけになった）
                //
                // ★ 半透明ソートの距離基準もワールド位置にする。
                //   t.position はローカル座標なので、親が動くと距離が嘘になる
                const glm::vec3 d = glm::vec3(model[3]) - context.camera_position;
                const DrawItem item{
                    .instance_data = InstanceData{ .model = model, .material_index = material_id.index },
                    .mesh = mh.id,
                    .distance_sq = glm::dot(d, d),  // 2乗距離（順序比較にしか使わないので sqrt 不要）
                };

                if (in_camera) {
                    (material.transparent ? transparent_items_ : opaque_items_).push_back(item);
                } else {
                    shadow_items_.push_back(item);
                }
            }
        );

        // 2. meshでソート
        // phase14 ②: MeshId が構造体になったので、比較は AssetHandle::operator<
        //   （index 比較）が担う。ここの式自体は変えなくてよいが、
        //   「何を比べているのか」が asset_handle.hpp 側に移ったことは意識しておくこと。
        std::ranges::sort(opaque_items_,
            [](const DrawItem& a, const DrawItem& b) {
                return a.mesh < b.mesh;
            }
        );

        // 半透明: 遠い順（back-to-front）。
        // ★ 正しさが順序に依存するため、テクスチャ・メッシュでまとめてはいけない（ソート順が絶対）。
        std::ranges::sort(transparent_items_,
            [](const DrawItem& a, const DrawItem& b) {
                return a.distance_sq > b.distance_sq;
            }
        );
    }

    // instances_ を組み立てて instance_buffers_[current_frame_] へ転送する。
    void Renderer::upload_instances() {
        instances_.clear();
        instances_.reserve(opaque_items_.size() + transparent_items_.size() + shadow_items_.size());
        // 不透明 → 半透明 → シャドウアイテムの順に詰める（★この順序が first_instance の基準になる）:
        for (const DrawItem& item : opaque_items_) {
            instances_.push_back(item.instance_data);
        }
        for (const DrawItem& item : transparent_items_) {
            instances_.push_back(item.instance_data);
        }
        for (const DrawItem& item : shadow_items_) {
            instances_.push_back(item.instance_data);
        }

        // 上限超えたら警告を出し、リサイズ
        if (instances_.size() > kMaxInstances) {
            std::size_t exceeds = instances_.size() - kMaxInstances;
            spdlog::warn("警告: インスタンスの個数 {} は上限 {} を超過しています。切り詰めます。\n",
                   instances_.size(), kMaxInstances);
            instances_.resize(kMaxInstances);

            // 描画側も同じ位置で止める（転送されていない範囲を描かないため）
            std::size_t size = shadow_items_.size();
            shadow_items_.resize(glm::min(exceeds, std::size_t{0}));
            exceeds -= size;
            if (exceeds > 0) {
                size = transparent_items_.size();
                transparent_items_.resize(glm::min(exceeds, std::size_t{0}));
            }
            if (exceeds > 0) {
                opaque_items_.resize(exceeds);
            }
        }

        instance_buffers_[current_frame_]->update(instances_.data(), instances_.size());
    }

    // シャドウパスを記録する。
    void Renderer::record_shadow_pass(VkCommandBuffer command_buffer, const FrameContext& context) {
        //   ★ 影を落とすライトが無いフレームでも、**レンダーパスの Begin / End だけは通す**こと。
        //     シャドウマップのレイアウトを DEPTH_STENCIL_READ_ONLY_OPTIMAL にするのは
        //     このパスの finalLayout だけなので、スキップすると初回フレームのレイアウトが
        //     UNDEFINED のまま本パスが set=2 を読み、バリデーションが出る。
        //     描画（手順 4〜7）だけを context.shadow_light_index で飛ばす。
        VkClearValue clear{};
        clear.depthStencil = {
            .depth = 1.0f,
            .stencil = 0
        };
        //      clearValueCount = 1   ★ カラーが無いので1つだけ
        VkRenderPassBeginInfo render_pass_begin_info{};
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.renderPass = shadow_render_pass_->handle();
        render_pass_begin_info.framebuffer = shadow_maps_[current_frame_]->framebuffer();
        render_pass_begin_info.renderArea =  {{0, 0}, shadow_maps_[current_frame_]->extent()};
        render_pass_begin_info.clearValueCount = 1;
        render_pass_begin_info.pClearValues = &clear;

        // レンダーパスの開始
        vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width  = static_cast<float>(shadow_maps_[current_frame_]->extent().width);
        viewport.height = static_cast<float>(shadow_maps_[current_frame_]->extent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);

        VkRect2D scissor{{0, 0}, shadow_maps_[current_frame_]->extent()};
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);

        if (context.shadow_light_index > 0) {
            vkCmdSetDepthBias(command_buffer, kDepthBiasConstant, 0.0f, kDepthBiasSlope);

            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_shadow_->handle());

            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline_shadow_->layout(), 0, 1, &descriptor_sets_[current_frame_], 0, nullptr);
            //  ★ set=1 / set=2 はバインドしない。set=2 binding=0 はいま書き込んでいる当のイメージ。

            record_draw_items(command_buffer, *pipeline_shadow_, shadow_items_,
                opaque_items_.size() + transparent_items_.size(), true);
        }

        vkCmdEndRenderPass(command_buffer);
    }

    // 本パスを記録する。
    void Renderer::record_main_pass(VkCommandBuffer command_buffer, std::uint32_t image_index) {
        // ★ ①-1 の時点ではシャドウパスが空なので、set=0 のバインドはここだけでよい。
        //   ①-6 でシャドウパスも set=0 をバインドするようになるが、レンダーパスを跨いでも
        //   バインドは残る（レイアウト互換なので）。それでも**ここで改めてバインドしておく**方が、
        //   パスの記録が互いに独立して読める。

        std::array<VkClearValue, 2> clear_values{};
        clear_values[0].color = { {0.6f, 0.6f, 0.6f, 1.0f} };
        clear_values[1].depthStencil = { 1.0f, 0 };  // far=1.0でクリア（GLM_FORCE_DEPTH_ZERO_TO_ONE前提）

        VkRenderPassBeginInfo render_pass_begin_info{};
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.renderPass = render_pass_->handle();
        render_pass_begin_info.framebuffer = framebuffers_[image_index];
        render_pass_begin_info.renderArea = { {0, 0}, swapchain_->extent() };
        render_pass_begin_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
        render_pass_begin_info.pClearValues = clear_values.data();

        // レンダーパスの開始
        vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width  = static_cast<float>(swapchain_->extent().width);
        viewport.height = static_cast<float>(swapchain_->extent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);

        VkRect2D scissor{ {0, 0}, swapchain_->extent() };
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);

        // ディスクリプタセットのバインド（レイアウトは 2 本のパイプラインで共通なので使い回せる）
        // set=0（カメラUBO）はフレーム先頭で1回だけバインドする。
        VkDescriptorSet descriptor_set = descriptor_sets_[current_frame_];
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_opaque_->layout(), 0, 1, &descriptor_set, 0, nullptr);

        VkDescriptorSet bindless = textures_->bindless_set();
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_opaque_->layout(), 1, 1, &bindless, 0, nullptr);

        VkDescriptorSet environment_set = environment_sets_[current_frame_];
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_shadow_->layout(), 2, 1, &environment_set, 0, nullptr);

        // 記録（不透明パス → 半透明パス）
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_opaque_->handle());
        record_draw_items(command_buffer, *pipeline_opaque_, opaque_items_, 0, true);

        // TODO(②-9): スカイボックスを入れる。

        // 半透明は depthWrite=FALSE のパイプライン（phase11 ①）
        // ★ 半透明はインスタンス化しない（描画順が正しさそのもの。まとめると順序が壊れる。D-5）。
        //   ただし InstanceData 経由でデータを渡す形は共通なので、シェーダは1本で済む。
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_transparent_->handle());
        record_draw_items(command_buffer, *pipeline_transparent_, transparent_items_, static_cast<uint32_t>(opaque_items_.size()), false);

        // レンダーパスの終了
        vkCmdEndRenderPass(command_buffer);
    }

    void Renderer::create_surface() {
        glfwCreateWindowSurface(instance_->handle(), window_->handle(), nullptr, &surface_);
    }

    void Renderer::create_framebuffers() {

        for (const auto& image_view : swapchain_->image_views()) {
            std::vector<VkImageView> image_views;
            image_views.push_back(image_view);
            image_views.push_back(depth_image_->view());

            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = render_pass_->handle();
            framebuffer_info.attachmentCount = 2;
            framebuffer_info.pAttachments = image_views.data();
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
    // phase12 手順3: 1つのセットに2 binding を詰めるのをやめ、用途ごとに2つのセットへ分離する。
    void Renderer::create_descriptor_set_layout() {
        // set=0: カメラUBO（フレームごとに1個。フレーム先頭で1回だけバインドする）
        VkDescriptorSetLayoutBinding ubo_layout_binding{};
        ubo_layout_binding.binding = 0;
        ubo_layout_binding.descriptorCount = 1;
        ubo_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        // set=0 に binding=1 として per-instance の SSBO を追加する。
        VkDescriptorSetLayoutBinding instance_binding{};
        instance_binding.binding         = 1;
        instance_binding.descriptorCount = 1;
        instance_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        instance_binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        // ★ phase14 ① 以降、InstanceData は model と material_index だけ。
        //   material_index は vert で読んで flat 補間で frag へ渡すので、FRAGMENT は不要のまま。

        // set=0 に binding=2 として 光源 の SSBO を追加する。
        VkDescriptorSetLayoutBinding light_binding{};
        light_binding.binding         = 2;
        light_binding.descriptorCount = 1;
        light_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        light_binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        // ★ set=1（bindless）と違い、こちらは UPDATE_AFTER_BIND 不要
        //   （フレーム先頭でバインドする前に書き終えているため）。
        const std::array bindings{ ubo_layout_binding, instance_binding, light_binding };
        VkDescriptorSetLayoutCreateInfo camera_layout_info{};
        camera_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        camera_layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        camera_layout_info.pBindings    = bindings.data();

        if (vkCreateDescriptorSetLayout(device_->handle(), &camera_layout_info, nullptr, &camera_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Renderer::create_descriptor_set_layout : カメラ用ディスクリプタセットレイアウトの作成に失敗しました！");
        }

        //   ★ PARTIALLY_BOUND が無いと、未書き込みの枠が1つでもあるとバインド時に不正になる。
        //     kMaxTextures=64 に対して実際は数枚しか登録しないので、これが無いと動かない。
        VkDescriptorSetLayoutBinding sampler_layout_binding{};
        sampler_layout_binding.binding = 0;
        sampler_layout_binding.descriptorCount = kMaxTextures;
        sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // set=1 に binding=1 としてマテリアル SSBO を足す。
        VkDescriptorSetLayoutBinding material_binding{};
        material_binding.binding         = 1;
        material_binding.descriptorCount = 1;
        material_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        material_binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;  // frag から直接引く
        //   ★ binding_flags / flags_info も2要素の配列にすること。
        //     bindingCount は pBindings の数と一致していなければならない（不一致は即バリデーション違反）。
        //     binding=1 側のフラグは **0**（UPDATE_AFTER_BIND_BIT を付けない）。
        //     付けると descriptorBindingStorageBufferUpdateAfterBind の機能有効化が必要になるが、
        //     マテリアルはバインド前に書き終えているので通常のバインディングで足りる（D-2 の注記）。
        //     プール側の UPDATE_AFTER_BIND_BIT は set 単位なのでそのままでよい。
        const std::array material_bindings{ sampler_layout_binding, material_binding };
        constexpr std::array<VkDescriptorBindingFlags, 2> binding_flag = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |      // 64枠中3枚だけ埋まっていてよい
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,     // バインド後に書き込んでよい
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
        };

        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount  = static_cast<uint32_t>(material_bindings.size()),        // ★ pBindings の数と一致させる
            .pBindingFlags = binding_flag.data(),
        };

        VkDescriptorSetLayoutCreateInfo material_layout_info{};
        material_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        material_layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        material_layout_info.bindingCount = static_cast<uint32_t>(material_bindings.size());
        material_layout_info.pBindings = material_bindings.data();
        material_layout_info.pNext = &flags_info;

        if (vkCreateDescriptorSetLayout(device_->handle(), &material_layout_info, nullptr, &material_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Renderer::create_descriptor_set_layout : マテリアル用ディスクリプタセットレイアウトの作成に失敗しました！");
        }

        // set=2「ライティング環境」のレイアウトを作る（D-1）。
        std::array<VkDescriptorSetLayoutBinding, 4> env_bindings{};
        for (std::size_t i = 0; i < env_bindings.size(); i++) {
            auto& binding = env_bindings[i];

            //   binding 0..3 はすべて同じ形なので、ループで4つ作れる:
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            binding.binding         = static_cast<uint32_t>(i);
        }

        VkDescriptorSetLayoutCreateInfo env_layout_info{};
        env_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        env_layout_info.flags = 0;            // ★ set=1 と違い UPDATE_AFTER_BIND_POOL_BIT は付けない
        env_layout_info.bindingCount = 4;
        env_layout_info.pBindings = env_bindings.data();
        env_layout_info.pNext = nullptr;      // ★ binding flags も要らない

        //   → environment_set_layout_ へ格納。失敗時は throw（上2つと同じ形）。
        if (vkCreateDescriptorSetLayout(device_->handle(), &env_layout_info, nullptr, &environment_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Renderer::create_descriptor_set_layout : 環境用ディスクリプタセットレイアウトの作成に失敗しました！");
        }
        // ★ PARTIALLY_BOUND を付けない以上、**4 binding すべてを書いてからでないと
        //   バインドできない**。① の時点では binding=1..3（IBL）の実体がまだ無いので、
        //   既定テクスチャ（白）の view で埋めておくこと（①-6 末尾の注記）。
        //   「① の間だけ binding を1つに減らす」より、そちらの方が ② でレイアウトを
        //   触り直さずに済む。
    }

    // UBOの作成
    void Renderer::create_uniform_buffers() {
        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            camera_ubos_.push_back(std::make_unique<UniformBuffer>(device_->allocator(), device_->handle(), sizeof(scene::CameraUBO)));
        }
    }

    // per-instance SSBO の作成（phase13 ②）
    void Renderer::create_instance_buffers() {
        // create_uniform_buffers と同じ形で kFramesInFlight 個作る。
        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            instance_buffers_.push_back(std::make_unique<InstanceBuffer>(
                device_->allocator(), device_->handle(), kMaxInstances));
        }
    }

    void Renderer::create_light_buffers() {
        // create_uniform_buffers と同じ形で kFramesInFlight 個作る。
        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            light_buffers_.push_back(std::make_unique<LightBuffer>(
                device_->allocator(), device_->handle(), kMaxLights));
        }
    }
    // ディスクリプタプールの作成
    void Renderer::create_descriptor_pool() {
        VkDescriptorPoolSize camera_pool_size;
        camera_pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        camera_pool_size.descriptorCount = static_cast<uint32_t>(kFramesInFlight);

        VkDescriptorPoolSize sampler_pool_size;
        sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_pool_size.descriptorCount = static_cast<std::uint32_t>(kMaxTextures + kFramesInFlight * 4); // 内訳: set=1 の bindless 配列 kMaxTextures 枠 + set=2（4 binding）× kFramesInFlight 個

        //   STORAGE_BUFFER の枠を +1 する。
        //   set=0 の instance_buffers_ + light_buffers_（kFramesInFlight 個）に加えて、
        //   set=1 binding=1 のマテリアル SSBO が1個要る。
        //   同じ type のプールサイズは足し合わせて1エントリにしてよい:
        //   ★ maxSets は変えなくてよい（set=1 は TextureRegistry が確保する1個のままで、
        //     binding が増えるだけ。セット数は増えない）。
        VkDescriptorPoolSize instance_pool_size{};
        instance_pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        instance_pool_size.descriptorCount = static_cast<uint32_t>(kFramesInFlight) * 2 + 1;

        std::vector<VkDescriptorPoolSize> descriptor_pools;
        descriptor_pools.push_back(camera_pool_size);
        descriptor_pools.push_back(sampler_pool_size);
        descriptor_pools.push_back(instance_pool_size);

        VkDescriptorPoolCreateInfo descriptor_pool_info{};
        descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // (phase13 ①-2): bindless 化に伴いプールを調整する。
        descriptor_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        descriptor_pool_info.maxSets = static_cast<uint32_t>(kFramesInFlight) * 2 + 1;  // 内訳: set=0 が kFramesInFlight 個 + set=1 が1個 + set=2 が kFramesInFlight 個。
        descriptor_pool_info.poolSizeCount = static_cast<uint32_t>(descriptor_pools.size());
        descriptor_pool_info.pPoolSizes = descriptor_pools.data();

        if (vkCreateDescriptorPool(device_->handle(), &descriptor_pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
            throw std::runtime_error("Renderer::create_descriptor_pool : 記述プールの作成に失敗しました。");
        }
    }

    void Renderer::create_descriptor_sets() {
        // -- set=0: カメラUBO（フレームごとに1個）--
        descriptor_sets_.resize(kFramesInFlight);
        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = descriptor_pool_;
            alloc_info.descriptorSetCount = 1;
            alloc_info.pSetLayouts = &camera_set_layout_;

            if (vkAllocateDescriptorSets(device_->handle(), &alloc_info, &descriptor_sets_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Renderer::create_descriptor_sets : カメラ用ディスクリプタセットの確保に失敗しました。");
            }

            VkDescriptorBufferInfo buffer_info{};
            buffer_info.buffer = camera_ubos_[i]->handle();
            buffer_info.offset = 0;
            buffer_info.range = sizeof(scene::CameraUBO);

            VkWriteDescriptorSet ubo_write{};
            ubo_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ubo_write.dstSet = descriptor_sets_[i];
            ubo_write.dstBinding = 0;
            ubo_write.dstArrayElement = 0;
            ubo_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            ubo_write.descriptorCount = 1;
            ubo_write.pBufferInfo = &buffer_info;

            // binding=1 へ instance_buffers_[i] を書き込む。
            VkDescriptorBufferInfo instance_info{};
            instance_info.buffer = instance_buffers_[i]->handle();
            instance_info.offset = 0;
            instance_info.range  = VK_WHOLE_SIZE;

            VkWriteDescriptorSet instance_write{};
            instance_write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            instance_write.dstSet          = descriptor_sets_[i];
            instance_write.dstBinding      = 1;
            instance_write.dstArrayElement = 0;
            instance_write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            instance_write.descriptorCount = 1;
            instance_write.pBufferInfo     = &instance_info;

            // binding=2 へ light_buffers_[i] を書き込む。
            VkDescriptorBufferInfo light_info{};
            light_info.buffer = light_buffers_[i]->handle();
            light_info.offset = 0;
            light_info.range  = VK_WHOLE_SIZE;

            VkWriteDescriptorSet light_write{};
            light_write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            light_write.dstSet          = descriptor_sets_[i];
            light_write.dstBinding      = 2;
            light_write.dstArrayElement = 0;
            light_write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            light_write.descriptorCount = 1;
            light_write.pBufferInfo     = &light_info;

            //   3件をまとめて vkUpdateDescriptorSets へ渡す（配列にして count=3）。
            std::array writes = { ubo_write, instance_write, light_write };
            vkUpdateDescriptorSets(device_->handle(), writes.size(), writes.data(), 0, nullptr);
        }

        // set=2: ライティング環境（フレームごとに1個）--
        environment_sets_.resize(kFramesInFlight);
        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            //   VkDescriptorSetAllocateInfo で environment_set_layout_ から1個確保する
            VkDescriptorSetAllocateInfo env_info{};
            env_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            env_info.descriptorPool = descriptor_pool_;
            env_info.descriptorSetCount = 1;
            env_info.pSetLayouts = &environment_set_layout_;

            if (vkAllocateDescriptorSets(device_->handle(), &env_info, &environment_sets_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Renderer::create_descriptor_sets : 環境用ディスクリプタセットの確保に失敗しました。");
            }
        }
        // ★ **確保するだけで、中身はここでは書かない。**
        //   binding=0（シャドウマップ）の実体は ①-2、binding=1..3（IBL）は ② で作る。
        //   どちらもこの関数より後に生成されるので、ここで書きようがない。
        //
        // ★ そして ⓪ の段階では environment_sets_ を**バインドもしないこと**。
        //   PARTIALLY_BOUND を付けていないので、一度も書いていないセットを
        //   バインドした時点で不正になる。⓪ の完了条件は「絵が phase15 と同じ」なので、
        //   set=2 は「枠だけ用意して触らない」が正しい状態。
        //
        // ★ 戻り値の確認も入れること（vkAllocateDescriptorSets が失敗すると
        //   VK_NULL_HANDLE が返り、バインド時に落ちる）。既存の set=0 のループも同様。
    }

    // 全テクスチャで共有するサンプラーの生成
    void Renderer::create_sampler() {
        // サンプラーを生成する (phase8プラン 項目5)
        // ★ phase16 ⓪-3 で SamplerConfig が入ったので、ここは「既定構築の config を
        //   省略して渡している」状態。挙動は phase15 までと変わらない。
        //   ① で比較サンプラ（shadow_sampler_）、② でキューブ用サンプラを
        //   この関数に足していく。
        sampler_ = std::make_unique<Sampler>(physical_device_, device_->handle());

        shadow_sampler_ = std::make_unique<Sampler>(physical_device_, device_->handle(),
            SamplerConfig{
                .filter = VK_FILTER_LINEAR,   // ★ LINEAR でハードウェア 2x2 PCF が効く
               .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
               .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,  // ★ 白 = 深度 1.0 = 影なし
               .anisotropy = false,
               .compare_enable = true,
               .compare_op = VK_COMPARE_OP_LESS_OR_EQUAL,
               .max_lod = 0.0f
            }
        );
    }

    // 光源空間行列（phase16 ①-3）。
    glm::mat4 Renderer::compute_light_view_projection(const glm::vec3& light_direction) {
        constexpr glm::vec3 center{ 0.0f };     // v1 は原点固定
        const glm::vec3 eye = center - light_direction * kShadowDistance;  // 光の来る側へ引く
        const glm::vec3 up = glm::abs(light_direction.y) > 0.99 ? glm::vec3{0, 0, 1} : glm::vec3{0, 1, 0};
        const auto view = glm::lookAt(eye, center, up);
        auto proj = glm::ortho(-kShadowOrthoExtent, kShadowOrthoExtent, -kShadowOrthoExtent, kShadowOrthoExtent, kShadowNear, kShadowFar);
        proj[1][1] *= -1;  // ★ カメラ（camera.cpp）と同じく Y を反転する
        return proj * view;
        // ★ glm::ortho は GLM_FORCE_DEPTH_ZERO_TO_ONE により z が [0,1]。追加の補正は要らない。
        // ★ Y 反転が要る理由は「上下」ではなく**三角形の巻き順**。
        //   - 影の位置の上下は、反転してもしなくても正しく出る。書き込み（シャドウパス）と
        //     読み出し（triangle.frag の uv = ndc.xy * 0.5 + 0.5）が同じ行列を使うので、
        //     どちらでも同じ向きになる。
        //   - 一方、Y を反転するとスクリーン上の巻き順が逆になる。本パスは「反転あり + CCW が表」で
        //     動いているので、シャドウパスだけ反転しないと表裏の判定が逆転し、
        //     cull_mode = FRONT が実際には**背面カリング**として働く
        //     （①-8 のアクネ対策が効かず、「FRONT にしても縞が減らない」になる）。
    }

    // シャドウマップを kFramesInFlight 枚作る（phase16 ①-2）。
    void Renderer::create_shadow_maps() {
        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            shadow_maps_.push_back(std::make_unique<ShadowMap>(
                device_->handle(), device_->allocator(),
                shadow_render_pass_->handle(), kShadowMapSize, depth_format_));
        }
    }

    // set=2 を書く（phase16 ①-6）。
    void Renderer::write_environment_sets() {
        VkImageView white_view = textures_->view(textures_->white_texture());

        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            std::array<VkDescriptorImageInfo, 4> infos{};
            infos[0] = {
                .sampler = shadow_sampler_->handle(),
                .imageView = shadow_maps_[i]->view(),
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            };

            // [1..3] { sampler_->handle(), white_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }
            //     ★ ② までの仮置き。シェーダがまだ binding=1..3 を宣言していないので、
            //       2D の view を入れておいても問題にならない。
            //       ② で samplerCube を宣言した時点で、ここはキューブの view に**必ず**差し替えること
            //       （2D の view を samplerCube で読むとバリデーションエラー）。
            for (std::size_t j = 1; j < infos.size(); j++) {
                infos[j] = {
                    .sampler = sampler_->handle(),
                    .imageView = white_view,
                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                };
            }

            //     VkWriteDescriptorSet を binding ごとに4本（dstSet = environment_sets_[i]）
            std::array<VkWriteDescriptorSet, 4> write_sets{};
            for (std::size_t j = 0; j < infos.size(); j++) {
                auto& set = write_sets[j];
                set.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                set.dstSet = environment_sets_[i];
                set.dstBinding = j;
                set.dstArrayElement = 0;
                set.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                set.descriptorCount = 1;
                set.pImageInfo = &infos[j];
            }

            vkUpdateDescriptorSets(device_->handle(), write_sets.size(), write_sets.data(), 0, nullptr);
        }
    }

    // メッシュの登録・参照（phase12 手順2）
    MeshRegistry& Renderer::meshes() {
        return *meshes_;
    }

    // テクスチャの登録・参照（phase12 手順4）
    TextureRegistry& Renderer::textures() {
        return *textures_;
    }

    // マテリアルの登録・参照（phase14 ①）
    MaterialRegistry& Renderer::materials() {
        return *materials_;
    }

    void Renderer::recreate_swapchain() {
        swapchain_->set_fullscreen_exclusive(
            device_->is_fullscreen_exclusive_supported() && is_fullscreen(),
            window_->win32_window()
        );

        window_->wait_while_minimized();
        vkDeviceWaitIdle(device_->handle());

        destroy_framebuffers();

        const auto& image_format = swapchain_->image_format(); // ★ ここで取得しておく。recreate() 後に変わる可能性があるため。
        swapchain_->recreate(window_->width(), window_->height());
        if (swapchain_->image_format() != image_format) {
            // 同サーフェス・同物理デバイスなら変わらないはずで、他のバグが発生する可能性があるので、落とす。
            throw std::runtime_error("Renderer::recreate_swapchain : スワップチェーン再作成により、フォーマットが変わりました。\n" + std::to_string(image_format) + " -> " + std::to_string(swapchain_->image_format()));
        }

        depth_image_ = std::make_unique<DepthImage>(physical_device_, device_->handle(), device_->allocator(), swapchain_->extent(), depth_format_);

        create_framebuffers();

        // 同期オブジェクトの再作成
        sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight, framebuffers_.size());
    }

    bool Renderer::is_key_pressed(int key) const {
        return window_->is_key_pressed(key);
    }

    void Renderer::set_key_callback(Window::KeyCallback callback) {
        window_->set_key_callback(std::move(callback));
    }

    void Renderer::set_cursor_pos_callback(Window::CursorPosCallback callback) {
        window_->set_cursor_pos_callback(std::move(callback));
    }

    void Renderer::set_mouse_button_callback(Window::MouseButtonCallback callback) {
        window_->set_mouse_button_callback(std::move(callback));
    }

    void Renderer::set_scroll_callback(Window::ScrollCallback callback) {
        window_->set_scroll_callback(std::move(callback));
    }

    void Renderer::set_cursor_captured(bool captured) {
        window_->set_cursor_captured(captured);
    }

    void Renderer::set_fullscreen(bool enabled) {
        window_->set_fullscreen(enabled);
    }

    // -- windowの委譲メソッド --


    bool Renderer::is_fullscreen() const {
        return window_->is_fullscreen();
    }

    bool Renderer::is_focused() const {
        return window_->is_focused();
    }
}  // namespace sq::graphics
