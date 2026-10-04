#pragma once

#include <memory>

#include <vulkan/vulkan.h>

#include "sq/graphics/asset_registry.hpp"
#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/material_buffer.hpp"
#include "sq/graphics/material_data.hpp"
#include "sq/graphics/texture_registry.hpp"
#include "sq/scene/material.hpp"

namespace sq::graphics {

// 1件のマテリアルの CPU 側の控え（phase16 ③-3）。
//
// ★ MaterialRegistry::Slot::data から名前空間スコープへ出した（MeshEntry と同じ理由）。
// ★ MaterialData を直接 Entry にせず構造体で包むのは、手順8（③-4）で
//   「このマテリアルが使うテクスチャ5枚の TextureRef」をここへ足すため。
//   そうすると destroy() が entry を空にした瞬間にテクスチャの release が走り、
//   **マテリアル → テクスチャの連鎖解放**が on_destroy に1行も書かずに成立する（D-9）。
struct MaterialEntry {
    MaterialData data{};   // resolve_textures 済みの値（GPU へ書いたものと同じ）
};

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
//
// phase16 ③-3: スロット管理を AssetRegistry へ括り出した。基底へは capacity = max_materials を渡す。
// ここに残るのは「テクスチャの解決」と「SSBO へ書く（on_registered / update）」だけ。
class MaterialRegistry final : public AssetRegistry<scene::MaterialId, MaterialEntry> {
public:
    // asset_set は Renderer が所有する set=1（TextureRegistry::bindless_set() と同じもの）。
    // コンストラクタで自分のバッファを binding=1 として1回だけ書き込む。
    //
    // textures は albedo のフォールバック判定にのみ使う（所有しない）。
    // ★ TextureRegistry より後に生成すること（bindless_set() を受け取るため）。
    MaterialRegistry(GpuAllocator& allocator, VkDevice device,
                     const TextureRegistry& textures,
                     VkDescriptorSet asset_set, std::uint32_t max_materials);
    ~MaterialRegistry() override;

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

    // ★ phase16 ③-3: contains() は基底（AssetRegistry）へ移した。
    //   material_registry.cpp の MaterialRegistry::contains の定義は削除すること。

    // Material::id が無効／未登録のときに使う既定マテリアル（最初に add したもの）。
    [[nodiscard]] scene::MaterialId default_material() const;

    // マテリアルを解放する（phase14 ②-3）。
    //
    // phase16 ③-3: 「既定マテリアルは解放させない」判定だけをここに残し、
    //   残りは基底の destroy(id) に任せる。
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
    // ★ phase16 ③-3: CPU 側の控えは基底の get_mutable(id).data で書き換える。
    void update(scene::MaterialId id, const MaterialData& data, const scene::MaterialTextures& textures);

protected:
    // 登録時のフック（phase16 ③-3）。既存の add() の手順3をそのまま移す:
    //   buffer_->write(id.index, entry.data)
    // ★ add() 側では buffer_->write を呼ばないこと（二重に書くだけで害は無いが、
    //   「GPU へ書く場所」が2箇所になる。phase15 ③-9 バグA と同じ教訓）。
    void on_registered(scene::MaterialId id, const MaterialEntry& entry) override;

    // 解放時のフック（phase16 ③-3）。手順7では**何もしない**（枠を空けるだけ）。
    //   GPU 上の古い MaterialData は残るが、古い ID は contains() で弾かれて
    //   既定マテリアルに落ちるので実害は無い（既存の unload() のコメントと同じ）。
    // ★ 手順8で MaterialEntry に TextureRef が入っても、ここに書き足すことは無い
    //   （基底の destroy() が entry を空にした時点で release が走る）。
    void on_destroy(scene::MaterialId id, MaterialEntry&& entry) override;

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

    // ★ phase16 ③-3: slots_ / free_indices_ は基底（AssetRegistry）へ移した
    //   （phase14 ②-2 で予告していた括り出し）。
    //   スロットの添字 = MaterialId::index = SSBO の添字、という対応は変わらない。

    scene::MaterialId default_material_ = scene::kInvalidMaterialId;
    // ★ phase16 ③-3: max_materials_ は基底の capacity_ へ移した（full() で判定する）。
};

}  // namespace sq::graphics
