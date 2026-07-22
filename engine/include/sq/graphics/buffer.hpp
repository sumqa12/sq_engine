#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// VkBuffer + VkDeviceMemory を所有するRAII基底クラス。
// 生成フロー（vkCreateBuffer → vkGetBufferMemoryRequirements →
// find_memory_type → vkAllocateMemory → vkBindBufferMemory）を一手に担い、
// VertexBuffer / UniformBuffer の重複を解消する。
//
// 注意: 派生クラスを直接（値またはunique_ptr<派生型>で）所有する前提のため、
// デストラクタは非virtual。Buffer* 経由での delete は行わないこと。
// C++の破棄順序により「派生デストラクタ → 基底デストラクタ」が保証されるので、
// 派生側は自分の後始末（例: UniformBufferのunmap）だけ行えばよい。
class Buffer {
public:
    Buffer(VkPhysicalDevice physical_device, VkDevice device,
           VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    ~Buffer();  // vkDestroyBuffer → vkFreeMemory の順で解放する

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // `type_filter`に合致し、かつ`properties`を満たすメモリタイプのインデックスを探す。
    // （旧 VertexBuffer::find_memory_type / UniformBuffer::find_memory_type の統合先）
    [[nodiscard]] static std::uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                                          std::uint32_t type_filter,
                                                          VkMemoryPropertyFlags properties);

    [[nodiscard]] VkBuffer handle() const;
    [[nodiscard]] VkDeviceSize size() const;

protected:
    // バッファ全域 [0, size_) を vkMapMemory してポインタを返す（失敗時は例外）。
    [[nodiscard]] void* map();
    void unmap();

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
};

// 転送元（TRANSFER_SRC）用のHOST_VISIBLEバッファ。Buffer基底の再利用例（4例目）。
// 構築時に data を size バイト分書き込む。テクスチャ画像のGPUアップロード元として使う
// （将来はDEVICE_LOCALな頂点/インデックスバッファのステージングにも流用できる）。
class StagingBuffer : public Buffer {
public:
    StagingBuffer(VkPhysicalDevice physical_device, VkDevice device,
                  const void* data, VkDeviceSize size);
    // 追加で解放するリソースは無い（破棄は基底クラスに任せる）
};

}  // namespace sq::graphics
