#pragma once

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// 毎フレームCPUから書き換えるUniform BufferのRAIIラッパー。
// VkBuffer + VkDeviceMemoryを保持し、HOST_VISIBLE|HOST_COHERENTメモリを
// コンストラクタで一度だけvkMapMemoryして永続的にマップしておく（毎フレームの
// map/unmapを避け、update()でmemcpyするだけにする）。VertexBufferと同じ設計。
class UniformBuffer {
public:
    UniformBuffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size);
    ~UniformBuffer();

    UniformBuffer(const UniformBuffer&) = delete;
    UniformBuffer& operator=(const UniformBuffer&) = delete;

    // マップ済みメモリへdataをsizeバイトmemcpyする（HOST_COHERENTなのでflush不要）。
    void update(const void* data, std::size_t size);

    [[nodiscard]] VkBuffer handle() const;
    [[nodiscard]] VkDeviceSize size() const;

private:
    // VertexBuffer::find_memory_typeと同一ロジック（共通化は将来課題）。
    [[nodiscard]] static std::uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                                          std::uint32_t type_filter,
                                                          VkMemoryPropertyFlags properties);

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;  // コンストラクタでvkMapMemoryした永続マップ先
};

}  // namespace sq::graphics
