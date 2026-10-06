#include "sq/graphics/texture_registry.hpp"

#include <stdexcept>
#include <spdlog/spdlog.h>

namespace sq::graphics {

    TextureRegistry::TextureRegistry(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
                                     std::uint32_t queue_family, VkQueue queue,
                                     VkDescriptorPool descriptor_pool,
                                     VkDescriptorSetLayout material_set_layout,
                                     VkSampler sampler, std::uint32_t max_textures,
                                     DeletionQueue& deletions)
        : AssetRegistry(max_textures), physical_device_(physical_device), device_(device), allocator_(&allocator),
          queue_family_(queue_family), queue_(queue),
          descriptor_pool_(descriptor_pool), material_set_layout_(material_set_layout), sampler_(sampler),
          deletions_(&deletions) {
        // 生成時は空。load() で登録していく。

        // (phase13 ①-3): 共有の bindless セット（set=1）をここで1つだけ確保する。
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &material_set_layout_;   // 配列化した set=1 のレイアウト
        if (vkAllocateDescriptorSets(device_, &alloc_info, &bindless_set_) != VK_SUCCESS) {
            throw std::runtime_error("TextureRegistry : bindless : ディスクリプタセットの確保に失敗しました。");
        }
        // ★ この時点では配列の要素が1つも書かれていないが、レイアウトに
        //   PARTIALLY_BOUND を付けてあるので確保・バインドできる。
    }

    scene::TextureId TextureRegistry::load(const std::string& path, VkFormat format) {
        //  1. 既にロード済みなら再利用する（同じ画像を何体が参照してもロードは1回）:
        if (auto it = by_path_.find({path, format}); it != by_path_.end()) { return it->second; }

        //  2. スロットの上限チェックと確保
        if (full()) {
            spdlog::error("textureRegistry::add: Too many textures");
            return default_texture_;
        }

        //  2. テクスチャ本体を読み込む:
        auto texture = std::make_unique<Texture>(
            physical_device_, device_, *allocator_, queue_family_, queue_, path, format);

        //  3. テクスチャの登録
        scene::TextureId id = register_texture(std::move(texture));

        //  4. パスの登録
        by_path_.emplace(std::pair{path, format}, id);

        return id;
    }

    scene::TextureId TextureRegistry::load_from_pixels(const unsigned char* pixels,
                                                       std::uint32_t width, std::uint32_t height,
                                                       VkFormat format) {
        //  1. スロットの上限チェックと確保
        if (full()) {
            spdlog::error("TextureRegistry::add: Too many textures");
            return default_texture_;
        }

        //  2. テクスチャ本体を読み込む
        auto texture = std::make_unique<Texture>(
            physical_device_, device_, *allocator_, queue_family_, queue_,
            pixels, width, height, format);

        //  3. テクスチャの登録してidを返す
        return register_texture(std::move(texture));
    }

    scene::TextureId TextureRegistry::register_texture(std::unique_ptr<Texture> texture) {
        auto id = emplace(TextureEntry{ std::move(texture) });

        if (default_texture_ == scene::kInvalidTextureId) { default_texture_ = id; }  // 最初の1枚を既定にする

        return id;
    }

    VkDescriptorSet TextureRegistry::bindless_set() const {
        return bindless_set_;
    }

    scene::TextureId TextureRegistry::create_white_texture() {
        //   ★ 二重登録の防止: 既に有効なら何もせず white_texture_ を返す
        //     （contains(white_texture_) で判定できる）。
        //   ★ ファイルを用意しないのは、これが「絵」ではなく「乗算の恒等元」だから。
        //     ディスクに置くとユーザーが差し替えられてしまい、中立性が保証できなくなる。
        if (contains(white_texture_)) {
            return white_texture_;
        }
        constexpr unsigned char kWhite[4] = { 255, 255, 255, 255 };
        // white は sRGB/UNORM のどちらで読んでも 1.0 なので、フォーマットは
        //   どちらでもよいが、慣習として VK_FORMAT_R8G8B8A8_SRGB を渡す。
        white_texture_ = load_from_pixels(kWhite, 1, 1, VK_FORMAT_R8G8B8A8_SRGB);
        return white_texture_;
    }

    scene::TextureId TextureRegistry::create_flat_normal_texture() {
        if (contains(flat_normal_texture_)) {
            return flat_normal_texture_;
        }
        constexpr unsigned char kFlatNormal[4] = { 128, 128, 255, 255 };

        // フォーマットは必ずUNORM
        flat_normal_texture_ = load_from_pixels(kFlatNormal, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
        return flat_normal_texture_;
    }

    scene::TextureId TextureRegistry::default_texture() const {
        return default_texture_;
    }

    scene::TextureId TextureRegistry::white_texture() const {
        return white_texture_;
    }

    scene::TextureId TextureRegistry::flat_normal_texture() const {
        return flat_normal_texture_;
    }

    VkImageView TextureRegistry::view(scene::TextureId id) const {
        if (contains(id)) {
            return get(id).texture->view();
        }
        throw std::runtime_error("TextureRegistry::view : テクスチャが見つかりませんでした。");
    }

    void TextureRegistry::unload(scene::TextureId id) {
        if (id == default_texture_) {
            spdlog::warn("TextureRegistry::unload : 既定テクスチャは解放できません。");
            return;
        }
        if (id == white_texture_ || id == flat_normal_texture_) {
            spdlog::warn("TextureRegistry::unload : フォールバックテクスチャは解放できません。");
            return;
        }

        destroy(id);
    }

    void TextureRegistry::on_registered(scene::TextureId id, const TextureEntry& entry) {
        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = entry.texture->view();
        image_info.sampler = sampler_;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = bindless_set_;
        write.dstBinding = 0;
        write.dstArrayElement = id.index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    void TextureRegistry::on_destroy(scene::TextureId id, TextureEntry&& entry) {
        VkDescriptorImageInfo info = {
            .sampler = sampler_,
            .imageView = get(default_texture_).texture->view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = bindless_set_, .dstBinding = 0,
            .dstArrayElement = id.index, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &info
        };
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        //   ★ このディスクリプタ更新は遅延しなくてよい（UPDATE_AFTER_BIND_BIT があるため）。
        //     遅延が要るのは VkImage / VkImageView の破棄の方。

        //   実体を遅延解放キューへ移す:
        auto tex = std::shared_ptr(std::move(entry.texture));
        deletions_->push([tex]() mutable { tex.reset(); });

        //   by_path_ から該当エントリを消す（★ 消し忘れると次の load が死んだIDを返す）
        std::erase_if(by_path_, [id] (const auto& pair) {
            return pair.second == id;
        });
    }

}  // namespace sq::graphics
