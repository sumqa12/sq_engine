#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/mesh.hpp"
#include "sq/scene/frustum.hpp"  // BoundingSphere（phase13 ⑤）
#include "sq/scene/mesh_handle.hpp"

namespace sq::graphics {

// MeshId → GPU実体（VertexBuffer + IndexBuffer）のキャッシュ（phase12 D-2）。
// 同じジオメトリを何体のエンティティが参照してもGPUバッファは1組で済む。
//
// 起動時に add() で登録し、以後は解放しない（動的アンロードと世代付きハンドルは将来課題）。
// MeshId は entries_ の添字そのもの。
class MeshRegistry {
public:
    // 生成には DEVICE_LOCAL 転送（staging + SingleTimeCommands）が必要なため queue 一式を受ける（phase11 ②）。
    MeshRegistry(GpuAllocator& allocator, VkDevice device,
                 std::uint32_t queue_family, VkQueue queue);
    ~MeshRegistry();

    MeshRegistry(const MeshRegistry&) = delete;
    MeshRegistry& operator=(const MeshRegistry&) = delete;

    // 1つのメッシュが持つGPUバッファと、CPU側で保持する形状情報。
    struct Entry {
        std::unique_ptr<VertexBuffer> vertices;
        std::unique_ptr<IndexBuffer> indices;
        // ローカル空間の境界球（phase13 ⑤）。フラスタムカリングの判定に使う。
        // 頂点はGPUへ送った後CPU側に残らないので、add() の時点で計算しておく必要がある。
        scene::BoundingSphere bounds;
    };

    // 頂点・インデックスをGPUへ登録し、その MeshId を返す。
    [[nodiscard]] scene::MeshId add(const std::vector<Vertex>& vertices,
                                    const std::vector<std::uint16_t>& indices);

    [[nodiscard]] bool contains(scene::MeshId id) const;    // id が有効な登録済みIDか
    [[nodiscard]] const Entry& get(scene::MeshId id) const;  // 範囲外は例外（先に contains で確認する）

private:
    GpuAllocator* allocator_ = nullptr;  // 所有しない（Device が所有）
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::vector<Entry> entries_;  // 添字が MeshId
};

// -- 組み込みジオメトリの生成ヘルパ --
// 将来: add_sphere_mesh / add_capsule_mesh 等を追加する。

// 面ごとに独立した24頂点・36インデックスのキューブ（テクスチャのUVが面ごとに破綻しない構成）。
scene::MeshId add_cube_mesh(MeshRegistry& registry);

// XZ平面上の1x1の板（4頂点・6インデックス）。メッシュ切り替えの動作確認用。
scene::MeshId add_plane_mesh(MeshRegistry& registry);

}  // namespace sq::graphics
