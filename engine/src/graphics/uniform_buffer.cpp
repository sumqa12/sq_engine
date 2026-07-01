#include "sq/graphics/uniform_buffer.hpp"

namespace sq::graphics {

UniformBuffer::UniformBuffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size)
    : device_(device), size_(size) {
    // TODO（VertexBufferと同じ流れ、usageだけ変える）:
    //  1. VkBufferCreateInfo（usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, size = size）
    //     でvkCreateBufferしbuffer_へ。
    //  2. vkGetBufferMemoryRequirements。
    //  3. find_memory_type(physical_device, requirements.memoryTypeBits,
    //     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)。
    //  4. vkAllocateMemory → vkBindBufferMemory。
    //  5. vkMapMemory(device_, memory_, 0, size, 0, &mapped_) で永続マップ
    //     （デストラクタまでunmapしない）。
    (void)physical_device;
}

UniformBuffer::~UniformBuffer() {
    // TODO: if (mapped_) vkUnmapMemory(device_, memory_);
    //       vkDestroyBuffer(device_, buffer_, nullptr);
    //       vkFreeMemory(device_, memory_, nullptr);
}

void UniformBuffer::update(const void* data, std::size_t size) {
    // TODO: std::memcpy(mapped_, data, size);  （<cstring>のinclude要）
    (void)data;
    (void)size;
}

VkBuffer UniformBuffer::handle() const {
    return buffer_;
}

VkDeviceSize UniformBuffer::size() const {
    return size_;
}

std::uint32_t UniformBuffer::find_memory_type(VkPhysicalDevice physical_device,
                                               std::uint32_t type_filter,
                                               VkMemoryPropertyFlags properties) {
    // TODO: VertexBuffer::find_memory_typeと同一実装。
    //  vkGetPhysicalDeviceMemoryPropertiesで取得し、
    //  (type_filter & (1 << i)) && (memoryTypes[i].propertyFlags & properties) == properties
    //  を満たす最初のiを返す。見つからなければ例外。
    (void)physical_device;
    (void)type_filter;
    (void)properties;
    return 0;
}

}  // namespace sq::graphics
