#include "sq/graphics/texture_registry.hpp"

#include <stdexcept>
#include <spdlog/spdlog.h>

namespace sq::graphics {

TextureRegistry::TextureRegistry(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
                                 std::uint32_t queue_family, VkQueue queue,
                                 VkDescriptorPool descriptor_pool,
                                 VkDescriptorSetLayout material_set_layout,
                                 VkSampler sampler, std::uint32_t max_textures)
    : physical_device_(physical_device), device_(device), allocator_(&allocator),
      queue_family_(queue_family), queue_(queue),
      descriptor_pool_(descriptor_pool), material_set_layout_(material_set_layout), sampler_(sampler),
      max_textures_(max_textures) {
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

    //  2. 上限チェック
    if (entries_.size() >= max_textures_) {
        spdlog::error("TextureRegistry::load : これ以上テクスチャを登録できません。");
        return default_texture_;
    }

    //  3. テクスチャ本体を読み込む:
    Entry entry;
    entry.texture = std::make_unique<Texture>(
        physical_device_, device_, *allocator_, queue_family_, queue_, path);

    //  4. セットへイメージを書き込む（★ set=1 なので dstBinding は 0）:
    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = entry.texture->view();
    image_info.sampler = sampler_;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindless_set_;
    write.dstBinding = 0;
    write.dstArrayElement = static_cast<uint32_t>(entries_.size()); // ★ = これから振る TextureId
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
