#include "sq/graphics/renderer.hpp"

#include <array>
#include <stdexcept>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <GLFW/glfw3.h>
#include <glm/ext/matrix_transform.hpp>

#include "sq/scene/camera.hpp"
#include "sq/scene/position.hpp"
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

        // 12. パイプラインの作成
        pipeline_ = std::make_unique<GraphicsPipeline>(device_->handle(), render_pass_->handle(), swapchain_->extent(),
                                            "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
                                            descriptor_set_layout_);

        // 13. フレームバッファの作成
        create_framebuffers();

        // 14. コマンドバッファの作成
        command_buffers_ = std::make_unique<CommandBuffers>(device_->handle(), *queue_family_indices_.graphics_family, kFramesInFlight);

        // 15. 同期オブジェクトの作成
        sync_objects_ = std::make_unique<SyncObjects>(device_->handle(), kFramesInFlight, framebuffers_.size());

        // 16. デモ用三角形メッシュの作成（ECS連携）
        create_cube_mesh();
    }

    Renderer::~Renderer() {
        vkDeviceWaitIdle(device_->handle());
        destroy_framebuffers();
        triangle_mesh_.reset();
        cube_indices_.reset();
        sync_objects_.reset();
        command_buffers_.reset();
        pipeline_.reset();

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

            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

            // アスペクト比の計算
            float aspect_ratio = static_cast<float>(swapchain_->extent().width) / static_cast<float>(swapchain_->extent().height);

            // カメラの取得
            // TODO: アクティブカメラの選択を3段フォールバックにする（phase10プラン D-2）
            //   1. registry.view<scene::Camera, scene::ActiveCamera>().front()  … ActiveCamera タグ付き
            //   2. null なら registry.view<scene::Camera>().front()             … 後方互換（従来の挙動）
            //   3. それも null なら default_view_projection（下の既定値のまま）
            //   フォールバックした事実を毎フレームログに出さないこと。
            ecs::Entity camera_entity = registry.view<scene::Camera>().front();

            // カメラ不在時のフォールバック
            glm::mat4 view_projection = scene::Camera::default_view_projection(aspect_ratio);
            if (!camera_entity.is_null()) {
                const scene::Camera& camera = registry.get<scene::Camera>(camera_entity);
                view_projection = camera.view_projection(aspect_ratio);
            }

            // カメラUBOの更新
            scene::CameraUBO camera_ubo{};
            camera_ubo.view_projection = view_projection;
            camera_ubos_[current_frame_]->update(&camera_ubo, sizeof(camera_ubo));

            // ディスクリプタセットのバインド
            VkDescriptorSet descriptor_set = descriptor_sets_[current_frame_];
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0, 1, &descriptor_set, 0, nullptr);

            // メッシュのバインド
            triangle_mesh_->bind(command_buffer);
            cube_indices_->bind(command_buffer);

            // 描画
            registry.view<scene::Transform, scene::Position>().each([&](ecs::Entity, scene::Transform& t, scene::Position &pos) {

                // モデル行列を更新
                t.model = glm::translate(glm::mat4(1.0f), glm::vec3(pos.x, pos.y, pos.z));

                vkCmdPushConstants(command_buffer, pipeline_->layout(),
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &t.model);
                vkCmdDrawIndexed(command_buffer, cube_indices_->index_count(), 1, 0, 0, 0);
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
            camera_ubos_.push_back(std::make_unique<UniformBuffer>(physical_device_, device_->handle(), sizeof(scene::CameraUBO)));
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
            physical_device_, device_->handle(),
            *queue_family_indices_.graphics_family, device_->graphics_queue(),
            "textures/hsr_icon_01/Blade 1.png");
    }

    // 全テクスチャで共有するサンプラーの生成
    void Renderer::create_sampler() {
        // サンプラーを生成する (phase8プラン 項目5)
        sampler_ = std::make_unique<Sampler>(physical_device_, device_->handle());
    }

    void Renderer::create_triangle_mesh() {
        std::vector<Vertex> vertices = {
            {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
            {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}}
        };
        triangle_mesh_ = std::make_unique<VertexBuffer>(physical_device_, device_->handle(), vertices);
    }

    void Renderer::create_cube_mesh() {
        // テクスチャを貼るため、面ごとに独立した24頂点構成へ作り直す（phase8プラン 項目9）。
        // 各面の4頂点に uv = {0,0}/{1,0}/{1,1}/{0,1} を割り当て、インデックスは面ごと6個×6面=36個にする。
        // （現状は8頂点共有のため面ごとのUVが破綻する。uv 未指定の頂点は {0,0} に値初期化される）
        // 左- 右+ 上- 下+ 手前- 奥+
        std::vector<Vertex> vertices = {
            // 手前
            {{-0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
            {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
            {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
            {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

            // 奥
            {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
            {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
            {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
            {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

            // 左
            {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
            {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
            {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
            {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

            // 右
            {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
            {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
            {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
            {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

            // 上
            {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
            {{-0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
            {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
            {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

            // 下
            {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
            {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
            {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
            {{-0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},
        };

        std::vector<uint16_t> indices = {
             0,  1,  2,  0,  2,  3, // 手前
             4,  5,  6,  4,  6,  7, // 奥
             8,  9, 10,  8, 10, 11, // 右
            12, 13, 14, 12, 14, 15, // 左
            16, 17, 18, 16, 18, 19, // 上
            20, 21, 22, 20, 22, 23  // 下
        };

        triangle_mesh_ = std::make_unique<VertexBuffer>(physical_device_, device_->handle(), vertices);
        cube_indices_ = std::make_unique<IndexBuffer>(physical_device_, device_->handle(), indices);
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
