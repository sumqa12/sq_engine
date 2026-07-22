#include "sq/graphics/texture.hpp"
#include "sq/graphics/buffer.hpp"

#include <stdexcept>

// stb_image の実装をこの翻訳単位でのみ実体化する。
// STB_IMAGE_IMPLEMENTATION はプロジェクト全体で1箇所だけ定義すること。
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace sq::graphics {

Texture::Texture(VkPhysicalDevice physical_device, VkDevice device,
                 std::uint32_t graphics_queue_family, VkQueue graphics_queue,
                 const std::string& path)
    : device_(device) {
    // テクスチャ生成の手順（phase8プラン 項目3）
    //  1. stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha) で RGBA として読み込む。
    //     失敗（nullptr）なら throw。image_size = w * h * 4。
    int width, height, channels;
    stbi_uc* stbi_image = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (stbi_image == nullptr) {
        throw std::runtime_error("Texture : stbi_load : 画像の読み込みに失敗しました。");
    } else if (width <= 0 || height <= 0) {
        throw std::runtime_error("Texture : stbi_load : 画像のサイズが適切ではありません。");
    }

    //  2. StagingBuffer(physical_device, device, pixels, image_size) を作成 → stbi_image_free(pixels)。
    VkDeviceSize image_size = width * height * 4;
    StagingBuffer staging_buffer(physical_device, device, stbi_image, image_size);

    //  3. VkImageCreateInfo（imageType=2D, extent={w,h,1}, mipLevels=1, arrayLayers=1,
    //     format=VK_FORMAT_R8G8B8A8_SRGB, tiling=OPTIMAL,
    //     usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    //     samples=VK_SAMPLE_COUNT_1_BIT, initialLayout=UNDEFINED, sharingMode=EXCLUSIVE）
    //     → vkCreateImage(device_, ..., &image_)。
    VkImageCreateInfo image_create_info = {};
    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.extent.width = width;
    image_create_info.extent.height = height;
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (VkResult result = vkCreateImage(device_, &image_create_info, nullptr, &image_)
        ; result != VK_SUCCESS) {
        throw std::runtime_error("Texture : vkCreateImage : イメージの作成に失敗しました。");
    }

    //  4. vkGetImageMemoryRequirements → Buffer::find_memory_type(physical_device, bits,
    //     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) → vkAllocateMemory（戻り値チェック）→ vkBindImageMemory。
    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device_, image_, &memory_requirements);
    uint32_t memory_type_index = Buffer::find_memory_type(physical_device, memory_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo memory_allocate_info = {};
    memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_allocate_info.allocationSize = memory_requirements.size;
    memory_allocate_info.memoryTypeIndex = memory_type_index;
    if (VkResult result = vkAllocateMemory(device_, &memory_allocate_info, nullptr, &memory_); result != VK_SUCCESS) {
        throw std::runtime_error("Texture : vkAllocateMemory : メモリの確保に失敗しました。");
    }

    if (VkResult result = vkBindImageMemory(device_, image_, memory_, 0); result != VK_SUCCESS) {
        throw std::runtime_error("Texture : vkAllocateMemory : メモリの確保に失敗しました。");
    }


    //  5. 一時コマンドプール（VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, graphics_queue_family）を作り、
    //     1本のコマンドバッファに以下をまとめて記録:
    //       transition_image_layout(cmd, image_, UNDEFINED, TRANSFER_DST_OPTIMAL)
    //       vkCmdCopyBufferToImage(cmd, staging.handle(), image_, TRANSFER_DST_OPTIMAL, 1, &region)
    //         region: bufferOffset=0, imageSubresource{aspect=COLOR, mip0, layer0, count1}, imageExtent={w,h,1}
    //       transition_image_layout(cmd, image_, TRANSFER_DST_OPTIMAL, SHADER_READ_ONLY_OPTIMAL)
    //     vkQueueSubmit(graphics_queue, ...) → vkQueueWaitIdle → コマンドバッファ解放 → 一時プール破棄。
    VkCommandPool command_pool;
    VkCommandPoolCreateInfo command_pool_create_info = {};
    command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool_create_info.queueFamilyIndex = graphics_queue_family;
    vkCreateCommandPool(device, &command_pool_create_info, nullptr, &command_pool);

    // コマンドバッファを1本確保
    VkCommandBuffer command_buffer;
    VkCommandBufferAllocateInfo command_buffer_allocate_info = {};
    command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_allocate_info.commandBufferCount = 1;
    command_buffer_allocate_info.commandPool = command_pool;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    if (VkResult result = vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer); result != VK_SUCCESS) {
        throw std::runtime_error("Texture : vkAllocateCommandBuffers : コマンドバッファの確保に失敗しました。");
    }

    // 記録
    VkCommandBufferBeginInfo command_buffer_begin_info = {};
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info);

    transition_image_layout(command_buffer  , image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = width;
    region.imageExtent.height = height;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(command_buffer, staging_buffer.handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,  &region);

    transition_image_layout(command_buffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkEndCommandBuffer(command_buffer);

    // 送信 -> 完了待ち
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue); // 転送完了までブロック

    // 後始末
    vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
    vkDestroyCommandPool(device, command_pool, nullptr);

    //  6. VkImageViewCreateInfo（format=VK_FORMAT_R8G8B8A8_SRGB, aspect=VK_IMAGE_ASPECT_COLOR_BIT）
    //     → vkCreateImageView(device_, ..., &view_)。
    VkImageViewCreateInfo view_create_info = {};
    view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_create_info.image = image_;
    view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_create_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_create_info.subresourceRange.levelCount = 1;
    view_create_info.subresourceRange.layerCount = 1;
    view_create_info.subresourceRange.baseMipLevel = 0;
    view_create_info.subresourceRange.baseArrayLayer = 0;
    if (VkResult result = vkCreateImageView(device_, &view_create_info, nullptr, &view_); result != VK_SUCCESS) {
        throw std::runtime_error("Texture : vkCreateImageView : イメージビューの作成に失敗しました。");
    }

    (void)physical_device;
    (void)graphics_queue_family;
    (void)graphics_queue;
    (void)path;
}

Texture::~Texture() {
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

VkImageView Texture::view() const {
    return view_;
}

void Texture::transition_image_layout(VkCommandBuffer command_buffer, VkImage image,
                                      VkImageLayout old_layout, VkImageLayout new_layout) {
    // VkImageMemoryBarrier を構築し vkCmdPipelineBarrier で発行する（phase8プラン 項目3）
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0,
        0, nullptr,
        0, nullptr,
        1, &barrier);

    (void)command_buffer;
    (void)image;
    (void)old_layout;
    (void)new_layout;
}

}  // namespace sq::graphics
