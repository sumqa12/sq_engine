#pragma once

#include <cstddef>

#include <vulkan/vulkan.h>

#include "sq/graphics/buffer.hpp"

namespace sq::graphics {

// 毎フレームCPUから書き換えるUniform BufferのRAIIラッパー。
// バッファ生成・解放は基底クラスBufferが担い、本クラスは
// 「コンストラクタで一度だけmap()して永続的にマップしておく」責務のみ持つ
// （毎フレームのmap/unmapを避け、update()でmemcpyするだけにする）。
// （コピー禁止・handle()/size()は基底クラスから継承）
class UniformBuffer : public Buffer {
public:
    UniformBuffer(GpuAllocator& allocator, VkDevice device, VkDeviceSize size);
    // 永続マップのunmap()のみ行う。バッファ/メモリの解放はその後に走る基底デストラクタが行う
    // （C++の破棄順序: 派生デストラクタ → 基底デストラクタ）。
    ~UniformBuffer();

    // マップ済みメモリへdataをsizeバイトmemcpyする（HOST_COHERENTなのでflush不要）。
    void update(const void* data, std::size_t size);

private:
    void* mapped_ = nullptr;  // コンストラクタでmap()した永続マップ先
};

}  // namespace sq::graphics
