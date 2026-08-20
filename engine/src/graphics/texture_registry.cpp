#include "sq/graphics/texture_registry.hpp"

#include <stdexcept>
#include <spdlog/spdlog.h>

namespace sq::graphics {

TextureRegistry::TextureRegistry(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
                                 std::uint32_t queue_family, VkQueue queue,
                                 VkDescriptorPool descriptor_pool,
                                 VkDescriptorSetLayout material_set_layout,
                                 VkSampler sampler)
    : physical_device_(physical_device), device_(device), allocator_(&allocator),
      queue_family_(queue_family), queue_(queue),
      descriptor_pool_(descriptor_pool), material_set_layout_(material_set_layout), sampler_(sampler) {
    // 生成時は空。load() で登録していく。

    // TODO(phase13 ①-3): 共有の bindless セット（set=1）をここで1つだけ確保する。
    //   VkDescriptorSetAllocateInfo alloc_info{};
    //   alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    //   alloc_info.descriptorPool     = descriptor_pool_;
    //   alloc_info.descriptorSetCount = 1;
    //   alloc_info.pSetLayouts        = &material_set_layout_;   // 配列化した set=1 のレイアウト
    //   if (vkAllocateDescriptorSets(device_, &alloc_info, &bindless_set_) != VK_SUCCESS) { throw ...; }
    //
    // ★ この時点では配列の要素が1つも書かれていないが、レイアウトに
    //   PARTIALLY_BOUND を付けてあるので確保・バインドできる。
}

TextureRegistry::~TextureRegistry() {
    // entries_ の unique_ptr が Texture（VkImage / VkImageView / VkDeviceMemory）を破棄する。
    // ディスクリプタセットは個別に解放しない（プール破棄でまとめて解放される）。
    // 前提: このデストラクタは VkDevice の破棄より前に走ること（Renderer が破棄順序を担保）。
    entries_.clear();
    by_path_.clear();
}

scene::TextureId TextureRegistry::load(const std::string& path) {
    // （phase12 手順4）:
    //  1. 既にロード済みなら再利用する（同じ画像を何体が参照してもロードは1回）:
    if (auto it = by_path_.find(path); it != by_path_.end()) { return it->second; }

    //  2. テクスチャ本体を読み込む:
    Entry entry;
    entry.texture = std::make_unique<Texture>(
        physical_device_, device_, *allocator_, queue_family_, queue_, path);

    // TODO(phase13 ①-3): 手順3（セットの確保）を丸ごと削除する。
    //   bindless ではセットは共有の1つだけなので、テクスチャごとの確保は不要になる。
    //   代わりに、上限チェックを自前で行う（プール枯渇では検出できなくなるため）:
    //     if (entries_.size() >= kMaxTextures) {
    //         spdlog::error("TextureRegistry::load : これ以上テクスチャを登録できません。");
    //         return default_texture_;
    //     }
    //   ★ kMaxTextures は現在 Renderer の private 定数なので、TextureRegistry へ移すか
    //     コンストラクタ引数で受け取る形にする（レイアウトの配列長と必ず一致させること）。

    //  3. このテクスチャ専用のディスクリプタセット（set=1）を1つ確保する:
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &material_set_layout_;
    if (VkResult result = vkAllocateDescriptorSets(device_, &alloc_info, &entry.set)
        ; result != VK_SUCCESS) {
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY) {
            // プール枯渇時 = Renderer::kMaxTextures 超過
            spdlog::error("Textureregistry::load : これ以上テクスチャを登録できません。");
            return default_texture_;
        } else {
            throw std::runtime_error("Textureregistry::load : ディスクリプタセットの確保に失敗しました。");
        }
    }

    // TODO(phase13 ①-3): 書き込み先を「共有セットの配列要素」に変える。
    //   変更点は2つだけ:
    //     write.dstSet          = bindless_set_;                       // 共有の1セット
    //     write.dstArrayElement = static_cast<std::uint32_t>(entries_.size());  // ★ = これから振る TextureId
    //   dstBinding = 0 / descriptorCount = 1 はそのまま（「配列の id 番目に1枚書く」の意味になる）。
    //
    // ★ dstArrayElement は「登録後の id」と一致させること。ここでは push_back 前なので
    //   entries_.size() がそのまま次の id になる。ずれると別のテクスチャが表示される。
    // ★ UPDATE_AFTER_BIND を付けてあるので、既にバインド済みのセットへ後から書いてよい。
    //   ただし「GPU が読んでいる最中の要素」を書き換えるのは不可。起動時のみ登録する現状は安全。

    //  4. セットへイメージを書き込む（★ set=1 なので dstBinding は 0）:
    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = entry.texture->view();
    image_info.sampler = sampler_;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = entry.set;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    //  5. 登録してIDを返す:
    entries_.push_back(std::move(entry));
    const auto id = static_cast<scene::TextureId>(entries_.size() - 1);
    by_path_.emplace(path, id);
    if (default_texture_ == scene::kInvalidTextureId) { default_texture_ = id; }  // 最初の1枚を既定にする

    return id;
}

VkDescriptorSet TextureRegistry::descriptor_set(scene::TextureId id) const {
    if (contains(id)) {
        return entries_[id].set;
    }

    throw std::runtime_error("TextureRegistry::descriptor_set : テクスチャが登録されていません。");
}

VkDescriptorSet TextureRegistry::bindless_set() const {
    return bindless_set_;
}

bool TextureRegistry::contains(scene::TextureId id) const {
    return id < entries_.size();
}

scene::TextureId TextureRegistry::default_texture() const {
    return default_texture_;
}

}  // namespace sq::graphics
