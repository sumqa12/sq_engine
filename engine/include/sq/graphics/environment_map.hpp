#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "compute_pipeline.hpp"
#include "cubemap.hpp"
#include "gpu_allocator.hpp"
#include "storage_texture.hpp"

namespace sq::graphics {

// IBL の前計算とその成果物を持つ（phase16 ②）。
//
// 手順5（②-9 まで）で持つもの:
//   environment_cube_ … HDR の equirectangular から変換した元の環境キューブ（スカイボックス用）
// 手順6で足すもの:
//   irradiance_       … 32×32、mip 1（②-5）
//   prefiltered_      … 128×128、mip 5（②-6）
//   brdf_lut_         … 512×512 の 2D、R16G16_SFLOAT（②-7。Cubemap ではない。
//                       Device::is_supported_storage_image_extended_formats が前提）
//
// ★ 前計算は**コンストラクタで1回だけ**（D-7）。毎フレームの経路には一切入らない。
//   すべて SingleTimeCommands に載せ、完了を待ってからコンストラクタを抜ける。
// ★ Renderer::recreate_swapchain で作り直さないこと（解像度がウィンドウと無関係）。
//   作り直すと set=2 に書いた view が死ぬ（シャドウマップと同じ理由）。
class EnvironmentMap {
public:
    // 環境キューブの一辺。8 の倍数であること（local_size = 8。②-4）。
    static constexpr std::uint32_t kEnvironmentCubeSize = 512;
    // キューブのフォーマット（D-8）。HDR を保持でき、ストレージイメージに使える。
    //   ★ *_SRGB はストレージイメージに使えない。
    static constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // ---- phase16 手順6（②-5〜②-7） ----
    //
    // irradiance（②-5）。拡散の畳み込みは極端に低周波なので 32 で十分
    // （128 にしても絵はほぼ変わらず、前計算だけが 16 倍遅くなる）。
    static constexpr std::uint32_t kIrradianceSize = 32;

    // prefiltered specular（②-6）。mip 0 = roughness 0.0 … mip 4 = roughness 1.0。
    //   roughness = mip / (kPrefilteredMips - 1)
    // ★ kPrefilteredMips - 1 は triangle.frag の kMaxReflectionLod（4.0）と**必ず一致**させること。
    //   ずれると「roughness 1.0 だけ映り込みが鋭い」という気付きにくい壊れ方をする（②-8）。
    static constexpr std::uint32_t kPrefilteredSize = 128;
    static constexpr std::uint32_t kPrefilteredMips = 5;

    // BRDF LUT（②-7）。x = NdotV、y = roughness、出力 rg = (scale, bias)。
    static constexpr std::uint32_t kBrdfLutSize = 512;
    // ★ brdf_lut.comp の書式修飾子 rg16f と一致させること。
    //   拡張ストレージ書式なので、デバイス機能 shaderStorageImageExtendedFormats の有効化が要る（device.cpp）。
    static constexpr VkFormat kBrdfLutFormat = VK_FORMAT_R16G16_SFLOAT;

    // hdr_path: 実行時CWD基準の .hdr。見つからなければ灰色の一様キューブになる（HdrTexture の仕様）。
    // input_sampler: equirect をサンプルするサンプラ（所有しない。変換中だけ使う）。
    //   Renderer の cube_sampler_（CLAMP_TO_EDGE / 異方性なし）を渡す。
    //   ★ equirect の u（経度）は本来一周してつながっているので REPEAT が正しいが、
    //     CLAMP_TO_EDGE でも継ぎ目は 1 テクセル以下で目立たない。
    //     REPEAT にすると v（緯度）まで回り込み、極で反対側の極の色が混ざる。
    //     U と V で別のアドレスモードにしたくなったら SamplerConfig を拡張する。
    EnvironmentMap(VkDevice device, GpuAllocator& allocator,
                   std::uint32_t graphics_queue_family, VkQueue graphics_queue,
                   const std::string& hdr_path, VkSampler input_sampler);
    ~EnvironmentMap();

    EnvironmentMap(const EnvironmentMap&) = delete;
    EnvironmentMap& operator=(const EnvironmentMap&) = delete;

    // 元の環境キューブ（スカイボックスが背景として読む）。
    [[nodiscard]] const Cubemap& environment_cube() const;

    // set=2 binding=1（samplerCube irradiance_map）に書く。
    [[nodiscard]] const Cubemap& irradiance() const;
    // set=2 binding=2（samplerCube prefiltered_map）に書く。
    // ★ 手順5まで binding=2 に入れていた environment_cube() をこれに差し替える
    //   （スカイボックスは mip 0 を読むので 128×128 の背景になる。skybox.frag の注記）。
    [[nodiscard]] const Cubemap& prefiltered() const;
    // set=2 binding=3（sampler2D brdf_lut）に書く。
    [[nodiscard]] const StorageTexture& brdf_lut() const;

private:
    // 前計算用のディスクリプタセットレイアウトとプールを作る（②-4）。
    //   equirect_set_layout_:
    //     binding=0  COMBINED_IMAGE_SAMPLER  COMPUTE  入力（HDR の 2D）
    //     binding=1  STORAGE_IMAGE           COMPUTE  出力（環境キューブの storage_view(0)）
    //   bake_pool_: **Renderer のプールとは別に持つ**。
    //     ★ Renderer のプールは UPDATE_AFTER_BIND で作られており、STORAGE_IMAGE の枠も無い。
    //       前計算の都合でそちらを広げると、描画側のプール計算（⓪-4）が汚れる。
    //     ★ 手順5では set 1 個 / COMBINED_IMAGE_SAMPLER 1 / STORAGE_IMAGE 1 で足りる。
    //       手順6で irradiance（1）+ prefiltered（mip ごとに 1 = 5）+ BRDF LUT（1）が加わるので、
    //       そのとき kMaxBakeSets と各枠を増やすこと（足りないと vkAllocateDescriptorSets が
    //       VK_ERROR_OUT_OF_POOL_MEMORY を返す。戻り値は必ず見ること）。
    void create_bake_descriptors();

    // HDR の equirectangular -> 環境キューブ（②-4）。
    //   1. HdrTexture を作る（このスコープだけ生きる一時オブジェクト）
    //   2. ディスクリプタセットを確保して binding=0 / 1 を書く
    //        binding=0: { input_sampler, hdr.view(), SHADER_READ_ONLY_OPTIMAL }
    //        binding=1: { VK_NULL_HANDLE, environment_cube_->storage_view(0), GENERAL }
    //   3. SingleTimeCommands の中で:
    //        transition_image_layout(cube, UNDEFINED -> GENERAL, mip 0..1, layer 0..6)
    //        vkCmdBindPipeline(COMPUTE, equirect_pipeline_)
    //        vkCmdBindDescriptorSets(COMPUTE, layout, set=0)
    //        vkCmdDispatch(size / 8, size / 8, 6)      ★ z = 面番号
    //        transition_image_layout(cube, GENERAL -> SHADER_READ_ONLY, mip 0..1, layer 0..6)
    //      submit_and_wait()
    //   ★ HdrTexture は submit_and_wait() の**後**に破棄されること（GPU が読み終える前に消さない）。
    void bake_environment_cube(const std::string& hdr_path, VkSampler input_sampler);

    // ---- phase16 手順6 ----
    //
    // ★ 3つとも bake_environment_cube と**同じ形**（セット確保 → 2 binding を書く →
    //   UNDEFINED -> GENERAL → bind → dispatch → GENERAL -> SHADER_READ_ONLY → submit_and_wait）。
    //   違うのは「入力」「出力」「ディスパッチの回数」だけ。
    //   ★ 共通部分を private な補助関数に括り出すかは任せるが、4回コピーすると
    //     vkUpdateDescriptorSets の本数（手順5で踏んだ）のような間違いが4箇所に増える。
    //
    // ★ ディスクリプタセットレイアウトは equirect_set_layout_ を**共用**できる
    //   （binding=0 COMBINED_IMAGE_SAMPLER / binding=1 STORAGE_IMAGE。型が sampler2D か
    //   samplerCube かはシェーダ側の話で、レイアウトは区別しない）。
    //   共用するなら名前を bake_set_layout_ などに変えておくと読み違えない。
    //
    // ★ 入力の環境キューブは bake_environment_cube の最後で SHADER_READ_ONLY になっており、
    //   その遷移の dst ステージに COMPUTE_SHADER が含まれている（image_utils）。
    //   なので irradiance / prefilter の前に追加のバリアは要らない
    //   （ただし別々の submit_and_wait なので、そもそも完了済み）。

    // irradiance（②-5）。
    //   入力 binding=0: { input_sampler, environment_cube_->sample_view(), SHADER_READ_ONLY }
    //   出力 binding=1: { VK_NULL_HANDLE, irradiance_->storage_view(0), GENERAL }
    //   dispatch: (kIrradianceSize / 8, kIrradianceSize / 8, 6) を1回
    void bake_irradiance(VkSampler input_sampler);

    // prefiltered specular（②-6）。
    //   mip = 0..kPrefilteredMips-1 について:
    //     セットを1個ずつ確保する（★ 出力の storage_view(mip) が mip ごとに違うため。計5個）
    //     出力 binding=1: { VK_NULL_HANDLE, prefiltered_->storage_view(mip), GENERAL }
    //     vkCmdPushConstants(layout, COMPUTE, 0, sizeof(float), &roughness)
    //       roughness = float(mip) / float(kPrefilteredMips - 1)
    //     size = max(kPrefilteredSize >> mip, 1)
    //     dispatch: ((size + 7) / 8, (size + 7) / 8, 6)
    //       ★ size / 8 だと mip の一辺が 8 未満になったときに 0 になり、その mip が一切書かれない
    //         （roughness 1.0 の金属だけが真っ黒。②-6）。今の 128 / mip 5 なら最小 8 で問題は出ないが、
    //         切り上げにしておけば mip 数を増やしても壊れない（シェーダ側の範囲チェックは手順5で入れ済み）。
    //   遷移は**全 mip まとめて**前後に1回ずつでよい（0, kPrefilteredMips, 0, 6）。
    //   ★ 5回のディスパッチは同じ SingleTimeCommands に積んでよい（書き込み先の mip が重ならないので
    //     ディスパッチ間のバリアは不要）。
    void bake_prefiltered(VkSampler input_sampler);

    // BRDF LUT（②-7）。環境マップに依存しない固定の表。
    //   出力 binding=1: { VK_NULL_HANDLE, brdf_lut_->view(), GENERAL }
    //   ★ binding=0（入力）は**書かなくてよい**。brdf_lut.comp は binding=0 を宣言しない
    //     （= 静的に使用しない）ので、未更新のままでもバリデーションは出ない。
    //     手順5で出た「has never been updated」は、シェーダが使っている binding が未更新だったから。
    //   遷移は image_utils の COLOR / layer 1 / mip 1（既定引数のまま）でよい。
    //   dispatch: (kBrdfLutSize / 8, kBrdfLutSize / 8, 1)   ★ z は 1（キューブではない）
    void bake_brdf_lut();

    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;  // 所有しない
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    // 前計算で確保するディスクリプタセットの総数（手順6）:
    //   equirect 1 + irradiance 1 + prefiltered kPrefilteredMips（5）+ BRDF LUT 1 = 8
    // ★ プールの枠（COMBINED_IMAGE_SAMPLER / STORAGE_IMAGE）も**それぞれ kMaxBakeSets 個**にすること。
    //   レイアウトの binding 数ぶんがセット確保時に消費される（BRDF LUT が binding=0 を
    //   書かなくても、確保した時点で枠は減る）。
    static constexpr std::uint32_t kMaxBakeSets = 2 + kPrefilteredMips + 1;
    VkDescriptorSetLayout bake_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bake_pool_ = VK_NULL_HANDLE;

    // ★ 前計算が終われば不要になる。デストラクタまで持っていても害は無いが、
    //   コンストラクタの最後で reset() / 破棄してもよい（起動後のメモリを少しでも返すなら）。
    std::unique_ptr<ComputePipeline> equirect_pipeline_;

    // 手順6の前計算パイプライン。equirect_pipeline_ と同じく、焼き終えたら reset() してよい。
    //   irradiance_pipeline_ : "shaders/irradiance.comp.spv"、プッシュ定数 0
    //   prefilter_pipeline_  : "shaders/prefilter.comp.spv"、プッシュ定数 sizeof(float)（roughness）
    //   brdf_lut_pipeline_   : "shaders/brdf_lut.comp.spv"、プッシュ定数 0
    std::unique_ptr<ComputePipeline> irradiance_pipeline_;
    std::unique_ptr<ComputePipeline> prefilter_pipeline_;
    std::unique_ptr<ComputePipeline> brdf_lut_pipeline_;

    std::unique_ptr<Cubemap> environment_cube_;
    std::unique_ptr<Cubemap> irradiance_;        // kIrradianceSize, mip 1
    std::unique_ptr<Cubemap> prefiltered_;       // kPrefilteredSize, mip kPrefilteredMips
    std::unique_ptr<StorageTexture> brdf_lut_;   // kBrdfLutSize × kBrdfLutSize
};

}  // namespace sq::graphics
