#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/deletion_queue.hpp"
#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/texture.hpp"
#include "sq/scene/material.hpp"

namespace sq::graphics {

// TextureId → GPU実体（共有配列の添え字）のキャッシュ（phase12 D-3）。
// 同じ画像を何体のエンティティが参照してもロードは1回で済む（パスで重複を排除する）。
//
// テクスチャは起動時に確定し以後ほぼ不変なので、ディスクリプタセット（set=1）は
// フレーム数とは無関係にテクスチャごと1個で足りる（phase12 D-5）。
//
// phase14 ②: スロット + 世代番号の管理に変えた（D-5）。TextureId は { index, generation } で、
// **bindless 配列の添字になるのは index だけ**（generation は CPU 側の検証用）。
class TextureRegistry {
public:
    // descriptor_pool / material_set_layout / sampler は Renderer が所有し、ここでは破棄しない。
    // material_set_layout は set=1 用のレイアウト
    // （binding=0: COMBINED_IMAGE_SAMPLER 配列 / binding=1: マテリアル SSBO。phase14 ①）。
    // deletions は Renderer が所有する遅延解放キュー（phase14 ②。所有しない）。
    TextureRegistry(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
                    std::uint32_t queue_family, VkQueue queue,
                    VkDescriptorPool descriptor_pool,
                    VkDescriptorSetLayout material_set_layout,
                    VkSampler sampler, std::uint32_t max_textures,
                    DeletionQueue& deletions);
    ~TextureRegistry();

    TextureRegistry(const TextureRegistry&) = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    // load/load_from_pixelsの共通部分
    scene::TextureId register_texture(std::unique_ptr<Texture> texture, std::uint32_t index);

    // path の画像を読み込んで登録し TextureId を返す。
    // 同じ path が既に登録済みならロードせず既存のIDを返す（by_path_ で判定）。
    scene::TextureId load(const std::string& path);

    // デコード済みピクセル列から登録する（phase14 ③-4。glTF の埋め込み画像用）。
    //
    // ★ by_path_ による重複排除は効かない（パスが無いため）。
    //   同じ画像を2回渡せば2回登録される。glTF 側は model.images を1回だけ走査するので、
    //   1ファイル内では重複しない。複数ファイルが同じ画像を埋め込んでいる場合は素通りする。
    //   重複排除したいならピクセル列のハッシュをキーにするしかないが、このフェーズではやらない。
    scene::TextureId load_from_pixels(const unsigned char* pixels,
                                      std::uint32_t width, std::uint32_t height);

    // 全テクスチャ共有の bindless ディスクリプタセット（set=1）。phase13 ①-3。
    // binding=0 が sampler2D の配列で、**TextureId::index がそのまま配列の添字**になる。
    // phase14 ①: binding=1 に MaterialRegistry の SSBO が同居する（同じセットを共有する）。
    // 内容は起動後に増えるだけ（既存要素は書き換わらない）なので、
    // フレーム先頭で1回バインドすれば以後ドロー中に触る必要がない。
    [[nodiscard]] VkDescriptorSet bindless_set() const;

    // ★ phase14 ②: 添字の範囲だけでなく generation の一致も見る。
    [[nodiscard]] bool contains(scene::TextureId id) const;

    // 1×1 の白テクスチャを生成して登録する（phase14 ③）。起動時に1回だけ呼ぶ。
    //
    // ★ default_texture() とは**役割が違う**。1つのフォールバックで兼ねてはいけない:
    //     white_texture()   … 「このマテリアルはテクスチャを持たない」。中立であることが仕事。
    //                          白を乗算しても base_color がそのまま出るので、glTF 仕様どおり
    //                          baseColorFactor だけの見た目になる
    //     default_texture() … 「読み込み・変換に失敗した」。目立つことが仕事（市松模様）
    //   兼用すると、テクスチャ無しのマテリアルに市松模様が掛かって仕様と食い違い、
    //   さらに「テクスチャ無し」と「読み込み失敗」が見分けられなくなる。
    //
    // ★ 呼ぶ順序に注意。default_texture_ は「最初に登録されたもの」で決まるので、
    //   既定テクスチャ（textures/default.png）を load した**後**に呼ぶこと。
    //   先に呼ぶと白が既定テクスチャになり、失敗時に何も気付けなくなる。
    scene::TextureId create_white_texture();

    // マテリアルの albedo_index が無効／未登録のときに使う既定テクスチャ（最初に load したもの）。
    // ★ 「異常を知らせる」用。テクスチャを持たないマテリアルには white_texture() を使うこと。
    [[nodiscard]] scene::TextureId default_texture() const;

    // テクスチャを持たないマテリアル用の中立テクスチャ（create_white_texture() で登録したもの）。
    // ★ create_white_texture() を呼ぶ前は無効ハンドルが返る。
    [[nodiscard]] scene::TextureId white_texture() const;

    // テクスチャを解放する（phase14 ②-3）。
    //
    // MeshRegistry::unload の手順に加えて、bindless 配列の後始末が要る:
    //   5. 該当要素を**既定テクスチャの view で上書きする**
    //      ★ 「空にする」ではなく「有効な view で埋め直す」こと。
    //        PARTIALLY_BOUND があっても、破棄済みの VkImageView を指したままの要素を
    //        シェーダが引けば未定義動作（PARTIALLY_BOUND が許すのは「一度も書いていない要素」だけで、
    //        「書いたが中身が死んでいる要素」は救ってくれない）。
    //   ★ このディスクリプタ更新は unload の時点で行ってよい（遅延不要）。
    //     ディスクリプタの書き換え自体は UPDATE_AFTER_BIND_BIT のおかげで安全で、
    //     遅延が要るのは VkImage / VkImageView の破棄の方。
    void unload(scene::TextureId id);

private:
    struct Entry {
        std::unique_ptr<Texture> texture;
    };

    // 1スロット = 「1つのテクスチャを置ける枠」。bindless 配列の1要素と1対1に対応する。
    struct Slot {
        Entry entry;
        std::uint32_t generation = 0;
        bool alive = false;
    };

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;  // 所有しない（Device が所有）
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;        // 所有しない（Renderer が所有）
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;  // 所有しない（Renderer が所有）
    VkSampler sampler_ = VK_NULL_HANDLE;                       // 所有しない（Renderer が所有）

    // 全テクスチャ共有の set=1（phase13 ①-3）。コンストラクタで1つだけ確保する。
    // プールから確保するので個別破棄は不要（プール破棄でまとめて解放される）。
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;

    DeletionQueue* deletions_ = nullptr;  // 所有しない（Renderer が所有）

    std::vector<Slot> slots_;                  // 添字が TextureId::index（= bindless 配列の添字）
    std::vector<std::uint32_t> free_indices_;  // 解放済みスロットの再利用リスト

    // 同一パスの再ロード防止。
    // ★ phase14 ②: unload したテクスチャのエントリを**ここからも消す**こと。
    //   消し忘れると、次の load が「登録済み」と誤判定して死んだハンドルを返す。
    std::unordered_map<std::string, scene::TextureId> by_path_;

    scene::TextureId default_texture_ = scene::kInvalidTextureId;
    scene::TextureId white_texture_ = scene::kInvalidTextureId;   // phase14 ③（中立フォールバック）
    std::uint32_t max_textures_ = 0;  // 登録できるテクスチャ数の上限（プールの容量）
};

}  // namespace sq::graphics
