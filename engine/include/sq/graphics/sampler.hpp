#pragma once

#include <vulkan/vulkan.h>

namespace sq::graphics {

// サンプラーの可変設定（phase16 ⓪-3）。
//
// phase15 まではサンプラーが1本しか無く、引数を1つも取らなかった。
// phase16 では**3本**になるので、違いを設定として外へ出す:
//
//   1. 既存の共有サンプラ  … REPEAT / 異方性あり / 比較なし          （bindless テクスチャ配列）
//   2. シャドウ用          … CLAMP_TO_BORDER / 白ボーダー / 比較あり （set=2 binding=0。phase16 ①）
//   3. キューブ・LUT 用    … CLAMP_TO_EDGE / 異方性なし / 比較なし   （set=2 binding=1..3。phase16 ②）
//
// ★ 既定値は**すべて現在の挙動**にすること（⓪-5 の完了条件）。
struct SamplerConfig {
    VkFilter filter = VK_FILTER_LINEAR;                                     // magFilter / minFilter の両方に使う
    VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;     // U/V/W の3つに同じ値を入れる
    VkBorderColor border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    bool anisotropy = true;   // ★ デバイスが非対応なら結局 false になる（実装側で AND を取る）

    // 比較サンプラ（シャドウ用）。true なら compareEnable = VK_TRUE。
    //
    // ★ compare_op は VK_COMPARE_OP_LESS_OR_EQUAL。
    //   「サンプルした深度 <= 比較値なら 1.0（= 手前にいる = 照らされている）」という意味になる。
    //   GREATER 系にすると**影と光が反転**するが、それでも「それらしい絵」が出るので
    //   目視では気付きにくい。影が出ないときの疑いどころの一つ（①-11）。
    // ★ GLSL 側の型も変わる: 比較サンプラは sampler2D ではなく sampler2DShadow で受ける。
    bool compare_enable = false;
    VkCompareOp compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;

    // ミップの上限。既存テクスチャは VK_LOD_CLAMP_NONE（= 全レベル使う）。
    // ★ シャドウマップは mip を持たないので 0.0f にしてよい（どちらでも動く）。
    float max_lod = VK_LOD_CLAMP_NONE;
};

// VkSampler を所有するRAIIラッパー。テクスチャのサンプリング方法（フィルタリング・
// アドレッシング・異方性）を定義する。
// phase15 までは全テクスチャで1つを共有していたが、phase16 で用途ごとに3本になる（SamplerConfig）。
class Sampler {
public:
    // physical_device は異方性フィルタリングの上限値（maxSamplerAnisotropy）の取得に使う。
    // config: phase16 ⓪-3 で追加。既定は phase15 までと同じ設定。
    Sampler(VkPhysicalDevice physical_device, VkDevice device, const SamplerConfig& config = {});
    ~Sampler();  // vkDestroySampler

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    [[nodiscard]] VkSampler handle() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
