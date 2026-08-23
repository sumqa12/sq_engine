#pragma once

#include <vulkan/vulkan.h>

#include "sq/graphics/buffer.hpp"
#include "sq/graphics/instance_data.hpp"

namespace sq::graphics {

// per-instance データを毎フレームCPUから書き換えるストレージバッファ（SSBO）のRAIIラッパー
// （phase13 ②-1 / D-5）。
//
// 構造は UniformBuffer とほぼ同じ（永続マップして update() で memcpy するだけ）で、
// 違いは usage が VK_BUFFER_USAGE_STORAGE_BUFFER_BIT である点。
// UBO ではなく SSBO を使う理由は、UBO のサイズ上限（maxUniformBufferRange の最小保証は
// 16KiB = InstanceData 約170体分）では足りないため。SSBO は桁違いに大きく取れる。
//
// ★ kFramesInFlight 個作ること（Renderer 側）。1つを使い回すと、GPU が前フレームを
//   読んでいる最中に CPU が次フレームを書き込んで壊れる（カメラUBOと同じ理由）。
class InstanceBuffer : public Buffer {
public:
    // max_instances 体ぶんの領域を確保する（実バイト数は max_instances * sizeof(InstanceData)）。
    InstanceBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_instances);
    // 永続マップの unmap() のみ行う（UniformBuffer と同じ。解放は基底デストラクタ）。
    ~InstanceBuffer();

    // data の先頭 count 要素をマップ済みメモリへ memcpy する（HOST_COHERENT なので flush 不要）。
    // ★ count > capacity() の場合の扱いは呼び出し側で決める
    //   ここでは capacity_ を超える書き込みを弾くこと（超えるとヒープ破壊になる）。
    void update(const InstanceData* data, std::size_t count);

    // 確保済みのインスタンス数（バイト数ではない）。
    [[nodiscard]] std::size_t capacity() const;

private:
    void* mapped_ = nullptr;   // コンストラクタで map() した永続マップ先
    std::size_t capacity_ = 0; // 収容できるインスタンス数
};

}  // namespace sq::graphics
