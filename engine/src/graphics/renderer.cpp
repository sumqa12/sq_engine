#include "sq/graphics/renderer.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <fmt/format.h>
#include <GLFW/glfw3.h>
#include <glm/ext/matrix_transform.hpp>

#include "sq/scene/camera.hpp"
#include "sq/scene/frustum.hpp"
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

        // 8. ディスクリプタセットレイアウトの作成（カメラUBO用、set=0）
        create_descriptor_set_layout();

        // 9. UBOの作成
        create_uniform_buffers();

        // 10. サンプラーを生成する（TextureRegistry へ渡すのでプールより前に必要）
        create_sampler();

        // 11. インスタンスバッファの作成
        create_instance_buffers();

        // 12. ディスクリプタプールの作成
        create_descriptor_pool();
        create_descriptor_sets();

        // 13. パイプラインの作成（phase11 ①: 不透明用・半透明用の 2 本。SPV とレイアウトは共通）
        // phase12 手順3: index が set 番号に対応する（[0]=カメラ, [1]=マテリアル）。
        const std::vector<VkDescriptorSetLayout> set_layouts = { camera_set_layout_, material_set_layout_ };

        pipeline_opaque_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
                                            "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
                                            set_layouts,
                                            PipelineConfig{ .depth_write_enable = true,  .blend_enable = false });

        pipeline_transparent_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
                                            "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
                                            set_layouts,
                                            PipelineConfig{ .depth_write_enable = false, .blend_enable = true });

        // 14. フレームバッファの作成
        create_framebuffers();

        // 15. コマンドバッファの作成
        command_buffers_ = std::make_unique<CommandBuffers>(device_->handle(), *queue_family_indices_.graphics_family, kFramesInFlight);

        // 16. 同期オブジェクトの作成
        sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight, framebuffers_.size());

        // 17. アセットレジストリの生成（phase12 手順2・4）
        // 実際の登録はアプリ側が renderer.meshes() / renderer.textures() 経由で行う
        // （例: graphics::add_cube_mesh(renderer.meshes())）。
        meshes_ = std::make_unique<MeshRegistry>(
            device_->allocator(), device_->handle(),
            *queue_family_indices_.graphics_family, device_->graphics_queue());

        textures_ = std::make_unique<TextureRegistry>(
            physical_device_, device_->handle(), device_->allocator(),
            *queue_family_indices_.graphics_family, device_->graphics_queue(),
            descriptor_pool_, material_set_layout_, sampler_->handle(), kMaxTextures);

        // 既定テクスチャを最初に登録する（Material::albedo が無効なエンティティが使う）。
        // 最初に load したものが TextureRegistry::default_texture() になる。
        textures_->load("textures/default.png");
    }

    Renderer::~Renderer() {
        vkDeviceWaitIdle(device_->handle());
        destroy_framebuffers();
        // メッシュ・テクスチャ（GPUリソース）は GpuAllocator（= Device）より前に破棄する
        // （phase11 ③ の不変条件）。ディスクリプタセットはこの後のプール破棄でまとめて解放される。
        meshes_.reset();
        textures_.reset();
        sync_objects_.reset();
        command_buffers_.reset();
        pipeline_transparent_.reset();
        pipeline_opaque_.reset();

        vkDestroyDescriptorPool(device_->handle(), descriptor_pool_, nullptr);
        // phase12 手順3: レイアウトは2つになった（セットはプール破棄でまとめて解放される）。
        vkDestroyDescriptorSetLayout(device_->handle(), camera_set_layout_, nullptr);
        vkDestroyDescriptorSetLayout(device_->handle(), material_set_layout_, nullptr);

        sampler_.reset();
        camera_ubos_.clear();
        instance_buffers_.clear();
        descriptor_sets_.clear();
        descriptor_pool_ = VK_NULL_HANDLE;
        camera_set_layout_ = VK_NULL_HANDLE;
        material_set_layout_ = VK_NULL_HANDLE;

        render_pass_.reset();
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

        // 3. コマンドバッファの記録と送信
        command_buffers_->record(current_frame_, [this, &image_index, &registry](VkCommandBuffer command_buffer, std::size_t _) {
            std::array<VkClearValue, 2> clear_values{};
            clear_values[0].color = { {0.2f, 0.2f, 0.2f, 1.0f} };
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

            // アスペクト比の計算
            float aspect_ratio = static_cast<float>(swapchain_->extent().width) / static_cast<float>(swapchain_->extent().height);

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

            // カメラUBOの更新
            scene::CameraUBO camera_ubo{};
            camera_ubo.view_projection = view_projection;
            camera_ubos_[current_frame_]->update(&camera_ubo, sizeof(camera_ubo));

            // ディスクリプタセットのバインド（レイアウトは 2 本のパイプラインで共通なので使い回せる）
            // phase12 手順3: set=0（カメラUBO）はフレーム先頭で1回だけバインドする。
            VkDescriptorSet descriptor_set = descriptor_sets_[current_frame_];
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline_opaque_->layout(), 0, 1, &descriptor_set, 0, nullptr);

            // phase13
            VkDescriptorSet bindless = textures_->bindless_set();
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline_opaque_->layout(), 1, 1, &bindless, 0, nullptr);

            // ---- phase12 手順6: 収集 → ソート → バッチ記録 ----
            // 描画順を「その場で決める」のをやめ、いったん全件を集めてから並べ替えて記録する。
            // これにより不透明のバインド切り替えを最小化でき、フラスタムカリング等の置き場もできる。

            // 1. 収集（不透明・半透明に振り分ける）
            // MeshHandle を持つエンティティのみが描画対象（カメラ等の非描画エンティティは除外される）。
            //
            // phase13 ⑤: この view-projection から視錐台を作り、収集の時点で画面外を落とす。
            // カメラ解決後・収集前に1回だけ作ればよい（フレーム中は不変）。
            const scene::Frustum frustum = scene::Frustum::from_view_projection(view_projection);

            opaque_items_.clear();
            transparent_items_.clear();

            registry.view<scene::Transform, scene::MeshHandle>().each(
                [&](ecs::Entity e, scene::Transform& t, scene::MeshHandle& mh) {
                    // 未登録のメッシュを指すハンドルはスキップする
                    if (!meshes_->contains(mh.id)) { return; }

                    // (phase13 ⑤-3): フラスタムカリング。ローカルの境界球をワールドへ移して判定する。
                    const MeshRegistry::Entry& entry = meshes_->get(mh.id);
                    const glm::mat4 model = t.model();

                    //   中心は model で変換する（平行移動を含めるため vec4 の w は 1）
                    const auto world_center = glm::vec3(model * glm::vec4(entry.bounds.center, 1.0f));

                    //   回転は球を変えない。非等方スケールは最大成分で保守的に見積もる
                    const float max_scale = std::max({ t.scale.x, t.scale.y, t.scale.z });
                    if (const float world_radius = entry.bounds.radius * max_scale
                        ; !frustum.intersects(world_center, world_radius)) {
                        return;
                    }

                    // Material を1回だけ引く。持たないエンティティは既定マテリアル
                    // （既定テクスチャ・白 tint・不透明）として扱う（後方互換）。
                    scene::Material material{};
                    if (registry.has<scene::Material>(e)) {
                        material = registry.get<scene::Material>(e);
                    }

                    // albedo が無効／未登録なら既定テクスチャへフォールバックする
                    const scene::TextureId texture = textures_->contains(material.albedo)
                        ? material.albedo
                        : textures_->default_texture();

                    // model 行列を合成し（T * R * S）、描画用データを作る
                    const glm::vec3 d = t.position - cam_pos;
                    const DrawItem item{
                        .instance_data = InstanceData{ .model = model, .base_color = material.base_color, .texture_index = texture },
                        .mesh = mh.id,
                        .distance_sq = glm::dot(d, d),  // 2乗距離（順序比較にしか使わないので sqrt 不要）
                    };

                    (material.transparent ? transparent_items_ : opaque_items_).push_back(item);
                }
            );

            // 2. meshでソート
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

            // ソート済みの並びから InstanceData 配列を作り、1回で転送する。
            instances_.clear();
            instances_.reserve(opaque_items_.size() + transparent_items_.size());
            // 不透明 → 半透明 の順に詰める（★この順序が first_instance の基準になる）:
            for (const DrawItem& item : opaque_items_) {
                instances_.push_back(item.instance_data);
            }
            for (const DrawItem& item : transparent_items_) {
                instances_.push_back(item.instance_data);
            }

            // 上限超えたら警告を出し、リサイズ
            if (instances_.size() > kMaxInstances) {
                printf("Warning: instance count %zu exceeds kMaxInstances %zu. Truncating.\n",
                       instances_.size(), kMaxInstances);
                instances_.resize(kMaxInstances);
                // 描画側も同じ位置で止める（転送されていない範囲を描かないため）
                if (opaque_items_.size() > kMaxInstances) {
                    opaque_items_.resize(kMaxInstances);
                    transparent_items_.clear();
                } else {
                    transparent_items_.resize(kMaxInstances - opaque_items_.size());
                }
            }

            instance_buffers_[current_frame_]->update(instances_.data(), instances_.size());

            // 3. 記録（不透明パス → 半透明パス）
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_opaque_->handle());
            record_draw_items(command_buffer, *pipeline_opaque_, opaque_items_, 0, true);

            // 半透明は depthWrite=FALSE のパイプライン（phase11 ①）
            // ★ 半透明はインスタンス化しない（描画順が正しさそのもの。まとめると順序が壊れる。D-5）。
            //   ただし InstanceData 経由でデータを渡す形は共通なので、シェーダは1本で済む。
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_transparent_->handle());
            record_draw_items(command_buffer, *pipeline_transparent_, transparent_items_, static_cast<uint32_t>(opaque_items_.size()), false);

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
        ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        // set=0 に binding=1 として per-instance の SSBO を追加する。
        VkDescriptorSetLayoutBinding instance_binding{};
        instance_binding.binding         = 1;
        instance_binding.descriptorCount = 1;
        instance_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        instance_binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        // ★ base_color / texture_index は vert で読んで frag へ渡す形にするので、
        //   FRAGMENT は不要（frag から直接引くなら VERTEX | FRAGMENT にする）。

        // ★ set=1（bindless）と違い、こちらは UPDATE_AFTER_BIND 不要
        //   （フレーム先頭でバインドする前に書き終えているため）。
        const std::array bindings{ ubo_layout_binding, instance_binding };
        VkDescriptorSetLayoutCreateInfo camera_layout_info{};
        camera_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        camera_layout_info.pBindings = &ubo_layout_binding;
        camera_layout_info.bindingCount = bindings.size();
        camera_layout_info.pBindings    = bindings.data();

        if (vkCreateDescriptorSetLayout(device_->handle(), &camera_layout_info, nullptr, &camera_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Renderer::create_descriptor_set_layout : カメラ用ディスクリプタセットレイアウトの作成に失敗しました！");
        }
        
        constexpr VkDescriptorBindingFlags binding_flags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |      // 64枠中3枚だけ埋まっていてよい
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;     // バインド後に書き込んでよい

        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
        flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flags_info.bindingCount  = 1;                        // ★ pBindings の数と一致させる
        flags_info.pBindingFlags = &binding_flags;
        //   ★ PARTIALLY_BOUND が無いと、未書き込みの枠が1つでもあるとバインド時に不正になる。
        //     kMaxTextures=64 に対して実際は数枚しか登録しないので、これが無いと動かない。

        VkDescriptorSetLayoutBinding sampler_layout_binding{};
        sampler_layout_binding.binding = 0;
        sampler_layout_binding.descriptorCount = kMaxTextures;
        sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo material_layout_info{};
        material_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        material_layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        material_layout_info.bindingCount = 1;
        material_layout_info.pBindings = &sampler_layout_binding;
        material_layout_info.pNext = &flags_info;

        if (vkCreateDescriptorSetLayout(device_->handle(), &material_layout_info, nullptr, &material_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Renderer::create_descriptor_set_layout : マテリアル用ディスクリプタセットレイアウトの作成に失敗しました！");
        }
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

    // ディスクリプタプールの作成
    void Renderer::create_descriptor_pool() {
        VkDescriptorPoolSize camera_pool_size;
        camera_pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        camera_pool_size.descriptorCount = static_cast<uint32_t>(kFramesInFlight);
        
        VkDescriptorPoolSize sampler_pool_size;
        sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_pool_size.descriptorCount = kMaxTextures;

        VkDescriptorPoolSize instance_pool_size{};
        instance_pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        instance_pool_size.descriptorCount = static_cast<uint32_t>(kFramesInFlight);

        std::vector<VkDescriptorPoolSize> descriptor_pools;
        descriptor_pools.push_back(camera_pool_size);
        descriptor_pools.push_back(sampler_pool_size);
        descriptor_pools.push_back(instance_pool_size);

        VkDescriptorPoolCreateInfo descriptor_pool_info{};
        descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // (phase13 ①-2): bindless 化に伴いプールを調整する。
        descriptor_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        descriptor_pool_info.maxSets = static_cast<uint32_t>(kFramesInFlight) + 1;
        descriptor_pool_info.poolSizeCount = static_cast<uint32_t>(descriptor_pools.size());
        descriptor_pool_info.pPoolSizes = descriptor_pools.data();

        vkCreateDescriptorPool(device_->handle(), &descriptor_pool_info, nullptr, &descriptor_pool_);
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

            vkAllocateDescriptorSets(device_->handle(), &alloc_info, &descriptor_sets_[i]);

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

            //   2件をまとめて vkUpdateDescriptorSets へ渡す（配列にして count=2）。
            std::array writes = { ubo_write, instance_write };

            vkUpdateDescriptorSets(device_->handle(), 2, writes.data(), 0, nullptr);
        }
    }

    // 全テクスチャで共有するサンプラーの生成
    void Renderer::create_sampler() {
        // サンプラーを生成する (phase8プラン 項目5)
        sampler_ = std::make_unique<Sampler>(physical_device_, device_->handle());
    }

    // メッシュの登録・参照（phase12 手順2）
    MeshRegistry& Renderer::meshes() {
        return *meshes_;
    }

    // テクスチャの登録・参照（phase12 手順4）
    TextureRegistry& Renderer::textures() {
        return *textures_;
    }

    void Renderer::recreate_swapchain() {
        swapchain_->set_fullscreen_exclusive(
            device_->is_fullscreen_exclusive_supported() && is_fullscreen(),
            window_->win32_window()
        );

        window_->wait_while_minimized();
        vkDeviceWaitIdle(device_->handle());

        destroy_framebuffers();

        swapchain_->recreate(window_->width(), window_->height());
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
