#include "sq/graphics/mesh_registry.hpp"

#include <stdexcept>

namespace sq::graphics {

MeshRegistry::MeshRegistry(GpuAllocator& allocator, VkDevice device,
                           std::uint32_t queue_family, VkQueue queue)
    : allocator_(&allocator), device_(device), queue_family_(queue_family), queue_(queue) {
    // 生成時は空。add() で登録していく。
}

MeshRegistry::~MeshRegistry() {
    // entries_ の unique_ptr が VertexBuffer / IndexBuffer を破棄する（= GpuAllocator へ free）。
    // 前提: このデストラクタは GpuAllocator（Device）より前に走ること（Renderer が破棄順序を担保）。
    entries_.clear();
}

scene::MeshId MeshRegistry::add(const std::vector<Vertex>& vertices,
                                const std::vector<std::uint16_t>& indices) {
    // （phase12 手順2）:
    //   1. 空のメッシュ（vertices か indices が空）は弾く。
    if (vertices.empty() || indices.empty()) {
        throw std::runtime_error("MeshRegistry::add : 頂点またはインデックスが空です。");
    }

    //   2. Entry を作って push する:
    Entry entry;
    entry.vertices = std::make_unique<VertexBuffer>(*allocator_, device_, queue_family_, queue_, vertices);
    entry.indices  = std::make_unique<IndexBuffer>(*allocator_, device_, queue_family_, queue_, indices);

    // ローカル空間の境界球を計算して entry.bounds に入れる。
    //   1. 全頂点の position から AABB（各成分の最小・最大）を求める
    glm::vec3 min = vertices[0].position;
    glm::vec3 max = vertices[0].position;
    for (const auto& vertex : vertices) {
        min = glm::min(min, vertex.position);
        max = glm::max(max, vertex.position);
    }
    //   2. 中心を求める
    entry.bounds.center = (min + max) * 0.5f;

    //   3. entry.bounds.radius = 全頂点について max(length(v.position - center))
    //      （AABB の対角長 / 2 でも正しいが、頂点から直接求めた方が球が締まる＝無駄な描画が減る）
    float radius = 0;
    for (const auto& vertex : vertices) {
        float dist = glm::length(vertex.position - entry.bounds.center);
        radius = glm::max(radius, dist);
    }
    entry.bounds.radius = radius;

    entries_.push_back(std::move(entry));

    //   3. 添字を MeshId として返す:
    return static_cast<scene::MeshId>(entries_.size() - 1);
}

bool MeshRegistry::contains(scene::MeshId id) const {
    return id < entries_.size();
    // kInvalidMeshId = ~0u なので、この比較だけで無効IDも弾ける
}

const MeshRegistry::Entry& MeshRegistry::get(scene::MeshId id) const {
    // contains(id) でなければ throw、そうでなければ entries_[id] を返す。
    if (contains(id)) {
        return entries_[id];
    }

    throw std::runtime_error("MeshRegistry::get : メッシュが登録されていません。");
}

// ----- 組み込みジオメトリ -----

scene::MeshId add_cube_mesh(MeshRegistry& registry) {
    // テクスチャを貼るため、面ごとに独立した24頂点構成にする（phase8プラン 項目9 から移設）。
    // 各面の4頂点に uv = {0,0}/{1,0}/{1,1}/{0,1} を割り当て、インデックスは面ごと6個×6面=36個。
    // 左- 右+ 上- 下+ 手前- 奥+
    const std::vector<Vertex> vertices = {
        // 手前
        {{-0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
        {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

        // 奥
        {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

        // 左
        {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
        {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

        // 右
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

        // 上
        {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
        {{-0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},

        // 下
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},
    };

    const std::vector<std::uint16_t> indices = {
         0,  1,  2,  0,  2,  3, // 手前
         4,  5,  6,  4,  6,  7, // 奥
         8,  9, 10,  8, 10, 11, // 右
        12, 13, 14, 12, 14, 15, // 左
        16, 17, 18, 16, 18, 19, // 上
        20, 21, 22, 20, 22, 23  // 下
    };

    return registry.add(vertices, indices);
}

scene::MeshId add_plane_mesh(MeshRegistry& registry) {
    // XZ平面上の1x1の板。キューブとの見た目の差でメッシュ切り替えを確認するためのもの。
    // 巻き順はキューブの「上」面に合わせる（cullMode=BACK / frontFace=CLOCKWISE のため）。
    const std::vector<Vertex> vertices = {
        {{-0.5f, 0.0f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0, 0}},
        {{-0.5f, 0.0f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0, 1}},
        {{0.5f, 0.0f, -0.5f}, {1.0f, 1.0f, 1.0f}, {1, 1}},
        {{0.5f, 0.0f, 0.5f}, {1.0f, 1.0f, 1.0f}, {1, 0}},
    };

    const std::vector<std::uint16_t> indices = {
        0, 1, 2, 0, 2, 3
    };

    return registry.add(vertices, indices);
}

}  // namespace sq::graphics
