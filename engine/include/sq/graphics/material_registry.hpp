#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/material_buffer.hpp"
#include "sq/graphics/material_data.hpp"
#include "sq/graphics/texture_registry.hpp"
#include "sq/scene/material.hpp"

namespace sq::graphics {

// MaterialId → GPU側マテリアル（SSBO の配列要素）のレジストリ（phase14 ① / D-1）。
//
// TextureRegistry と同じ形（添字がID・起動時に登録・以後不変）。違うのは
// 「ディスクリプタの配列要素ではなく、1本の SSBO の配列要素を書く」点だけ。
//
// 置き場は set=1（アセット用のセット。D-2）の binding=1。
// set=0 と違ってフレームごとに変わらないので、バッファもセットも**1つで足りる**。
//
// ★ set=1 のレイアウトには UPDATE_AFTER_BIND_POOL_BIT が付いているが、
//   binding=1 に UPDATE_AFTER_BIND_BIT を**付けなければ**通常のバインディングとして扱われ、
//   追加の機能フラグ（descriptorBindingStorageBufferUpdateAfterBind）は要らない。
class MaterialRegistry {
public:
    // asset_set は Renderer が所有する set=1（TextureRegistry::bindless_set() と同じもの）。
    // コンストラクタで自分のバッファを binding=1 として1回だけ書き込む。
    //
    // textures は albedo のフォールバック判定にのみ使う（所有しない）。
    // ★ TextureRegistry より後に生成すること（bindless_set() を受け取るため）。
    MaterialRegistry(GpuAllocator& allocator, VkDevice device,
                     const TextureRegistry& textures,
                     VkDescriptorSet asset_set, std::uint32_t max_materials);
    ~MaterialRegistry();

    MaterialRegistry(const MaterialRegistry&) = delete;
    MaterialRegistry& operator=(const MaterialRegistry&) = delete;

    // マテリアルを登録し MaterialId を返す。
    //
    // ★ textures の各スロットが無効／未登録のテクスチャを指していれば、resolve_textures
    //   （phase15 ③-3）が用途ごとの中立テクスチャへ解決する。用途ごとにフォールバック先が
    //   違う点に注意（scene::MaterialTextures のコメント参照）:
    //     albedo             → textures_->white_texture()
    //     normal             → textures_->flat_normal_texture()
    //     metallic_roughness → textures_->white_texture()
    //     occlusion          → textures_->white_texture()
    //     emissive           → textures_->white_texture()
    //
    // ★ 重複排除はしない（phase14 ① で決定）。同じ内容の MaterialData を2回渡せば
    //   2件登録される。TextureRegistry の by_path_ 相当を入れないのは、
    //   キーが「パス1本」ではなく MaterialData 全体になり、operator== とハッシュの
    //   定義が要るわりに、1件64バイトなので重複してもメモリ的にはほぼ無害なため。
    //   ★ glTF（③）は model.materials を1回だけ走査するので、1ファイル内では重複しない。
    scene::MaterialId add(const MaterialData& data, const scene::MaterialTextures& textures);

    [[nodiscard]] bool contains(scene::MaterialId id) const;

    // Material::id が無効／未登録のときに使う既定マテリアル（最初に add したもの）。
    [[nodiscard]] scene::MaterialId default_material() const;

    // マテリアルを解放する（phase14 ②-3）。
    //
    // ★ メッシュ・テクスチャと違い、**遅延解放は不要**。
    //   解放するものが GPU リソース（VkImage / VkBuffer）ではなく「配列の1枠」だけで、
    //   バッファ本体はレジストリの寿命と一致しているため。
    //   ただしスロットの再利用で古い MaterialData が残るので、
    //   再利用時に上書きされるまで**古い見た目のまま描かれる**点には注意
    //   （気になるなら unload 時に MaterialData{} で潰しておく）。
    void unload(scene::MaterialId id);

    // 登録済みの内容を1件だけ書き換える（デバッグ・エディタ用途）。
    // ★ GPU が読んでいる最中に書き換えると壊れる。呼ぶ側が vkDeviceWaitIdle するか、
    //   フレーム複製する必要がある。このフェーズでは「起動時のみ」の前提で使うこと。
    //
    // ★ phase15 ③-3: add と同じ resolve_textures を通すこと。update だけ生の
    //   MaterialData を受けて素通しにすると、add で効いていたフォールバックが
    //   update 経由の書き換えでは効かない、という片手落ちが起きる。
    void update(scene::MaterialId id, const MaterialData& data, const scene::MaterialTextures& textures);

private:
    // add / update が共有するテクスチャ解決処理（phase15 ③-3）。
    // data の各 *_index を textures の対応スロットで上書きした MaterialData を返す。
    //
    // ★ 括り出す理由: phase14 で Texture::create_from_pixels を括り出したのと同じで、
    //   ここを1本にしておかないと「add だけ直して update を直し忘れる」事故が起きる。
    [[nodiscard]] MaterialData resolve_textures(const MaterialData& data,
                                                const scene::MaterialTextures& textures) const;

    VkDevice device_ = VK_NULL_HANDLE;
    const TextureRegistry* textures_ = nullptr;  // 所有しない（Renderer が所有）
    VkDescriptorSet asset_set_ = VK_NULL_HANDLE; // 所有しない（プール破棄でまとめて解放される）

    // 永続マップの SSBO を1本だけ持つ（フレーム複製は不要。D-2）。
    std::unique_ptr<MaterialBuffer> buffer_;

    // 1スロット = 「1件のマテリアルを置ける枠」。SSBO の1要素と1対1に対応する。
    // ★ MeshRegistry / TextureRegistry と同じ形（phase14 ②-2）。
    //   3つとも書いてみてから共通項（テンプレート基底 AssetRegistry<T>）を抜くと、
    //   「何を共通化すべきか」が見えて学習になる。
    struct Slot {
        MaterialData data{};
        std::uint32_t generation = 0;
        bool alive = false;
    };

    std::vector<Slot> slots_;                  // 添字が MaterialId::index（= SSBO の添字）
    std::vector<std::uint32_t> free_indices_;  // 解放済みスロットの再利用リスト

    scene::MaterialId default_material_ = scene::kInvalidMaterialId;
    std::uint32_t max_materials_ = 0;    // 登録できるマテリアル数の上限（バッファの容量）
};

}  // namespace sq::graphics
