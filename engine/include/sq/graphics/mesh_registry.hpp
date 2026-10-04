#pragma once

#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/asset_ref.hpp"
#include "sq/graphics/asset_registry.hpp"
#include "sq/graphics/deletion_queue.hpp"
#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/mesh.hpp"
#include "sq/scene/frustum.hpp"  // BoundingSphere（phase13 ⑤）
#include "sq/scene/mesh_handle.hpp"

namespace sq::graphics {

// 1つのメッシュが持つGPUバッファと、CPU側で保持する形状情報。
//
// ★ phase16 ③-3: MeshRegistry::Entry から名前空間スコープへ出した。
//   派生クラスは自分の入れ子型を基底のテンプレート引数に使えない
//   （基底を決める時点では、まだ MeshRegistry の中身が見えていないため）。
//   既存の呼び出し側（renderer.cpp の MeshRegistry::Entry）は、
//   クラス内の using Entry = MeshEntry; でそのまま通る。
struct MeshEntry {
    std::unique_ptr<VertexBuffer> vertices;
    std::unique_ptr<IndexBuffer> indices;
    // ローカル空間の境界球（phase13 ⑤）。フラスタムカリングの判定に使う。
    // 頂点はGPUへ送った後CPU側に残らないので、add() の時点で計算しておく必要がある。
    scene::BoundingSphere bounds;
};

// MeshId → GPU実体（VertexBuffer + IndexBuffer）のキャッシュ（phase12 D-2）。
// 同じジオメトリを何体のエンティティが参照してもGPUバッファは1組で済む。
//
// phase14 ②: 「登録のみ・解放なし」をやめ、スロット + 世代番号の管理に変えた（D-5）。
// MeshId は { index, generation } で、index がスロットの添字になる。
//
// phase16 ③-3: スロット管理（slots_ / free_indices_ / contains / get）を
// AssetRegistry へ括り出した。ここに残るのは「バッファを作る」と「解放時に遅延キューへ積む」だけ。
// 上限は無いので基底へは capacity = 0（無制限）を渡す。
class MeshRegistry final : public AssetRegistry<scene::MeshId, MeshEntry> {
public:
    using Entry = MeshEntry;

    // 生成には DEVICE_LOCAL 転送（staging + SingleTimeCommands）が必要なため queue 一式を受ける（phase11 ②）。
    // deletions は Renderer が所有する遅延解放キュー（phase14 ②。所有しない）。
    MeshRegistry(GpuAllocator& allocator, VkDevice device,
                 std::uint32_t queue_family, VkQueue queue,
                 DeletionQueue& deletions);
    ~MeshRegistry() override = default;

    MeshRegistry(const MeshRegistry&) = delete;
    MeshRegistry& operator=(const MeshRegistry&) = delete;

    // 頂点・インデックスをGPUへ登録し、その MeshId を返す。
    //
    // ★ phase16 ③-3: スロットの確保は基底の emplace(std::move(entry)) に任せる。
    //   register_mesh() は emplace そのものになったので削除した。
    [[nodiscard]] scene::MeshId add(const std::vector<Vertex>& vertices,
                                    const std::vector<std::uint16_t>& indices);

    // uint32 インデックス版（phase14 ③-2）。glTF が 65536 頂点を超えるモデルで使う。
    // ★ uint16 版との違いは IndexBuffer のコンストラクタだけ。
    //   境界球の計算・スロット確保・ID の払い出しは完全に共通なので、
    //   共通部分は register_mesh() に括り出して両方から呼ぶこと
    //   （TextureRegistry の load / load_from_pixels と同じ構図）。
    [[nodiscard]] scene::MeshId add(const std::vector<Vertex>& vertices,
                                    const std::vector<std::uint32_t>& indices);

    // ★ phase16 ③-3: contains() / get() は基底（AssetRegistry）へ移した。
    //   mesh_registry.cpp の MeshRegistry::contains / MeshRegistry::get の定義は削除すること。

    // メッシュを解放する（phase14 ②-3）。
    //
    // phase16 ③-3: 中身は基底の destroy(id) を呼ぶだけになる。
    //   二重解放の防止・alive / generation / free_indices_ の更新は destroy() が行い、
    //   「実体を遅延解放キューへ移す」部分だけが on_destroy() へ移る。
    void unload(scene::MeshId id);

protected:
    // 解放時のフック（phase16 ③-3）。既存の unload() の手順2をそのまま移す:
    //   entry.vertices / entry.indices を shared_ptr へ載せ替えて deletions_->push(...)
    //   （unique_ptr のままムーブキャプチャすると std::function に入らない。
    //     deletion_queue.hpp の注記 (a) の方法）
    // ★ ここで直接 destroy してはいけない（GPU がまだ読んでいる可能性がある。phase14 D-5）。
    void on_destroy(scene::MeshId id, MeshEntry&& entry) override;

private:

    // 頂点列からローカル空間の境界球を求める（phase13 ⑤）。
    // 頂点は GPU へ送った後 CPU 側に残らないので、add() の時点で計算しておく必要がある。
    [[nodiscard]] static scene::BoundingSphere compute_bounds(const std::vector<Vertex>& vertices);

    GpuAllocator* allocator_ = nullptr;  // 所有しない（Device が所有）
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    DeletionQueue* deletions_ = nullptr;  // 所有しない（Renderer が所有）

    // ★ phase16 ③-3: slots_ / free_indices_ は基底（AssetRegistry）へ移した。
};

// メッシュへの所有参照（phase16 手順8 / ③-4）。assets::LoadedModel が持つ。
using MeshRef = AssetRef<MeshRegistry, scene::MeshId>;

// -- 組み込みジオメトリの生成ヘルパ --
// 将来: add_capsule_mesh 等を追加する。

// 面ごとに独立した24頂点・36インデックスのキューブ（テクスチャのUVが面ごとに破綻しない構成）。
scene::MeshId add_cube_mesh(MeshRegistry& registry);

// XZ平面上の1x1の板（4頂点・6インデックス）。メッシュ切り替えの動作確認用。
scene::MeshId add_plane_mesh(MeshRegistry& registry);

// UV 球（phase15 ②）。緯度・経度で分割した球で、**位置がそのまま法線になる**のが特徴。
//
// なぜ立方体では足りないのか:
//   立方体は法線が6方向しか無いため、roughness を変えてもハイライトは
//   「その面に載るか載らないか」の二択になり、**粗さの連続的な変化が読めない**。
//   球なら全方向の法線が揃うので、ハイライトの広がり方とフレネル（縁の明るさ）を
//   1体の上で同時に観察できる。②のマテリアルボール検証はこれを前提にしている。
//
// segments = 経度方向の分割数、rings = 緯度方向の分割数。
// ★ 頂点数が 65536 を超えると uint16 のインデックスでは足りなくなる（phase14 ③ の uint32 版が要る）。
//   既定の 32 × 16 なら 33 × 17 = 561 頂点なので問題ない。
scene::MeshId add_sphere_mesh(MeshRegistry& registry, int segments = 32, int rings = 16);

}  // namespace sq::graphics
