#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "sq/graphics/gpu_allocator.hpp"

namespace sq::graphics {

// VkBuffer + サブアロケートされたメモリ（Allocation）を所有するRAII基底クラス。
// 生成フロー（vkCreateBuffer → vkGetBufferMemoryRequirements →
// GpuAllocator::allocate → vkBindBufferMemory）を一手に担い、
// VertexBuffer / IndexBuffer / UniformBuffer / StagingBuffer の重複を解消する。
//
// phase11 ③: メモリ確保を GpuAllocator 経由にした。個別 vkAllocateMemory はやめ、
// 大ブロックからサブアロケートする。map/unmap はブロックの persistent mapping を参照するだけ。
//
// 注意: 派生クラスを直接（値またはunique_ptr<派生型>で）所有する前提のため、
// デストラクタは非virtual。Buffer* 経由での delete は行わないこと。
// C++の破棄順序により「派生デストラクタ → 基底デストラクタ」が保証されるので、
// 派生側は自分の後始末（例: UniformBufferのunmap）だけ行えばよい。
class Buffer {
public:
    Buffer(GpuAllocator& allocator, VkDevice device,
           VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    ~Buffer();  // vkDestroyBuffer → allocator_->free(allocation_) の順で解放する

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // `type_filter`に合致し、かつ`properties`を満たすメモリタイプのインデックスを探す。
    // GpuAllocator と Texture（イメージメモリ）から共用する唯一の実装。
    [[nodiscard]] static std::uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                                          std::uint32_t type_filter,
                                                          VkMemoryPropertyFlags properties);

    [[nodiscard]] VkBuffer handle() const;
    [[nodiscard]] VkDeviceSize size() const;

protected:
    // HOST_VISIBLE バッファのマップ先を返す。GpuAllocator がブロックを persistent map しているので、
    // ここでは allocation_.mapped（block.mapped + offset）を返すだけ（vkMapMemory は呼ばない）。
    // DEVICE_LOCAL バッファでは nullptr が返る（呼んではいけない）。
    [[nodiscard]] void* map();
    void unmap();  // persistent mapping のため no-op（ブロックの unmap は GpuAllocator が行う）

    // 既に (TRANSFER_DST | ... , DEVICE_LOCAL) で生成済みのこのバッファへ、staging 経由で
    // data を size バイト転送する（phase11 ②）。起動時アセット用（vkQueueWaitIdle でブロック）。
    // 手順:
    //   1. StagingBuffer staging(allocator, device_, data, size);  // TRANSFER_SRC/HOST_VISIBLE
    //   2. SingleTimeCommands cmd(device_, queue_family, queue);
    //   3. VkBufferCopy region{ .srcOffset = 0, .dstOffset = 0, .size = size };
    //      vkCmdCopyBuffer(cmd.handle(), staging.handle(), buffer_, 1, &region);
    //   4. cmd.submit_and_wait();  // staging はスコープ末尾で破棄
    void upload_with_staging(GpuAllocator& allocator,
                             std::uint32_t queue_family, VkQueue queue,
                             const void* data, VkDeviceSize size);

    GpuAllocator* allocator_ = nullptr;  // 解放時に free() を呼ぶために保持する（所有はしない）
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    Allocation allocation_{};  // GpuAllocator から借りた区間（旧 VkDeviceMemory memory_ を置換）
    VkDeviceSize size_ = 0;
};

// 転送元（TRANSFER_SRC）用のHOST_VISIBLEバッファ。Buffer基底の再利用例。
// 構築時に data を size バイト分書き込む。テクスチャ画像や DEVICE_LOCAL 頂点/インデックス
// バッファのステージング元として使う。
class StagingBuffer : public Buffer {
public:
    StagingBuffer(GpuAllocator& allocator, VkDevice device,
                  const void* data, VkDeviceSize size);
    // 追加で解放するリソースは無い（破棄は基底クラスに任せる）
};

}  // namespace sq::graphics
