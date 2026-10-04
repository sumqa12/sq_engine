#include "sq/graphics/material_registry.hpp"
#include <spdlog/spdlog.h>

namespace sq::graphics {

// TODO(phase16 手順7 / ③-3): AssetRegistry への載せ替え
//   1. コンストラクタの初期化子リストの先頭に基底を足す: AssetRegistry(max_materials)
//      max_materials_(max_materials) は消す（基底の capacity_ へ移した）。
//      ★ MaterialBuffer の要素数には引き続き max_materials（引数）を使う。
//   2. デストラクタの slots_.clear() / free_indices_.clear() を消す（buffer_.reset() は残す）。
//   3. add():
//        「上限チェック + スロット確保」の塊を if (full()) { spdlog::error(...); return default_material_; }
//        だけにする。
//        resolve_textures → emplace(MaterialEntry{ resolved }) → default_material_ の設定、の順。
//        buffer_->write は on_registered() へ移す。
//   4. contains の定義を消す（基底へ移した）。
//   5. unload(): 既定マテリアルの判定だけ残して destroy(id) を呼ぶ。
//   6. update(): slots_[id.index].data = resolved を get_mutable(id).data = resolved に置き換える。
//   7. on_registered / on_destroy を定義する（on_destroy は空でよい）。

MaterialRegistry::MaterialRegistry(GpuAllocator& allocator, VkDevice device,
                                   const TextureRegistry& textures,
                                   VkDescriptorSet asset_set, std::uint32_t max_materials)
    : AssetRegistry(max_materials), device_(device), textures_(&textures), asset_set_(asset_set) {

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
    buffer_.reset();
}

scene::MaterialId MaterialRegistry::add(const MaterialData& data, const scene::MaterialTextures& textures) {
    //   1. スロットの上限チェック
    if (full()) {
        spdlog::error("MaterialRegistry::add: 登録件数の上限に達しています。");
        return default_material_;
    }

    //   2. 5スロットぶんのフォールバック解決（phase15 ③-3）:
    MaterialData resolved = resolve_textures(data, textures);

    //   3. レジストリに登録
    auto id = emplace(MaterialEntry{ resolved });

    //   4. 最初の1件を既定マテリアルにする（TextureRegistry::default_texture と同じ流儀）
    if (default_material_.is_null()) { default_material_ = id; }
    return id;
}

void MaterialRegistry::unload(scene::MaterialId id) {
    //   ★ 既定マテリアルは unload させない
    if (id == default_material_) {
        spdlog::warn("MaterialRegistry::unload : 既定マテリアルは解放できません。");
        return;
    }

    destroy(id);
}

scene::MaterialId MaterialRegistry::default_material() const {
    return default_material_;
}

void MaterialRegistry::update(scene::MaterialId id, const MaterialData& data,
                              const scene::MaterialTextures& textures) {
    // contains(id) でなければ何もしない
    // ★ GPU が読んでいる最中に書き換えると壊れる。このフェーズでは起動時専用。
    if (contains(id)) {
        const MaterialData resolved = resolve_textures(data, textures);
        get_mutable(id).data = resolved;
        buffer_->write(id.index, resolved);
    }
}

//   各スロットについて:
//     1. textures_->contains(textures.<slot>) なら .index を使う
//     2. そうでなければ上記の中立テクスチャの .index を使う
//   ★ 「無効なハンドル」と「未登録のハンドル」はどちらも textures_->default_texture()
//     （市松模様）にしないこと。テクスチャを持たないのは異常ではない（phase14 ③ の方針）。
MaterialData MaterialRegistry::resolve_textures(const MaterialData& data,
                                                const scene::MaterialTextures& textures) const {
    MaterialData result = {
        .base_color = data.base_color,
        .emissive = data.emissive,
        .metallic = data.metallic,
        .roughness = data.roughness,
        .alpha_cutoff = data.alpha_cutoff
    };

    result.albedo_index = textures_->contains(textures.albedo)
        ? textures.albedo.index
        : textures_->white_texture().index;

    result.normal_index = textures_->contains(textures.normal)
        ? textures.normal.index
        : textures_->flat_normal_texture().index;

    result.metallic_roughness_index = textures_->contains(textures.metallic_roughness)
        ? textures.metallic_roughness.index
        : textures_->white_texture().index;

    result.occlusion_index = textures_->contains(textures.occlusion)
        ? textures.occlusion.index
        : textures_->white_texture().index;

    result.emissive_index = textures_->contains(textures.emissive)
        ? textures.emissive.index
        : textures_->white_texture().index;

    return result;
}

}  // namespace sq::graphics
