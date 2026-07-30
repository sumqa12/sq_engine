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

        depth_image_ = std::make_unique<DepthImage>(physical_device_, device_->handle(), swapchain_->extent(), depth_format_);

        // 7. レンダーパスの作成
        render_pass_ = std::make_unique<RenderPass>(device_->handle(), swapchain_->image_format(), depth_format_);

        // 8. ディスクリプタセットレイアウトの作成（カメラUBO用、set=0）
        create_descriptor_set_layout();

        // 9. UBOの作成
        create_uniform_buffers();

        // 10. テクスチャとサンプラーを生成する
        create_texture();
        create_sampler();

        // 11. ディスクリプタプールの作成
        create_descriptor_pool();
        create_descriptor_sets();

        // 12. パイプラインの作成（phase11 ①: 不透明用・半透明用の 2 本。SPV とレイアウトは共通）
        pipeline_opaque_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
                                            "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
                                            descriptor_set_layout_,
                                            PipelineConfig{ .depth_write_enable = true,  .blend_enable = false });

        pipeline_transparent_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
                                            "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
                                            descriptor_set_layout_,
                                            PipelineConfig{ .depth_write_enable = false, .blend_enable = true });

        // 13. フレームバッファの作成
        create_framebuffers();

        // 14. コマンドバッファの作成
        command_buffers_ = std::make_unique<CommandBuffers>(device_->handle(), *queue_family_indices_.graphics_family, kFramesInFlight);

        // 15. 同期オブジェクトの作成
        sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight, framebuffers_.size());

        // 16. メッシュレジストリの生成（phase12 手順2）
        // 実際のメッシュ登録はアプリ側が renderer.meshes() 経由で行う
        // （例: graphics::add_cube_mesh(renderer.meshes())）。
        meshes_ = std::make_unique<MeshRegistry>(
            device_->allocator(), device_->handle(),
            *queue_family_indices_.graphics_family, device_->graphics_queue());
    }

    Renderer::~Renderer() {
        vkDeviceWaitIdle(device_->handle());
        destroy_framebuffers();
        // メッシュ（GPUバッファ）は GpuAllocator（= Device）より前に破棄する（phase11 ③ の不変条件）。
        meshes_.reset();
        sync_objects_.reset();
        command_buffers_.reset();
        pipeline_transparent_.reset();
        pipeline_opaque_.reset();

        vkDestroyDescriptorPool(device_->handle(), descriptor_pool_, nullptr);
        vkDestroyDescriptorSetLayout(device_->handle(), descriptor_set_layout_, nullptr);

        texture_.reset();
        sampler_.reset();
        camera_ubos_.clear();
        descriptor_sets_.clear();
        descriptor_pool_ = VK_NULL_HANDLE;
        descriptor_set_layout_ = VK_NULL_HANDLE;

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
            VkDescriptorSet descriptor_set = descriptor_sets_[current_frame_];
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_opaque_->layout(), 0, 1, &descriptor_set, 0, nullptr);

            // ---- phase11 ①: 不透明パス → 半透明ソート → 半透明パス ----
            // phase12 手順2: メッシュは共有ではなく MeshHandle ごとに引いてバインドする。

            // 描画情報。model 行列・どのメッシュか・カメラからの2乗距離を持つ。
            struct DrawItem {
                glm::mat4 model;
                scene::MeshId mesh;
                float distance_sq;
            };
            std::vector<DrawItem> transparent_items;

            // 1つのメッシュをバインドして描画する（不透明パス・半透明パスで共用）。
            // 直前にバインドしたメッシュを覚えて、変化した時だけ再バインドする。
            scene::MeshId bound_mesh = scene::kInvalidMeshId;
            auto draw_mesh = [&](const GraphicsPipeline& pipeline, const glm::mat4& model, scene::MeshId mesh) {
                const auto&[vertices, indices] = meshes_->get(mesh);
                if (mesh != bound_mesh) {
                    vertices->bind(command_buffer);
                    indices->bind(command_buffer);
                    bound_mesh = mesh;
                }
                vkCmdPushConstants(command_buffer, pipeline.layout(),
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
                // インデックス数はメッシュごとに引く（共有 cube 固定をやめた点が本手順の要）
                vkCmdDrawIndexed(command_buffer, indices->index_count(), 1, 0, 0, 0);
            };

            // 1. 不透明パス（順不同。深度テストが前後関係を解決する）
            // MeshHandle を持つエンティティのみが描画対象（カメラ等の非描画エンティティは除外される）。
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_opaque_->handle());
            registry.view<scene::Transform, scene::MeshHandle>().each(
                [&](ecs::Entity e, scene::Transform& t, scene::MeshHandle& mh) {
                    // 未登録のメッシュを指すハンドルはスキップする
                    if (!meshes_->contains(mh.id)) { return; }

                    // モデル行列を合成する（T * R * S）
                    const glm::mat4 model = t.model();

                    // Material を持ち transparent なら、その場では描かず transparent_items へ回す。
                    const bool is_transparent = registry.has<scene::Material>(e)
                                                && registry.get<scene::Material>(e).transparent;
                    if (is_transparent) {
                        glm::vec3 d = t.position - cam_pos;
                        transparent_items.push_back({ model, mh.id, glm::dot(d, d) });  // 2乗距離（sqrt 不要）
                        return;
                    }

                    // 不透明はその場で描画
                    draw_mesh(*pipeline_opaque_, model, mh.id);
                });

            // 2. 半透明ソート（遠い順 = distance_sq 降順）
            // ★ 正しさが順序に依存するため、メッシュでまとめてはいけない（ソート順が絶対）。
            std::ranges::sort(transparent_items,
                [](const DrawItem& a, const DrawItem& b) {
                    return a.distance_sq > b.distance_sq;
                }
            );

            // 3. 半透明パス（back-to-front。depthWrite=FALSE のパイプライン）
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_transparent_->handle());
            for (const auto&[model, mesh, distance_sq] : transparent_items) {
                draw_mesh(*pipeline_transparent_, model, mesh);
            }

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
    void Renderer::create_descriptor_set_layout() {
        VkDescriptorSetLayoutBinding ubo_layout_binding{};
        ubo_layout_binding.binding = 0;
        ubo_layout_binding.descriptorCount = 1;
        ubo_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        // combined image sampler 用の binding を追加する (phase8 項目7)
        VkDescriptorSetLayoutBinding sampler_layout_binding{};
        sampler_layout_binding.binding = 1;
        sampler_layout_binding.descriptorCount = 1;
        sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.push_back(ubo_layout_binding);
        bindings.push_back(sampler_layout_binding);

        VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info{};
        descriptor_set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_set_layout_info.bindingCount = 2;
        descriptor_set_layout_info.pBindings = bindings.data();

        vkCreateDescriptorSetLayout(device_->handle(), &descriptor_set_layout_info, nullptr, &descriptor_set_layout_);
    }

    // UBOの作成
    void Renderer::create_uniform_buffers() {
        for (std::size_t i = 0; i < kFramesInFlight; ++i) {
            camera_ubos_.push_back(std::make_unique<UniformBuffer>(device_->allocator(), device_->handle(), sizeof(scene::CameraUBO)));
        }
    }

    // ディスクリプタプールの作成
    void Renderer::create_descriptor_pool() {
        VkDescriptorPoolSize camera_pool_size;
        camera_pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        camera_pool_size.descriptorCount = static_cast<uint32_t>(kFramesInFlight);

        VkDescriptorPoolSize sampler_pool_size;
        sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_pool_size.descriptorCount = static_cast<uint32_t>(kFramesInFlight);

        std::vector<VkDescriptorPoolSize> descriptor_pools;
        descriptor_pools.push_back(camera_pool_size);
        descriptor_pools.push_back(sampler_pool_size);

        VkDescriptorPoolCreateInfo descriptor_pool_info{};
        descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool_info.maxSets = kFramesInFlight;
        descriptor_pool_info.poolSizeCount = 2;
        descriptor_pool_info.pPoolSizes = descriptor_pools.data();

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

            VkWriteDescriptorSet ubo_write{};
            ubo_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ubo_write.dstSet = descriptor_sets_[i];
            ubo_write.dstBinding = 0;
            ubo_write.dstArrayElement = 0;
            ubo_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            ubo_write.descriptorCount = 1;
            ubo_write.pBufferInfo = &buffer_info;

            // binding=1（combined image sampler）の書き込みを追加する (phase8 項目7)
            VkDescriptorImageInfo image_info{};
            image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_info.imageView = texture_->view();
            image_info.sampler = sampler_->handle();

            VkWriteDescriptorSet image_write{};
            image_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            image_write.dstSet = descriptor_sets_[i];
            image_write.dstBinding = 1;
            image_write.dstArrayElement = 0;
            image_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            image_write.descriptorCount = 1;
            image_write.pImageInfo = &image_info;

            std::vector<VkWriteDescriptorSet> descriptor_write_sets;
            descriptor_write_sets.push_back(ubo_write);
            descriptor_write_sets.push_back(image_write);

            vkUpdateDescriptorSets(device_->handle(), 2, descriptor_write_sets.data(), 0, nullptr);
        }
    }

    // テクスチャの読み込み（textures/ の画像を Texture として生成する）。
    void Renderer::create_texture() {
        // 実行ファイル隣の textures/ にある画像を読み込む（phase8プラン 項目3・11）
        texture_ = std::make_unique<Texture>(
            physical_device_, device_->handle(), device_->allocator(),
            *queue_family_indices_.graphics_family, device_->graphics_queue(),
            "textures/hsr_icon_01/Blade 1.png");
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

    void Renderer::recreate_swapchain() {
        swapchain_->set_fullscreen_exclusive(
            device_->is_fullscreen_exclusive_supported() && is_fullscreen(),
            window_->win32_window()
        );

        window_->wait_while_minimized();
        vkDeviceWaitIdle(device_->handle());

        destroy_framebuffers();

        swapchain_->recreate(window_->width(), window_->height());
        depth_image_ = std::make_unique<DepthImage>(physical_device_, device_->handle(), swapchain_->extent(), depth_format_);

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
