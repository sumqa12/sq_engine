#pragma once

#include <cstddef>

#include <vulkan/vulkan.h>

#include "sq/graphics/buffer.hpp"
#include "sq/graphics/light_data.hpp"

namespace sq::graphics {

// 1フレーム分のライト配列を保持するストレージバッファ（SSBO）のRAIIラッパー（phase15 ① / D-3）。
//
// 構造は InstanceBuffer とまったく同じ（永続マップして update() で memcpy するだけ）で、
// 違いは要素の型が LightData であることだけ。
//
// なぜ set=1（アセット側）ではなく set=0 に置くのか:
//   ライトは毎フレーム動きうる（太陽が回る・キャラに付いた光源が移動する）。
//   phase14 D-2 で決めた「set=0 = フレームごとに変わるもの / set=1 = 起動後は不変」という
//   役割分担に素直に従うと set=0 の binding=2 になる。
//
// ★ kFramesInFlight 個作ること（Renderer 側）。1本を使い回すと、GPU が前フレームを
//   読んでいる最中に CPU が次フレームを書き込んで壊れる（カメラUBO・InstanceBuffer と同じ理由）。
//
// TODO: InstanceBuffer とコードが完全に重複する。テンプレート（MappedStorageBuffer<T> 等）に
//   括り出すかは**3本目が来たときに**決める。2本で共通化すると、何を共通項と見なすべきかが
//   まだ見えていない状態で形を固めてしまう。
class LightBuffer : public Buffer {
public:
    // max_lights 件ぶんの領域を確保する（実バイト数は max_lights * sizeof(LightData)）。
    //
    // 基底 Buffer に次を渡す:
    //   size       = max_lights * sizeof(LightData)
    //   usage      = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    //   properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    // そのうえで mapped_ = map(); capacity_ = max_lights; を行う。
    LightBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_lights);

    // 永続マップの unmap() のみ行う（バッファ/メモリの解放はその後に走る基底デストラクタ）。
    ~LightBuffer();

    LightBuffer(const LightBuffer&) = delete;
    LightBuffer& operator=(const LightBuffer&) = delete;

    // data の先頭 count 要素をマップ済みメモリへ memcpy する（HOST_COHERENT なので flush 不要）。
    //
    // ★ count > capacity_ の書き込みは**ここで弾くこと**（そのまま memcpy すると
    //   確保していない領域を踏む）。呼び出し側（Renderer）は上限クランプ済みの想定だが、
    //   最後の砦をこちらにも置いておく。
    // ★ count == 0 は正常（ライトが1つも無いシーン）。memcpy を呼ばずに返ること。
    void update(const LightData* data, std::size_t count);

    // 収容できるライト数（バイト数ではない）。
    [[nodiscard]] std::size_t capacity() const;

private:
    void* mapped_ = nullptr;    // コンストラクタで map() した永続マップ先
    std::size_t capacity_ = 0;  // 収容できるライト数
};

}  // namespace sq::graphics
