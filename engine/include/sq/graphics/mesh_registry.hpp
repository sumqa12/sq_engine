#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/deletion_queue.hpp"
#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/mesh.hpp"
#include "sq/scene/frustum.hpp"  // BoundingSphere（phase13 ⑤）
#include "sq/scene/mesh_handle.hpp"

namespace sq::graphics {

// MeshId → GPU実体（VertexBuffer + IndexBuffer）のキャッシュ（phase12 D-2）。
// 同じジオメトリを何体のエンティティが参照してもGPUバッファは1組で済む。
//
// phase14 ②: 「登録のみ・解放なし」をやめ、スロット + 世代番号の管理に変えた（D-5）。
// MeshId は { index, generation } で、index が slots_ の添字になる。
class MeshRegistry {
public:
    // 生成には DEVICE_LOCAL 転送（staging + SingleTimeCommands）が必要なため queue 一式を受ける（phase11 ②）。
    // deletions は Renderer が所有する遅延解放キュー（phase14 ②。所有しない）。
    MeshRegistry(GpuAllocator& allocator, VkDevice device,
                 std::uint32_t queue_family, VkQueue queue,
                 DeletionQueue& deletions);
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
    //
    // phase14 ②: スロットの確保手順は共通:
    //   1. free_indices_ が空でなければ末尾から取り出して再利用、空なら slots_ に push_back
    //   2. slot.generation は**インクリメントしない**（上げるのは解放時。ここで上げると
    //      発行した直後のハンドルが自分自身と照合しなくなる）
    //   3. slot.alive = true にして { index, slot.generation } を返す
    [[nodiscard]] scene::MeshId add(const std::vector<Vertex>& vertices,
                                    const std::vector<std::uint16_t>& indices);

    // uint32 インデックス版（phase14 ③-2）。glTF が 65536 頂点を超えるモデルで使う。
    // ★ uint16 版との違いは IndexBuffer のコンストラクタだけ。
    //   境界球の計算・スロット確保・ID の払い出しは完全に共通なので、
    //   共通部分は register_mesh() に括り出して両方から呼ぶこと
    //   （TextureRegistry の load / load_from_pixels と同じ構図）。
    [[nodiscard]] scene::MeshId add(const std::vector<Vertex>& vertices,
                                    const std::vector<std::uint32_t>& indices);

    // id が有効な登録済みIDか。
    // ★ phase14 ②: 添字の範囲だけでなく **generation の一致** も見る。
    //   古いハンドルが再利用されたスロットを指していれば false になる（ABA 問題の検出）。
    [[nodiscard]] bool contains(scene::MeshId id) const;

    [[nodiscard]] const Entry& get(scene::MeshId id) const;  // 範囲外は例外（先に contains で確認する）

    // メッシュを解放する（phase14 ②-3）。
    //
    // 手順:
    //   1. contains(id) でなければ何もしない（二重解放の防止）
    //   2. 実体を**遅延解放キューへ移す**（ここで直接 destroy しない。D-5）
    //      unique_ptr をムーブキャプチャすると std::function に入らないので、
    //      shared_ptr に載せ替えるか専用の型消去を使うこと（deletion_queue.hpp の注記）
    //   3. slot.alive = false; ++slot.generation;   // 以後、古いハンドルは弾かれる
    //   4. free_indices_.push_back(id.index);
    void unload(scene::MeshId id);

private:
    // 1スロット = 「1つの実体を置ける枠」。解放しても枠自体は消さず、
    // generation を上げて再利用する（ecs::Registry の EntityRecord と同じ設計。D-5）。
    struct Slot {
        Entry entry;
        std::uint32_t generation = 0;
        bool alive = false;
    };

    // 完成した Entry をスロットへ収めて MeshId を払い出す（phase14 ③-2）。
    // add() の2つのオーバーロードが共有する後半部分。
    [[nodiscard]] scene::MeshId register_mesh(Entry entry);

    // 頂点列からローカル空間の境界球を求める（phase13 ⑤）。
    // 頂点は GPU へ送った後 CPU 側に残らないので、add() の時点で計算しておく必要がある。
    [[nodiscard]] static scene::BoundingSphere compute_bounds(const std::vector<Vertex>& vertices);

    GpuAllocator* allocator_ = nullptr;  // 所有しない（Device が所有）
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    DeletionQueue* deletions_ = nullptr;  // 所有しない（Renderer が所有）

    std::vector<Slot> slots_;                    // 添字が MeshId::index
    std::vector<std::uint32_t> free_indices_;    // 解放済みスロットの再利用リスト
};

// -- 組み込みジオメトリの生成ヘルパ --
// 将来: add_sphere_mesh / add_capsule_mesh 等を追加する。

// 面ごとに独立した24頂点・36インデックスのキューブ（テクスチャのUVが面ごとに破綻しない構成）。
scene::MeshId add_cube_mesh(MeshRegistry& registry);

// XZ平面上の1x1の板（4頂点・6インデックス）。メッシュ切り替えの動作確認用。
scene::MeshId add_plane_mesh(MeshRegistry& registry);

}  // namespace sq::graphics
