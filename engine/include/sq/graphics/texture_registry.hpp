#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include <vulkan/vulkan.h>

#include "sq/graphics/asset_ref.hpp"
#include "sq/graphics/asset_registry.hpp"
#include "sq/graphics/deletion_queue.hpp"
#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/texture.hpp"
#include "sq/scene/material.hpp"

namespace sq::graphics {

// 1枚のテクスチャの実体（phase16 ③-3）。
//
// ★ TextureRegistry::Entry から名前空間スコープへ出した（MeshEntry と同じ理由）。
// ★ unique_ptr を1本持つだけの構造体だが、型を挟んでおくと手順8以降で
//   付帯情報（by_path_ の逆引きキーなど）を足すときに基底の型引数を変えずに済む。
struct TextureEntry {
    std::unique_ptr<Texture> texture;
};

// TextureId → GPU実体（共有配列の添え字）のキャッシュ（phase12 D-3）。
// 同じ画像を何体のエンティティが参照してもロードは1回で済む（パスで重複を排除する）。
//
// テクスチャは起動時に確定し以後ほぼ不変なので、ディスクリプタセット（set=1）は
// フレーム数とは無関係にテクスチャごと1個で足りる（phase12 D-5）。
//
// phase14 ②: スロット + 世代番号の管理に変えた（D-5）。TextureId は { index, generation } で、
// **bindless 配列の添字になるのは index だけ**（generation は CPU 側の検証用）。
//
// phase16 ③-3: スロット管理を AssetRegistry へ括り出した。基底へは capacity = max_textures を渡す。
// ここに残るのは「画像を読む」「bindless 配列へ書く（on_registered）」
// 「bindless を埋め直して by_path_ から消し、遅延キューへ積む（on_destroy）」の3つ。
class TextureRegistry final : public AssetRegistry<scene::TextureId, TextureEntry> {
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
    ~TextureRegistry() override = default;

    TextureRegistry(const TextureRegistry&) = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    // path の画像を読み込んで登録し TextureId を返す。
    // 同じ path が既に登録済みならロードせず既存のIDを返す（by_path_ で判定）。
    //
    // format: 用途で決める（phase15 ③-1 / D-6。Texture コンストラクタへそのまま渡す）。
    // ★ 同じ path でも format が異なれば**別のテクスチャとして登録する**こと
    //   （by_path_ のキーを {path, format} にした理由。baseColor と occlusion が
    //     同じ画像ファイルを指すモデルが実在する）。
    // ★ phase16 手順8: キャッシュヒットでも**retain しない**（生の ID を返すだけ）。
    //   呼び出し側が TextureRef に包んだ時点で +1 されるので、同じテクスチャを2回 load して
    //   2回包めば ref_count は 2 になる。ここで retain すると二重に数える（③-4）。
    scene::TextureId load(const std::string& path, VkFormat format);

    // デコード済みピクセル列から登録する（phase14 ③-4。glTF の埋め込み画像用）。
    //
    // ★ by_path_ による重複排除は効かない（パスが無いため）。
    //   同じ画像を2回渡せば2回登録される。glTF 側は model.images を1回だけ走査するので、
    //   1ファイル内では重複しない。複数ファイルが同じ画像を埋め込んでいる場合は素通りする。
    //   重複排除したいならピクセル列のハッシュをキーにするしかないが、このフェーズではやらない。
    //
    // format: load() と同じ意味（phase15 ③-1 / D-6）。
    scene::TextureId load_from_pixels(const unsigned char* pixels,
                                      std::uint32_t width, std::uint32_t height,
                                      VkFormat format);

    // 全テクスチャ共有の bindless ディスクリプタセット（set=1）。phase13 ①-3。
    // binding=0 が sampler2D の配列で、**TextureId::index がそのまま配列の添字**になる。
    // phase14 ①: binding=1 に MaterialRegistry の SSBO が同居する（同じセットを共有する）。
    // 内容は起動後に増えるだけ（既存要素は書き換わらない）なので、
    // フレーム先頭で1回バインドすれば以後ドロー中に触る必要がない。
    [[nodiscard]] VkDescriptorSet bindless_set() const;

    // ★ phase16 ③-3: contains() は基底（AssetRegistry）へ移した。

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

    // 1×1 の「真上を向いた法線」(128, 128, 255, 255) を UNORM で生成して登録する
    // （phase15 ③-2）。normal_index を持たないマテリアル用の中立フォールバック。
    //
    // ★ create_white_texture() と同型だが、フォーマットは**必ず UNORM**にすること。
    //   128 を sRGB で読むと約 0.22 になり「真上を向いた法線」のつもりが
    //   斜めを向いた法線として解釈される（絵は出るが陰影がわずかにおかしいだけの、
    //   一番気付きにくい壊れ方）。
    scene::TextureId create_flat_normal_texture();

    // マテリアルの albedo_index が無効／未登録のときに使う既定テクスチャ（最初に load したもの）。
    // ★ 「異常を知らせる」用。テクスチャを持たないマテリアルには white_texture() を使うこと。
    [[nodiscard]] scene::TextureId default_texture() const;

    // テクスチャを持たないマテリアル用の中立テクスチャ（create_white_texture() で登録したもの）。
    // ★ create_white_texture() を呼ぶ前は無効ハンドルが返る。
    [[nodiscard]] scene::TextureId white_texture() const;

    // 法線マップを持たないマテリアル用の中立テクスチャ（phase15 ③-2）。
    // ★ create_flat_normal_texture() を呼ぶ前は無効ハンドルが返る。
    [[nodiscard]] scene::TextureId flat_normal_texture() const;

    // 登録済みテクスチャの VkImageView を返す（phase16 ①-6）。
    //
    // bindless 配列の外（set=2）へ書くときに使う。①の時点では、まだ実体の無い
    // set=2 binding=1..3（IBL）を白テクスチャで埋めるためにだけ呼ぶ。
    // ★ contains(id) でなければ例外（死んだ view を返すとディスクリプタに書かれて未定義動作）。
    // ★ 返した view の寿命はレジストリが握っている。unload すれば死ぬので、
    //   既定アセット（white / default / flat_normal）以外には使わないこと。
    [[nodiscard]] VkImageView view(scene::TextureId id) const;

    // テクスチャを解放する（phase14 ②-3）。
    //
    // phase16 ③-3: 「既定テクスチャは解放させない」判定だけをここに残し、
    //   残りは基底の destroy(id) に任せる。以下の後始末は on_destroy() へ移る。
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

protected:
    // 登録時のフック（phase16 ③-3）。既存の register_texture の手順4をそのまま移す:
    //   bindless_set_ の binding=0, dstArrayElement = id.index へ entry.texture->view() を書く。
    void on_registered(scene::TextureId id, const TextureEntry& entry) override;

    // 解放時のフック（phase16 ③-3）。既存の unload() の手順3〜5をそのまま移す:
    //   1. bindless の id.index 番目を既定テクスチャの view で上書きする
    //      （get(default_texture_).texture->view()。この時点では id も既定もまだ生きている）
    //   2. entry.texture を shared_ptr へ載せ替えて deletions_->push(...)
    //   3. by_path_ から値が id のエントリを消す
    //      ★ 消し忘れると次の load が死んだハンドルを返す。
    //        手順8でここが一番効いてくる（参照カウントで自動的に解放されるようになると、
    //        「いつ消えたか」を呼び出し側が意識しなくなるため）。
    void on_destroy(scene::TextureId id, TextureEntry&& entry) override;

private:
    // load / load_from_pixels の共通部分（phase16 ③-3 で引数から index を外した）。
    //   1. emplace(TextureEntry{ std::move(texture) }) でスロットへ収める
    //      （bindless への書き込みは emplace の中から on_registered が呼ばれて行われる）
    //   2. default_texture_ が未設定なら、払い出した id を既定にする（最初の1枚を既定にする）
    //
    // ★ 上限チェック（full()）は**呼び出し側で Texture を作る前に**済ませておくこと。
    //   ここへ来た時点で満杯なら emplace が例外を投げる。
    // ★ 旧版は public だったが、外から呼ばれていないので private へ移した。
    [[nodiscard]] scene::TextureId register_texture(std::unique_ptr<Texture> texture);

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

    // ★ phase16 ③-3: slots_ / free_indices_ は基底（AssetRegistry）へ移した。
    //   スロットの添字 = TextureId::index = bindless 配列の添字、という対応は変わらない。

    // 同一パス・同一フォーマットの再ロード防止。
    // ★ phase15 ③-1 / D-6: キーを {path, format} にした。同じ画像を sRGB と UNORM の
    //   両方の用途で読むモデルが実在するため、パスだけをキーにすると片方が
    //   もう片方のフォーマットで静かに返ってしまう。
    // ★ phase14 ②: unload したテクスチャのエントリを**ここからも消す**こと。
    //   消し忘れると、次の load が「登録済み」と誤判定して死んだハンドルを返す。
    std::map<std::pair<std::string, VkFormat>, scene::TextureId> by_path_;

    scene::TextureId default_texture_ = scene::kInvalidTextureId;
    scene::TextureId white_texture_ = scene::kInvalidTextureId;   // phase14 ③（中立フォールバック）
    scene::TextureId flat_normal_texture_ = scene::kInvalidTextureId;  // phase15 ③-2（中立フォールバック）
    // ★ phase16 ③-3: max_textures_ は基底の capacity_ へ移した（full() で判定する）。
};

// テクスチャへの所有参照（phase16 手順8 / ③-4）。
// MaterialEntry（マテリアル → テクスチャの連鎖解放）と Renderer（既定テクスチャの永続参照）が持つ。
using TextureRef = AssetRef<TextureRegistry, scene::TextureId>;

}  // namespace sq::graphics
