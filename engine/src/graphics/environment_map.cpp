#include "sq/graphics/environment_map.hpp"

#include <array>
#include <stdexcept>
#include <chrono>

#include <spdlog/spdlog.h>

#include "sq/graphics/hdr_texture.hpp"
#include "sq/graphics/image_utils.hpp"
#include "sq/graphics/single_time_commands.hpp"

namespace sq::graphics {
    using namespace std::chrono;
    EnvironmentMap::EnvironmentMap(VkDevice device, GpuAllocator& allocator,
                                   std::uint32_t graphics_queue_family, VkQueue graphics_queue,
                                   const std::string& hdr_path, VkSampler input_sampler)
        : device_(device), allocator_(&allocator),
          queue_family_(graphics_queue_family), queue_(graphics_queue) {

        create_bake_descriptors();
        const auto start = steady_clock::now();
        equirect_pipeline_ = std::make_unique<ComputePipeline>(device_, "shaders/equirect_to_cube.comp.spv",
        std::vector{ equirect_set_layout_ }, 0);
        const auto end = steady_clock::now();
        spdlog::info("EnvironmentMap::EnvironmentMap : シェーダのコンパイル [{}ms]", duration_cast<milliseconds>(end - start).count());

        environment_cube_ = std::make_unique<Cubemap>(device_, *allocator_, kEnvironmentCubeSize, 1, kCubeFormat);

        bake_environment_cube(hdr_path, input_sampler);
        //   ★ 所要時間を spdlog::info で出しておくと、手順6で「前計算が 1 秒を超えていないか」
        //     （フェーズ全体の検証観点）をそのまま確認できる。

        equirect_pipeline_.reset(); // もう必要ない
    }

    EnvironmentMap::~EnvironmentMap() {
        environment_cube_.reset();
        vkDestroyDescriptorPool(device_, bake_pool_, nullptr);  // ★ セットはプールごと解放される
        vkDestroyDescriptorSetLayout(device_ , equirect_set_layout_, nullptr);
        //   ★ 呼ばれる時点で GPU がアイドルであることは Renderer のデストラクタが保証する。
    }

    const Cubemap& EnvironmentMap::environment_cube() const {
        return *environment_cube_;
    }

    void EnvironmentMap::create_bake_descriptors() {
        // 前計算用のディスクリプタセットレイアウトとプールを作る（②-4）。
        //   equirect_set_layout_:
        //     binding=0  COMBINED_IMAGE_SAMPLER  COMPUTE  入力（HDR の 2D）
        //     binding=1  STORAGE_IMAGE           COMPUTE  出力（環境キューブの storage_view(0)）
        VkDescriptorSetLayoutBinding input_binding = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };

        VkDescriptorSetLayoutBinding output_binding = {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };

        std::array bake_bindings = { input_binding, output_binding };
        VkDescriptorSetLayoutCreateInfo equirect_info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = bake_bindings.size(),
            .pBindings = bake_bindings.data(),
        };
        if (vkCreateDescriptorSetLayout(device_, &equirect_info, nullptr, &equirect_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("EnvironmentMap::create_bake_descriptors: 前計算用のディスクリプタセットレイアウトの作成に失敗しました。");
        }

        //   bake_pool_: **Renderer のプールとは別に持つ**。
        //     ★ Renderer のプールは UPDATE_AFTER_BIND で作られており、STORAGE_IMAGE の枠も無い。
        //       前計算の都合でそちらを広げると、描画側のプール計算（⓪-4）が汚れる。
        VkDescriptorPoolSize input_pool_size = {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1
        };

        VkDescriptorPoolSize output_pool_size = {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1
        };

        std::array pools = {input_pool_size, output_pool_size};
        VkDescriptorPoolCreateInfo bake_pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = 0,
            .maxSets = kMaxBakeSets,
            .poolSizeCount = static_cast<uint32_t>(pools.size()),
            .pPoolSizes = pools.data(),
        };

        if (vkCreateDescriptorPool(device_, &bake_pool_info, nullptr, &bake_pool_)) {
            throw std::runtime_error("EnvironmentMap::create_bake_descriptors : ディスクリプタプールの作成に失敗しました。");
        }

        //     ★ 手順5では set 1 個 / COMBINED_IMAGE_SAMPLER 1 / STORAGE_IMAGE 1 で足りる。
        //       手順6で irradiance（1）+ prefiltered（mip ごとに 1 = 5）+ BRDF LUT（1）が加わるので、
        //       そのとき kMaxBakeSets と各枠を増やすこと（足りないと vkAllocateDescriptorSets が
        //       VK_ERROR_OUT_OF_POOL_MEMORY を返す。戻り値は必ず見ること）。
    }

    void EnvironmentMap::bake_environment_cube(const std::string& hdr_path, VkSampler input_sampler) {
        // HDR の equirectangular -> 環境キューブ（②-4）。
        //   1. HdrTexture を作る（このスコープだけ生きる一時オブジェクト）
        auto start = steady_clock::now();
        auto hdr = HdrTexture(device_, *allocator_, queue_family_, queue_, hdr_path);
        auto end = steady_clock::now();
        spdlog::info("EnvironmentMap::bake_environment_cube : HdrTextureの作成 [{}ms]", duration_cast<milliseconds>(end - start).count());

        //   2. ディスクリプタセットを確保して binding=0 / 1 を書く
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = bake_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &equirect_set_layout_;

        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(device_, &alloc_info, &set) != VK_SUCCESS) {
            throw std::runtime_error("EnvironmentMap::bake_environment_cube : ディスクリプタセットの確保に失敗しました。");
        }

        VkDescriptorImageInfo input_image_info = {
            .sampler = input_sampler,
            .imageView = hdr.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        VkDescriptorImageInfo output_image_info = {
            .sampler = VK_NULL_HANDLE,
            .imageView = environment_cube_->storage_view(0),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };

        VkWriteDescriptorSet input_write_set = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &input_image_info
        };
        VkWriteDescriptorSet output_write_set = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &output_image_info
        };

        std::array write_sets = { input_write_set, output_write_set };
        vkUpdateDescriptorSets(device_, write_sets.size(), write_sets.data(), 0, nullptr);

        //   3. SingleTimeCommands の中で:
        SingleTimeCommands cmd(device_, queue_family_, queue_);

        transition_image_layout(cmd.handle(), environment_cube_->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,  VK_IMAGE_LAYOUT_GENERAL, 0, environment_cube_->mips(), 0, 6);

        vkCmdBindPipeline(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, equirect_pipeline_->handle());
        vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, equirect_pipeline_->layout(),
            0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd.handle(), environment_cube_->size() / 8, environment_cube_->size() / 8, 6); // ★ z = 面番号

        transition_image_layout(cmd.handle(), environment_cube_->handle(),
            VK_IMAGE_LAYOUT_GENERAL,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, environment_cube_->mips(), 0, 6);

        start = steady_clock::now();
        cmd.submit_and_wait();
        end = steady_clock::now();
        spdlog::info("EnvironmentMap::EnvironmentMap : GPUでの処理 [{}ms]", duration_cast<milliseconds>(end - start).count());
    }
}  // namespace sq::graphics
