#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "compute_pipeline.hpp"
#include "cubemap.hpp"
#include "gpu_allocator.hpp"

namespace sq::graphics {

// IBL の前計算とその成果物を持つ（phase16 ②）。
//
// 手順5（②-9 まで）で持つもの:
//   environment_cube_ … HDR の equirectangular から変換した元の環境キューブ（スカイボックス用）
// 手順6で足すもの:
//   irradiance_       … 32×32、mip 1（②-5）
//   prefiltered_      … 128×128、mip 5（②-6）
//   brdf_lut_         … 512×512 の 2D、R16G16_SFLOAT（②-7。Cubemap ではない）
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

    // TODO(手順6): irradiance() / prefiltered() / brdf_lut_view() を足す。

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

    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;  // 所有しない
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    static constexpr std::uint32_t kMaxBakeSets = 1;  // 手順6で増やすi
    VkDescriptorSetLayout equirect_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bake_pool_ = VK_NULL_HANDLE;

    // ★ 前計算が終われば不要になる。デストラクタまで持っていても害は無いが、
    //   コンストラクタの最後で reset() / 破棄してもよい（起動後のメモリを少しでも返すなら）。
    std::unique_ptr<ComputePipeline> equirect_pipeline_;

    std::unique_ptr<Cubemap> environment_cube_;
};

}  // namespace sq::graphics
