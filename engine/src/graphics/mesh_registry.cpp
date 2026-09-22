#include "sq/graphics/mesh_registry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <glm/gtc/constants.hpp>

namespace sq::graphics {

MeshRegistry::MeshRegistry(GpuAllocator& allocator, VkDevice device,
                           std::uint32_t queue_family, VkQueue queue,
                           DeletionQueue& deletions)
    : allocator_(&allocator), device_(device), queue_family_(queue_family), queue_(queue),
      deletions_(&deletions) {
    // 生成時は空。add() で登録していく。
}

MeshRegistry::~MeshRegistry() {
    // slots_ の unique_ptr が VertexBuffer / IndexBuffer を破棄する（= GpuAllocator へ free）。
    // 前提: このデストラクタは GpuAllocator（Device）より前に走ること（Renderer が破棄順序を担保）。
    //
    // ★ 遅延解放キューに積まれたまま残っている実体はここでは触らない。
    //   Renderer が vkDeviceWaitIdle → DeletionQueue::flush_all() を先に済ませている前提。
    slots_.clear();
    free_indices_.clear();
}

scene::MeshId MeshRegistry::add(const std::vector<Vertex>& vertices,
                                const std::vector<std::uint16_t>& indices) {
    //   1. 空のメッシュ（vertices か indices が空）は弾く。
    if (vertices.empty() || indices.empty()) {
        throw std::runtime_error("MeshRegistry::add : 頂点またはインデックスが空です。");
    }

    //   2. Entry を作って push する:
    Entry entry;
    entry.vertices = std::make_unique<VertexBuffer>(*allocator_, device_, queue_family_, queue_, vertices);
    entry.indices  = std::make_unique<IndexBuffer>(*allocator_, device_, queue_family_, queue_, indices);

    //   3. ローカル空間の境界球を計算して entry.bounds に入れる。
    entry.bounds = compute_bounds(vertices);

    // push_back をスロット確保に置き換える。
    return register_mesh(std::move(entry));
}

scene::MeshId MeshRegistry::add(const std::vector<Vertex>& vertices,
                                const std::vector<std::uint32_t>& indices) {
    //   1. 空のメッシュ（vertices か indices が空）は弾く。
    if (vertices.empty() || indices.empty()) {
        throw std::runtime_error("MeshRegistry::add : 頂点またはインデックスが空です。");
    }

    //   2. Entry を作って push する。
    Entry entry;
    entry.vertices = std::make_unique<VertexBuffer>(*allocator_, device_, queue_family_, queue_, vertices);
    entry.indices  = std::make_unique<IndexBuffer>(*allocator_, device_, queue_family_, queue_, indices);

    //   3. ローカル空間の境界球を計算して bounds に入れる。
    entry.bounds = compute_bounds(vertices);

    return register_mesh(std::move(entry));
}

scene::BoundingSphere MeshRegistry::compute_bounds(const std::vector<Vertex>& vertices) {
    //   1. 全頂点の position から AABB（各成分の最小・最大）を求める
    scene::BoundingSphere bounds{};
    glm::vec3 min = vertices[0].position;
    glm::vec3 max = vertices[0].position;
    for (const auto& vertex : vertices) {
        min = glm::min(min, vertex.position);
        max = glm::max(max, vertex.position);
    }
    //   2. 中心を求める
    bounds.center = (min + max) * 0.5f;

    //   3. entry.bounds.radius = 全頂点について max(length(v.position - center))
    //      （AABB の対角長 / 2 でも正しいが、頂点から直接求めた方が球が締まる＝無駄な描画が減る）
    float radius = 0;
    for (const auto& vertex : vertices) {
        float dist = glm::length(vertex.position - bounds.center);
        radius = glm::max(radius, dist);
    }
    bounds.radius = radius;
    return bounds;
}

scene::MeshId MeshRegistry::register_mesh(Entry entry) {
    std::uint32_t index;
    if (free_indices_.empty()) {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    } else {
        index = free_indices_.back(); // 解放済みスロットを再利用
        free_indices_.pop_back();
    }

    Slot& slot = slots_[index];
    slot.entry = std::move(entry);
    slot.alive = true;
    //   ★ MeshRegistry には登録数の上限が無い（テクスチャと違ってディスクリプタ枠を
    //     消費しないため）。TextureRegistry / MaterialRegistry のような上限チェックは要らない。
    return scene::MeshId{ { index, slot.generation } };
}

bool MeshRegistry::contains(scene::MeshId id) const {
    // 世代照合込みの判定にする。
    return id.index < slots_.size()
        && slots_[id.index].alive
        && slots_[id.index].generation == id.generation;
    //
    //   ★ 3条件すべてが要る。alive を落とすと「解放済みスロットを指す古いハンドル」が、
    //     generation を落とすと「再利用されたスロットを指す古いハンドル」が素通りする。
    //   ★ kInvalidMeshId は index == kInvalidIndex なので第1条件で弾ける。
}

const MeshRegistry::Entry& MeshRegistry::get(scene::MeshId id) const {
    // contains(id) でなければ throw、そうでなければ slots_[id.index].entry を返す。
    if (contains(id)) {
        return slots_[id.index].entry;
    }

    throw std::runtime_error("MeshRegistry::get : メッシュが登録されていません。");
}

void MeshRegistry::unload(scene::MeshId id) {
    //   1. 二重解放の防止
    if (!contains(id)) { return; }

    //   2. 実体を遅延解放キューへ移す。★ ここで直接 destroy してはいけない（D-5）
    Slot& slot = slots_[id.index];
    auto vertices = std::shared_ptr(std::move(slot.entry.vertices));
    auto indices  = std::shared_ptr(std::move(slot.entry.indices));
    deletions_->push([vertices, indices]() mutable {
        vertices.reset();
        indices.reset();
    });
    //      （unique_ptr のままムーブキャプチャすると std::function に入らない。
    //        shared_ptr へ載せ替えるのが一番簡単な回避策）

    slot.alive = false;
    ++slot.generation;      // 以後、古いハンドルは弾かれる
    free_indices_.push_back(id.index);
    //   ★ generation のオーバーフローは実用上は起きない（uint32 = 42億回の解放が必要）。
    //     気になるなら「上限に達したスロットは free_indices_ へ戻さない」で封じられる。
}

// ----- 組み込みジオメトリ -----
//
// 巻き順を CCW へ統一する。
//   graphics_pipeline.cpp の frontFace を COUNTER_CLOCKWISE に変えるのに合わせて、
//   ここの**インデックス順を反転**させること（三角形ごとに 2 番目と 3 番目を入れ替える）。
//   ★ 頂点配列ではなくインデックス配列を触る。頂点を並べ替えると UV も追随させる必要が出る。
//
// Vertex の color を normal に置き換えたので、
//   下の頂点データの第2要素を「面ごとの法線」に書き換えること。
//   （キューブは面ごとに独立した24頂点構成なので、面の法線をそのまま入れられる。
//     この構成にしておいたのが phase8 の投資として効いてくる）

scene::MeshId add_cube_mesh(MeshRegistry& registry) {
    // テクスチャを貼るため、面ごとに独立した24頂点構成にする（phase8プラン 項目9 から移設）。
    // 各面の4頂点に uv = {1,0}/{1,1}/{0,1}/{0,0} を割り当て、インデックスは面ごと6個×6面=36個。
    // 左- 右+ 上- 下+ 手前- 奥+
    const std::vector<Vertex> vertices = {
        // 手前
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1, 0}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1, 1}},
        {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0, 1}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0, 0}},

        // 奥
        {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1, 0}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1, 1}},
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0, 1}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0, 0}},

        // 左
        {{-0.5f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1, 0}},
        {{-0.5f, -0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1, 1}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0, 1}},
        {{-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0, 0}},

        // 右
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1, 0}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1, 1}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 1}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0, 0}},

        // 上
        {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 1}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0, 1}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0, 0}},

        // 下
        {{-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {1, 0}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {1, 1}},
        {{0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0, 1}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0, 0}},
    };

    const std::vector<std::uint16_t> indices = {
         0,  2,  1,  0,  3,  2, // 手前
         4,  6,  5,  4,  7,  6, // 奥
         8, 10,  9,  8, 11, 10, // 左
        12, 14, 13, 12, 15, 14, // 右
        16, 18, 17, 16, 19, 18, // 上
        20, 22, 21, 20, 23, 22  // 下
    };

    return registry.add(vertices, indices);
}

scene::MeshId add_plane_mesh(MeshRegistry& registry) {
    // XZ平面上の1x1の板。キューブとの見た目の差でメッシュ切り替えを確認するためのもの。
    const std::vector<Vertex> vertices = {
        {{-0.5f, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1, 0}},
        {{-0.5f, 0.0f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1, 1}},
        {{0.5f, 0.0f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0, 1}},
        {{0.5f, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, {0, 0}},
    };

    const std::vector<std::uint16_t> indices = {
        0, 2, 1, 0, 3, 2
    };

    return registry.add(vertices, indices);
}


scene::MeshId add_sphere_mesh(MeshRegistry& registry, int segments, int rings) {
    // ★ 分割数が少なすぎると面が張れない。最低限を保証しておく。
    segments = std::max(segments, 3);
    rings    = std::max(rings, 2);

    // 経度方向は「継ぎ目の頂点を重複させる」ため +1 列にする。
    //   phi = 0 と phi = 2*PI は同じ位置だが u が 0 と 1 で異なるため、
    //   頂点を共有すると継ぎ目で u が 1 → 0 へ巻き戻り、帯状の歪みが出る。
    const int columns = segments + 1;

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(rings + 1) * static_cast<std::size_t>(columns));

    for (int ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float theta = glm::pi<float>() * v;  // 0 = 北極, PI = 南極
        const float sin_theta = std::sin(theta);
        const float cos_theta = std::cos(theta);

        for (int seg = 0; seg <= segments; ++seg) {
            const float u = static_cast<float>(seg) / static_cast<float>(segments);
            const float phi = glm::two_pi<float>() * u;

            // 半径 0.5（cube の1辺 1.0 と大きさの感覚を揃える）。
            const glm::vec3 position = 0.5f * glm::vec3(
                sin_theta * std::cos(phi),
                cos_theta,
                sin_theta * std::sin(phi));

            vertices.push_back(Vertex{
                .position = position,
                // ★ 球は「原点からの向き」がそのまま法線になる。極でも position は
                //   (0, ±0.5, 0) で長さが 0 にならないので normalize は安全。
                .normal   = glm::normalize(position),
                .uv       = glm::vec2(u, v),
            });
        }
    }

    std::vector<std::uint16_t> indices;
    indices.reserve(static_cast<std::size_t>(rings) * static_cast<std::size_t>(segments) * 6);

    for (int ring = 0; ring < rings; ++ring) {
        for (int seg = 0; seg < segments; ++seg) {
            const auto a = static_cast<std::uint16_t>(ring * columns + seg);
            const auto b = static_cast<std::uint16_t>(a + 1);        // 経度方向の隣（phi + d）
            const auto c = static_cast<std::uint16_t>(a + columns);  // 緯度方向の隣（theta + d）
            const auto d = static_cast<std::uint16_t>(c + 1);

            // 外から見て反時計回りになる並び（frontFace = COUNTER_CLOCKWISE）。
            //   赤道上の点で確かめると (b - a) × (c - a) が中心から外を向く:
            //     b - a は +phi 方向、c - a は -y 方向（theta が増えると下がる）
            //   ★ 逆順にすると球が裏返り、背面カリングで**丸ごと消える**。
            // ★ 極（ring = 0 と ring = rings-1）では a と b（または c と d）が同じ位置に
            //   重なって三角形が縮退するが、面積0はラスタライズされないので実害は無い。
            //   極だけ三角形1枚にする最適化もあるが、条件分岐が増えるわりに得るものが少ない。
            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            indices.push_back(b); indices.push_back(d); indices.push_back(c);
        }
    }

    return registry.add(vertices, indices);
}

}  // namespace sq::graphics
