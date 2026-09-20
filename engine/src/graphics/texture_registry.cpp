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
    : physical_device_(physical_device), device_(device), allocator_(&allocator),
      queue_family_(queue_family), queue_(queue),
      descriptor_pool_(descriptor_pool), material_set_layout_(material_set_layout), sampler_(sampler),
      deletions_(&deletions), max_textures_(max_textures) {
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
    // slots_ の unique_ptr が Texture（VkImage / VkImageView / VkDeviceMemory）を破棄する。
    // ディスクリプタセットは個別に解放しない（プール破棄でまとめて解放される）。
    // 前提: このデストラクタは VkDevice の破棄より前に走ること（Renderer が破棄順序を担保）。
    //
    // ★ 遅延解放キューに積まれたまま残っている実体はここでは触らない
    //   （Renderer が vkDeviceWaitIdle → flush_all() を先に済ませている前提。phase14 ②）。
    slots_.clear();
    free_indices_.clear();
    by_path_.clear();
}

scene::TextureId TextureRegistry::load(const std::string& path) {
    // （phase12 手順4）:
    //  1. 既にロード済みなら再利用する（同じ画像を何体が参照してもロードは1回）:
    if (auto it = by_path_.find(path); it != by_path_.end()) { return it->second; }

    // スロット確保に置き換える。
    //   1. free_indices_ が空でなければ末尾から再利用、空なら slots_.emplace_back()
    //   2. 上限チェックは slots_.size() >= max_textures_ **かつ free_indices_ が空** のときだけ
    //      （解放済みスロットがあるなら上限に達していても登録できる。ここが
    //        「load/unload を繰り返してもメモリが単調増加しない」検証の肝）
    //   3. dstArrayElement は entries_.size() ではなく 確保した index を使う
    //   4. 返り値は scene::TextureId{ { index, slot.generation } }

    //  2. スロットの上限チェックと確保
    if (slots_.size() >= max_textures_ && free_indices_.empty()) {
        spdlog::error("MaterialRegistry::add: Too many materials");
        return default_texture_;
    }

    std::uint32_t index;
    if (free_indices_.empty()) {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    } else {
        index = free_indices_.back(); // 解放済みスロットを再利用
        free_indices_.pop_back();
    }

    //  3. テクスチャ本体を読み込む:
    auto texture = std::make_unique<Texture>(
        physical_device_, device_, *allocator_, queue_family_, queue_, path);

    //  4. テクスチャの登録
    scene::TextureId id = register_texture(std::move(texture), index);

    //  5. パスの登録
    by_path_.emplace(path, id);

    return id;
}

scene::TextureId TextureRegistry::load_from_pixels(const unsigned char* pixels,
                                                   std::uint32_t width, std::uint32_t height) {
    //   load() と同じ流れ。違うのは
    //     - by_path_ の照会・登録をしない（パスが無い）
    //     - Texture をピクセル列版のコンストラクタで作る
    //   の2点だけ。スロット確保・ディスクリプタ書き込み・ID の払い出しは共通なので、

    //  1. スロットの上限チェックと確保
    if (slots_.size() >= max_textures_ && free_indices_.empty()) {
        spdlog::error("MaterialRegistry::add: Too many materials");
        return default_texture_;
    }

    std::uint32_t index;
    if (free_indices_.empty()) {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    } else {
        index = free_indices_.back(); // 解放済みスロットを再利用
        free_indices_.pop_back();
    }

    //  2. テクスチャ本体を読み込む
    auto texture = std::make_unique<Texture>(
        physical_device_, device_, *allocator_, queue_family_, queue_,
        pixels, width, height);

    //  3. テクスチャの登録してidを返す
    return register_texture(std::move(texture), index);
}

scene::TextureId TextureRegistry::register_texture(std::unique_ptr<Texture> texture, std::uint32_t index) {
    Entry entry;
    entry.texture = std::move(texture);

    //  4. セットへイメージを書き込む（★ set=1 なので dstBinding は 0）:
    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = entry.texture->view();
    image_info.sampler = sampler_;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindless_set_;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    //  5. 登録してIDを返す:
    Slot& slot = slots_[index];
    slot.entry = std::move(entry);
    slot.alive = true;

    scene::TextureId id = {index, slot.generation};
    if (default_texture_ == scene::kInvalidTextureId) { default_texture_ = id; }  // 最初の1枚を既定にする

    return id;
}

VkDescriptorSet TextureRegistry::bindless_set() const {
    return bindless_set_;
}

bool TextureRegistry::contains(scene::TextureId id) const {
    // MeshRegistry::contains と同じ3条件にする。
    return id.index < slots_.size()
        && slots_[id.index].alive
        && slots_[id.index].generation == id.generation;
}

scene::TextureId TextureRegistry::create_white_texture() {
    constexpr unsigned char kWhite[4] = { 255, 255, 255, 255 };
    white_texture_ = load_from_pixels(kWhite, 1, 1);
    //   ★ 二重登録の防止: 既に有効なら何もせず white_texture_ を返す
    //     （contains(white_texture_) で判定できる）。
    //   ★ ファイルを用意しないのは、これが「絵」ではなく「乗算の恒等元」だから。
    //     ディスクに置くとユーザーが差し替えられてしまい、中立性が保証できなくなる。
    return white_texture_;
}

scene::TextureId TextureRegistry::default_texture() const {
    return default_texture_;
}

scene::TextureId TextureRegistry::white_texture() const {
    return white_texture_;
}

void TextureRegistry::unload(scene::TextureId id) {
    //  1. 二重解放の防止
    if (!contains(id)) { return; }

    //   2. ★ 既定テクスチャは unload させない（フォールバック先が消えると全体が壊れる）
    if (id == default_texture_) {
        spdlog::warn("TextureRegistry::unload : 既定テクスチャは解放できません。");
        return;
    }

    //   3. bindless 配列の該当要素を既定テクスチャの view で上書きする（★ 順序に注意。
    //      実体を手放す**前**に、差し替え用の view を取っておくこと）:
    VkDescriptorImageInfo info = {
        .sampler = sampler_,
        .imageView = slots_[default_texture_.index].entry.texture->view(),
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bindless_set_, .dstBinding = 0,
        .dstArrayElement = id.index, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &info
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    //      ★ このディスクリプタ更新は遅延しなくてよい（UPDATE_AFTER_BIND_BIT があるため）。
    //        遅延が要るのは VkImage / VkImageView の破棄の方。

    //   4. 実体を遅延解放キューへ移す:
    auto tex = std::shared_ptr<Texture>(std::move(slots_[id.index].entry.texture));
    deletions_->push([tex]() mutable { tex.reset(); });

    //   5. by_path_ から該当エントリを消す（★ 消し忘れると次の load が死んだIDを返す）
    std::erase_if(by_path_, [id] (const auto& pair) {
        return pair.second == id;
    });

    //   6. スロットと再利用リストの更新
    Slot& slot = slots_[id.index];
    slot.alive = false;
    ++slot.generation;
    free_indices_.push_back(id.index);
}

}  // namespace sq::graphics
