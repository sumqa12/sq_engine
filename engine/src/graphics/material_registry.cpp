#include "sq/graphics/material_registry.hpp"
#include <spdlog/spdlog.h>

namespace sq::graphics {

MaterialRegistry::MaterialRegistry(GpuAllocator& allocator, VkDevice device,
                                   const TextureRegistry& textures,
                                   VkDescriptorSet asset_set, std::uint32_t max_materials)
    : device_(device), textures_(&textures), asset_set_(asset_set), max_materials_(max_materials) {

    buffer_ = std::make_unique<MaterialBuffer>(allocator, device_, max_materials);

    // asset_set_ の binding=1 へこのバッファを書き込む（起動時に1回だけ）
    VkDescriptorBufferInfo buffer_info = {
        .buffer = buffer_->handle(),
        .offset = 0,
        .range = VK_WHOLE_SIZE
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = asset_set_,
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    // ★ TextureRegistry と違い、ここは**配列ではない**ので dstArrayElement は常に 0。
    // ★ バッファは中身が空のまま書き込んでよい（以後 add() でメモリを直接書くだけで、
    //   ディスクリプタを更新し直す必要はない。テクスチャとの一番の違い）。
}

MaterialRegistry::~MaterialRegistry() {
    // buffer_ の unique_ptr が MaterialBuffer（VkBuffer + Allocation）を破棄する。
    // ディスクリプタセットは個別に解放しない（プール破棄でまとめて解放される）。
    // 前提: このデストラクタは VkDevice の破棄より前に走ること（Renderer が破棄順序を担保）。
    slots_.clear();
    free_indices_.clear();
    buffer_.reset();
}

scene::MaterialId MaterialRegistry::add(const MaterialData& data, const scene::TextureId albedo) {
    //   1. スロットを確保する（phase14 ②-2。3レジストリ共通の手順）:
    //        free_indices_ が空でなければ末尾から再利用、空なら slots_.emplace_back()
    //        上限チェック: slots_.size() >= max_materials_ かつ free_indices_ が空なら
    //        spdlog::error を出して default_material_ を返す。

    //   1. スロットの上限チェックと確保
    if (slots_.size() >= max_materials_ && free_indices_.empty()) {
        spdlog::error("MaterialRegistry::add: Too many materials");
        return default_material_;
    }

    std::uint32_t index;
    if (free_indices_.empty()) {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    } else {
        index = free_indices_.back(); // 解放済みスロットを再利用
        free_indices_.pop_back();
    }

    //   2. albedo のフォールバック（phase13 まで収集フェーズにあった判定の移設先）:
    MaterialData resolved = data;
    //  ★ albedo_index は生の uint32 なので、TextureId へ包み直して照合する必要がある。
    //   ここが「ハンドルを構造体にした」ことの副作用で、一番読み替えを忘れやすい箇所:
    //    MaterialData は GPU レイアウトなので generation を持てず、index しか入らない。
    //    → add() の引数を MaterialData + scene::TextureId albedo の2本立てにして、
    //      世代照合を済ませてから resolved.albedo_index = albedo.index を入れる形が素直。
    //      （MaterialData をそのまま受ける現在の形のままだと、
    //        「index だけで contains できない」ので照合が成立しない）
    if (textures_->contains(albedo)) {
        resolved.albedo_index = albedo.index;
    } else {
        resolved.albedo_index = textures_->default_texture().index;
    }


    //   3. GPU へ書く
    buffer_->write(index, resolved);

    //   4. CPU側の控え
    Slot& slot = slots_[index];
    slot.data = resolved;
    slot.alive = true;

    //   5. ID を返す
    auto id = scene::MaterialId{ { index, slot.generation } };

    //   6. 最初の1件を既定マテリアルにする（TextureRegistry::default_texture と同じ流儀）
    if (default_material_.is_null()) { default_material_ = id; }
    return id;
}

bool MaterialRegistry::contains(scene::MaterialId id) const {
    // MeshRegistry::contains と同じ3条件にする。
    return id.index < slots_.size()
        && slots_[id.index].alive
        && slots_[id.index].generation == id.generation;
}

void MaterialRegistry::unload(scene::MaterialId id) {
    //  1. 二重解放の防止
    if (!contains(id)) { return; }

    //   2. ★ 既定マテリアルは unload させない
    if (id == default_material_) {
        spdlog::warn("MaterialRegistry::unload : 既定マテリアルは解放できません。");
        return;
    }

    slots_[id.index].alive = false;
    ++slots_[id.index].generation;
    free_indices_.push_back(id.index);
    // ★ 遅延解放は不要（GPUリソースではなく配列の枠を返すだけ）。
    //   ただし GPU 上の古い MaterialData は残る。まだ古いIDを握った体が居ても
    //   contains() で弾かれて既定マテリアルに落ちるので、実害は無い。
}

scene::MaterialId MaterialRegistry::default_material() const {
    return default_material_;
}

void MaterialRegistry::update(scene::MaterialId id, const MaterialData& data) {
    // contains(id) でなければ何もしない
    // ★ GPU が読んでいる最中に書き換えると壊れる。このフェーズでは起動時専用。
    if (contains(id)) {
        slots_[id.index].data = data;
        buffer_->write(id.index, data);
    }
}

}  // namespace sq::graphics
