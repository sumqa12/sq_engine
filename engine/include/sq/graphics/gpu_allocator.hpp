#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// GpuAllocator が貸し出す部分区間。値型（コピー可）。
// free() に必要な内部情報（ブロック番号・空きリストの位置等）は実装側が block_index と
// offset/size から解釈する。
struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;  // 所属ブロックの VkDeviceMemory
    VkDeviceSize offset = 0;                 // ブロック内オフセット（alignment 済み）
    VkDeviceSize size = 0;                   // 占有バイト数（alignment 切り上げ後）
    std::uint32_t memory_type_index = 0;     // どのメモリタイプのブロックか
    std::uint32_t block_index = 0;           // そのメモリタイプ内でのブロック番号
    void* mapped = nullptr;                  // HOST_VISIBLE ならブロックの map 先 + offset。DEVICE_LOCAL は nullptr
};

// 1 つの大きな VkDeviceMemory を確保して部分区間を貸し出す単純なサブアロケータ（学習用・自作）。
// メモリタイプごとに大ブロックを確保し、各ブロック内はフリーリストで管理する。
// Phase 7 で観測した small-dedicated-allocation 警告（小バッファごとの個別 vkAllocateMemory）への対応。
//
// 将来は vcpkg の vulkan-memory-allocator に差し替える前提。差し替え時の変更が
// allocate / free / map の 3 点に閉じるよう、Buffer からはこの API 越しにしか触らせない。
class GpuAllocator {
public:
    GpuAllocator(VkPhysicalDevice physical_device, VkDevice device);
    ~GpuAllocator();  // 全ブロックを unmap（HOST_VISIBLE のみ）してから vkFreeMemory する

    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    // reqs（vkGet*MemoryRequirements の結果）と properties を満たす区間を確保する。
    // 手順:
    //   1. Buffer::find_memory_type(physical_device_, reqs.memoryTypeBits, properties) でメモリタイプ index を決める
    //   2. そのメモリタイプの既存ブロックを走査し、reqs.alignment で切り上げた offset に
    //      reqs.size が収まる空き区間を探す（最初に見つかったものを使う = first-fit）
    //   3. 無ければ新しいブロックを確保する（kDefaultBlockSize と reqs.size の大きい方）。
    //      HOST_VISIBLE を含む properties なら、生成直後に vkMapMemory でブロック全体を map して保持する
    //   4. 見つけた空き区間を offset で切り出し、残りを空きリストへ戻して Allocation を返す
    //      （mapped は HOST_VISIBLE なら block.mapped + offset、そうでなければ nullptr）
    [[nodiscard]] Allocation allocate(const VkMemoryRequirements& reqs, VkMemoryPropertyFlags properties);

    // allocation の区間を空きリストへ戻す。
    // 手順: block_index からブロックを引き、{offset, size} を空きリストへ push する。
    //   （隣接する空き区間のマージ=defrag は将来課題。まずは push するだけでよい）
    void free(const Allocation& allocation);

private:
    // ブロック内の空き区間 [offset, offset+size)。
    struct FreeRange {
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
    };

    // 1 つの大きな VkDeviceMemory とその空き管理。
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        std::uint32_t memory_type_index = 0;
        void* mapped = nullptr;              // HOST_VISIBLE なら map 先。それ以外は nullptr
        std::vector<FreeRange> free_ranges;  // 初期状態は [{0, size}] の 1 区間
    };

    // 新しいブロックを確保して blocks_ に追加し、その index を返す。
    // 手順: vkAllocateMemory → HOST_VISIBLE を含むなら vkMapMemory → 空きリストを [{0, size}] で初期化。
    [[nodiscard]] std::uint32_t create_block(std::uint32_t memory_type_index,
                                             VkDeviceSize size, VkMemoryPropertyFlags properties);

    static constexpr VkDeviceSize kDefaultBlockSize = 64ull * 1024 * 1024;  // 64 MiB（要調整）

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<Block> blocks_;  // 全メモリタイプのブロックを一列に持つ（Block::memory_type_index で判別）
};

}  // namespace sq::graphics
