#pragma once

#include <vulkan/vulkan.h>

#include "sq/graphics/buffer.hpp"
#include "sq/graphics/material_data.hpp"

namespace sq::graphics {

// マテリアル配列を保持するストレージバッファ（SSBO）のRAIIラッパー（phase14 ① / D-1）。
//
// InstanceBuffer とほぼ同じ形（永続マップして memcpy するだけ）だが、性格が違う:
//   - InstanceBuffer … 毎フレーム全体を書き換える → kFramesInFlight 個必要
//   - MaterialBuffer … 起動時に書いて以後不変      → **1本でよい**（D-2）
// そのため update() は「1件だけ書き換える」形にしてある（全件転送する必要が無い）。
//
// ★ Buffer::map() は protected なので、Buffer を直接メンバに持つ形では永続マップできない。
//   UniformBuffer / InstanceBuffer と同じく「派生して map する」のがこのエンジンの流儀。
class MaterialBuffer : public Buffer {
public:
    // max_materials 件ぶんの領域を確保する（実バイト数は max_materials * sizeof(MaterialData)）。
    // usage は VK_BUFFER_USAGE_STORAGE_BUFFER_BIT、メモリは HOST_VISIBLE | HOST_COHERENT。
    MaterialBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_materials);
    // 永続マップの unmap() のみ行う（解放は基底デストラクタ）。
    ~MaterialBuffer();

    // index 番目の要素へ1件書き込む（HOST_COHERENT なので flush 不要）。
    // ★ index >= capacity_ は弾くこと（超えるとヒープ破壊になる）。
    void write(std::size_t index, const MaterialData& data);

    // 収容できるマテリアル数（バイト数ではない）。
    [[nodiscard]] std::size_t capacity() const;

private:
    void* mapped_ = nullptr;   // コンストラクタで map() した永続マップ先
    std::size_t capacity_ = 0; // 収容できるマテリアル数
};

}  // namespace sq::graphics
