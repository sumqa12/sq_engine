#include "sq/graphics/gpu_allocator.hpp"

#include <filesystem>
#include <stdexcept>

#include "sq/graphics/buffer.hpp"  // Buffer::find_memory_type を再利用する（重複実装しない）

namespace sq::graphics {

GpuAllocator::GpuAllocator(VkPhysicalDevice physical_device, VkDevice device)
    : physical_device_(physical_device), device_(device) {
    // 生成時にブロックは作らない（遅延確保）。最初の allocate() で必要な分だけ作る。
}

GpuAllocator::~GpuAllocator() {
    // 全ブロックを解放する。
    for (Block& block : blocks_) {
        if (block.mapped) { vkUnmapMemory(device_, block.memory); }
        vkFreeMemory(device_, block.memory, nullptr);
    }
    blocks_.clear();
    // 注意: ここに来る時点で全 Buffer が破棄済み（= free 済み）であることが前提。
    // 破棄順序（全バッファ → GpuAllocator → VkDevice）を Device / Renderer 側で担保すること。
}

Allocation GpuAllocator::allocate(const VkMemoryRequirements& reqs, VkMemoryPropertyFlags properties, bool linear) {
    // plan11 (③-1)
    const std::uint32_t type = Buffer::find_memory_type(physical_device_, reqs.memoryTypeBits, properties);
    //  2. blocks_ を走査し、block.memory_type_index == type のブロックの free_ranges から、
    //     align_up(range.offset, reqs.alignment) に reqs.size が収まる区間を探す（first-fit）。
    //     - 見つかったら: aligned_offset を切り出し、range を「前の隙間」「後ろの残り」に分割して
    //       free_ranges を更新する。
    //  3. 見つからなければ create_block(type, std::max(kDefaultBlockSize, reqs.size), properties) で新規確保し、
    //     その先頭から切り出す。
    auto align_up = [](VkDeviceSize x, VkDeviceSize a) {
        return (x + a - 1) & ~(a - 1);
    };

    for (std::uint32_t bi = 0; bi < blocks_.size(); ++bi) {
        Block& block = blocks_[bi]; // ★ 参照。コピーしない
        if (block.linear != linear || block.memory_type_index != type) continue;
        for (std::size_t ri = 0; ri < block.free_ranges.size(); ++ri) {
            auto [offset, size] = block.free_ranges[ri];
            if (VkDeviceSize aligned = align_up(offset, reqs.alignment)
                ; aligned + reqs.size <= offset + size) {   // ★ サイズ判定

                // ★ 空き区間を分割して更新する
                block.free_ranges.erase(block.free_ranges.begin() + static_cast<long long>(ri));

                if (aligned > offset) { // 前の隙間
                    block.free_ranges.push_back({ offset, aligned - offset });
                }

                if (VkDeviceSize used_end = aligned + reqs.size; used_end < offset + size) { // 後ろの残り
                    block.free_ranges.push_back({ used_end, offset + size - used_end });
                }

                return Allocation{
                    block.memory, aligned, reqs.size, type, bi,
                    block.mapped ? static_cast<char*>(block.mapped) + aligned : nullptr
                };
            }
        }
    }

    // 4. 見つからない場合、作る
    uint32_t index = create_block(type, max(kDefaultBlockSize, reqs.size), properties, linear);
    Block& block = blocks_[index];
    block.free_ranges.erase(block.free_ranges.begin());

    return Allocation{
        block.memory, 0, reqs.size, type, index,
        block.mapped ? static_cast<char*>(block.mapped) : nullptr
    };
}

void GpuAllocator::free(const Allocation& allocation) {
    Block& block = blocks_[allocation.block_index];
    block.free_ranges.push_back({ allocation.offset, allocation.size });
    // phase13 隣接空きのマージ

}

std::uint32_t GpuAllocator::create_block(std::uint32_t memory_type_index, VkDeviceSize size,
                                         VkMemoryPropertyFlags properties, bool linear) {
    // plan11 (③-1)
    // 1. メモリの割当て
    VkMemoryAllocateInfo alloc_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = size,
        .memoryTypeIndex = memory_type_index
    };
    VkDeviceMemory memory;
    if (VkResult result = vkAllocateMemory(device_, &alloc_info, nullptr, &memory)
        ; result != VK_SUCCESS) {
        throw std::runtime_error("GpuAllocator : create_block : メモリの割当に失敗しました。");
    }

    Block block {
        .memory=memory,
        .size=size,
        .memory_type_index=memory_type_index,
        .linear = linear
    };

    // 3. HOST_VISIBLE を含むなら vkMapMemory(device_, block.memory, 0, VK_WHOLE_SIZE, 0, &block.mapped)。
    // （persistent mapping。個別 Buffer では map/unmap しない）
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (VkResult result = vkMapMemory(device_, block.memory, 0, VK_WHOLE_SIZE, 0, &block.mapped)
            ; result != VK_SUCCESS) {
            throw std::runtime_error("GpuAllocator : create_block : メモリのマップに失敗しました。");
        }
    }

    // 4. ブロックの空き区間
    block.free_ranges = {{ 0, size }};

    // 5. ブロックを追加して、インデックスを返す
    blocks_.push_back(std::move(block));
    return static_cast<std::uint32_t>(blocks_.size() - 1);
}

}  // namespace sq::graphics
