# C++ ゲームエンジン（学習目的）— Phase 12: 描画データのコンポーネント化（エンティティごとの見た目）

## Context
Phase 11 で描画側の最適化（頂点/インデックスの DEVICE_LOCAL 化・自前サブアロケータ・不透明/半透明の 2 パス分離）まで到達した（[phase11-device-local-suballocator-transparency.md](phase11-device-local-suballocator-transparency.md) 参照）。

しかし現状、**全エンティティの見た目が完全に同一**である:

- **メッシュが 1 つ共有**: [renderer.cpp](../../engine/src/graphics/renderer.cpp) の `triangle_mesh_` / `cube_indices_` を全エンティティが使い、`vkCmdDrawIndexed(cube_indices_->index_count(), ...)` で同じ cube を描いている。
- **テクスチャが 1 枚共有**: `texture_` 1 枚を `descriptor_sets_[frame]`（`set=0, binding=1`）に焼き込んでいるため、エンティティ単位で差し替える余地がない。
- **per-entity なのは `Transform`/`Position` だけ**、しかも [renderer.cpp:224](../../engine/src/graphics/renderer.cpp:224) が `glm::translate` で平行移動のみ合成しており、**回転・スケールが表現できない**（[Transform](../../engine/include/sq/scene/transform.hpp) は合成済み `model` を持つだけ）。
- `Material` は Phase 11 で新設したが、まだ `bool transparent` のみ（[material.hpp](../../engine/include/sq/scene/material.hpp)）。

Phase 12 では「**描画に必要なデータをコンポーネント化し、エンティティごとに見た目を変えられる**」状態にする。

例によって Claude は宣言・骨格・TODO コメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### D-1. コンポーネントは「軽量ハンドル」、GPU実体はレジストリが所有【最重要】
コンポーネントに `VkBuffer` や `Texture` の**実体を持たせてはいけない**。理由:

- **重複確保**: 100 体が同じ cube を指しても GPU バッファは 1 本で済ませたい。実体を持つと 100 本確保してしまう。
- **ECS の構造と衝突**: 本エンジンの ECS はアーキタイプ／SoA で、`add<T>` 時に `T` を**値としてストレージへ move**する（[registry.hpp:44](../../engine/include/sq/ecs/registry.hpp:44)）。`Texture` / `Buffer` は**コピー禁止の RAII 型**なので、コンポーネントに入れるとアーキタイプ間移動で壊れる。
- コンポーネントは小さく値型（trivially copyable 相当）に保つのが SoA と相性が良い。

したがって:

| 層 | 役割 | 例 |
|---|---|---|
| **コンポーネント** | ID / 小さな値のみ | `MeshHandle{ MeshId }`, `Material{ TextureId, vec4, bool }` |
| **レジストリ（キャッシュ）** | GPU実体を所有し、ID から引く | `MeshRegistry`, `TextureRegistry` |

ID は単なる `std::uint32_t` の別名。**世代管理（generation）は付けない**（アセットは起動時に登録して破棄しない前提。動的アンロードは将来課題）。

### D-2. `MeshHandle` + `MeshRegistry`
- `MeshHandle{ MeshId id }`: どのジオメトリを描くか。
- `MeshRegistry`: `MeshId → { VertexBuffer, IndexBuffer }` を所有。描画時に `index_count()` をメッシュごとに引く（**現状の `cube_indices_->index_count()` 固定をやめる**）。
- `MeshHandle` を持たないエンティティは**描画しない**（スキップ）。カメラなど非描画エンティティが `Transform` を持ちうるため、「`MeshHandle` の有無」を描画対象の判定基準にする。

### D-3. `Material` の拡張 + `TextureRegistry`
```
Material { TextureId albedo; glm::vec4 base_color; bool transparent; }
```
- `albedo`: どの画像を貼るか。`TextureRegistry` が `TextureId → { Texture, VkDescriptorSet }` を所有。
- `base_color`: テクスチャに乗算する色 tint。**push constant で渡す**（小さいので UBO 不要）。`base_color.a < 1.0` で半透明の強さを表現でき、Phase 11 ① の半透明パスがアルファ付き PNG 無しでも検証できるようになる。
- `transparent`: Phase 11 から継続。不透明/半透明パイプラインの選択に使う。
- **`Material` を持たないエンティティは既定マテリアル扱い**（`albedo = 既定テクスチャ`, `base_color = white`, `transparent = false`）。後方互換。

### D-4. `Transform` を TRS 化し、`Position` を吸収する
```
Transform { glm::vec3 position; glm::quat rotation; glm::vec3 scale; }  →  model() で T*R*S を合成
```
- 現状 `Transform{ glm::mat4 model }` ＋ 別コンポーネント `Position` の二重管理で、しかも renderer が `Position` から `translate` だけを作って `Transform::model` を**毎フレーム上書き**している。回転・スケールを入れる余地がない。
- TRS を持たせ、`model()` で合成する。renderer は `view<Transform, MeshHandle>` を回すだけになり、`Position` を参照しなくなる。
- **回転はクォータニオン**（`glm::quat`）にする。オイラー角のジンバル問題を避け、Phase 10 の `Controller`（yaw/pitch）とは役割が別（あちらはカメラ操作の入力状態）。
- `Position` / `Velocity` は現状**どのシステムも更新していない**（`Velocity` は完全に未使用、`Position` は renderer が読むだけ）。描画からは切り離すが、ファイル自体は将来の物理/移動システム用に残してよい。
- 行列キャッシュ（dirty フラグ付き）は**将来課題**。まずは毎フレーム合成する。

### D-5. ディスクリプタセットを 2 つに分離する（set=0 カメラ / set=1 マテリアル）
ここが本フェーズの実作業の山。現状は `set=0` に `binding=0`（カメラUBO, VERTEX）と `binding=1`（combined image sampler, FRAGMENT）が同居し、フレームごとに 1 セットへ**固定テクスチャを焼き込んでいる**（[renderer.cpp](../../engine/src/graphics/renderer.cpp) `create_descriptor_sets`）。

これを分離する:

| セット | 内容 | 個数 | 更新頻度 |
|---|---|---|---|
| `set=0` | カメラUBO (`binding=0`, VERTEX) | `kFramesInFlight` 個 | フレームごとに 1 回バインド |
| `set=1` | combined image sampler (`binding=0`, FRAGMENT) | **テクスチャごとに 1 個** | マテリアルが変わるたびバインド |

- テクスチャは起動時に確定し以後不変なので、`set=1` は**フレーム数と無関係にテクスチャごと 1 個**でよい（フレームごとに複製する必要はない）。
- bindless（`VK_EXT_descriptor_indexing` でテクスチャ配列＋インデックスを push constant で渡す）は**将来課題**。学習段階ではセット分離のほうが Vulkan のディスクリプタモデルを理解しやすい。
- `VkPipelineLayout` は 2 つのセットレイアウトから作る。不透明/半透明の 2 本のパイプラインは**同じレイアウト**を使う（Phase 11 の前提を維持）。

### D-6. push constant に `base_color` を追加（VERTEX | FRAGMENT）
```
struct PushConstants { glm::mat4 model; glm::vec4 base_color; }   // 64 + 16 = 80 bytes
```
- 現状は `mat4 model`（64 バイト, VERTEX のみ）。`base_color` はフラグメントで使うため、**range のステージを `VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT` にする**。
- 80 バイトは `maxPushConstantsSize` の**最小保証 128 バイト**に収まる。これ以上マテリアル属性を増やすならマテリアル UBO へ移す（将来課題）。
- **シェーダ側の push constant ブロック定義を vert/frag で完全に一致させる**こと（メンバ順・型を揃える。片方で使わなくても宣言は同じにする）。

### D-7. `draw_frame` は「収集 → ソート → バッチ記録」に再構成
Phase 11 で「不透明パス → 半透明ソート → 半透明パス」にしたが、per-entity メッシュ/テクスチャが入ると各アイテムが情報を持つ必要がある。

- `DrawItem` に `model` / `base_color` / `mesh` / `texture` / `distance_sq` を持たせ、**不透明・半透明の 2 つの vector に収集**する。
- **不透明**: `(texture, mesh)` でソートしてから記録する（同じテクスチャ・メッシュが連続すれば再バインドを省ける）。深度テストが前後関係を解決するので順序は自由。
- **半透明**: `distance_sq` 降順（back-to-front）。**正しさが順序に依存するので、テクスチャでのバッチ化はできない**（ソート順が絶対）。
- 記録は共通ヘルパ `record_draw_items()` に集約し、**直前にバインドした mesh / texture を覚えて変化時のみ再バインド**する（冗長な `vkCmdBindVertexBuffers` / `vkCmdBindDescriptorSets` を避ける）。

---

## 1. コンポーネント（`engine/include/sq/scene/`）

### 1-1. `mesh_handle.hpp`（新規）

```cpp
#pragma once

#include <cstdint>

namespace sq::scene {

// メッシュ（ジオメトリ）の識別子。graphics::MeshRegistry が実体を所有する。
// 実体（VertexBuffer / IndexBuffer）はコピー禁止のRAII型なのでコンポーネントには入れない（phase12 D-1）。
using MeshId = std::uint32_t;
inline constexpr MeshId kInvalidMeshId = ~0u;

// このエンティティが描画するジオメトリ（ECSコンポーネント）。
// MeshHandle を持たないエンティティは描画されない（カメラ等の非描画エンティティを除外する判定に使う）。
struct MeshHandle {
    MeshId id = kInvalidMeshId;
};

}  // namespace sq::scene
```

### 1-2. `material.hpp`（既存を拡張）

```cpp
// 既存の material.hpp を以下に差し替える。
#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace sq::scene {

// テクスチャの識別子。graphics::TextureRegistry が実体（Texture + VkDescriptorSet）を所有する。
using TextureId = std::uint32_t;
inline constexpr TextureId kInvalidTextureId = ~0u;

// 描画マテリアル（ECSコンポーネント）。
// Material を持たないエンティティは既定マテリアル（既定テクスチャ・白・不透明）として扱う（後方互換）。
struct Material {
    TextureId albedo = kInvalidTextureId;   // 貼るテクスチャ。kInvalidTextureId なら既定テクスチャ
    glm::vec4 base_color{1.0f};             // テクスチャに乗算する色 tint（a < 1 で半透明の強さ）
    bool transparent = false;               // true なら半透明パスで back-to-front 描画（phase11 ①）

    // 将来: float alpha_cutoff / bool double_sided / vec3 emissive / metallic・roughness
};

}  // namespace sq::scene
```

### 1-3. `transform.hpp`（TRS 化。既存を差し替え）

```cpp
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sq::scene {

// エンティティのワールド変換（ECSコンポーネント）。
// phase12 D-4: 合成済み mat4 を保持する形から TRS（平行移動・回転・スケール）を持つ形へ変更した。
// 従来の Position コンポーネントの役割は position が引き継ぐ（renderer は Position を参照しなくなる）。
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // 単位クォータニオン（w, x, y, z）
    glm::vec3 scale{1.0f};

    // T * R * S を合成したモデル行列を返す。
    // 実装: glm::translate(mat4(1), position) * glm::mat4_cast(rotation) * glm::scale(mat4(1), scale)
    // （行列キャッシュ + dirty フラグは将来課題。まずは毎回合成する）
    [[nodiscard]] glm::mat4 model() const;
};

}  // namespace sq::scene
```

> `model()` の実装は新規 `engine/src/scene/transform.cpp` に置き、CMakeLists へ追加する
> （ヘッダ inline でもよいが、`glm/gtc/matrix_transform.hpp` の include をヘッダに広げないため .cpp 推奨）。

---

## 2. レジストリ（`engine/include/sq/graphics/`）

### 2-1. `mesh_registry.hpp`（新規）

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/mesh.hpp"
#include "sq/scene/mesh_handle.hpp"

namespace sq::graphics {

// MeshId → GPU実体（VertexBuffer + IndexBuffer）のキャッシュ（phase12 D-2）。
// 同じジオメトリを何体が参照してもGPUバッファは1組で済む。
// 起動時に add() で登録し、以後は解放しない（動的アンロードは将来課題）。
class MeshRegistry {
public:
    // 生成には DEVICE_LOCAL 転送（staging + SingleTimeCommands）が必要なため queue 一式を受ける（phase11 ②）。
    MeshRegistry(GpuAllocator& allocator, VkDevice device,
                 std::uint32_t queue_family, VkQueue queue);
    ~MeshRegistry();

    MeshRegistry(const MeshRegistry&) = delete;
    MeshRegistry& operator=(const MeshRegistry&) = delete;

    // 1つのメッシュを登録し、その MeshId を返す。
    // 手順: entries_.push_back({ make_unique<VertexBuffer>(...), make_unique<IndexBuffer>(...) });
    //       return static_cast<scene::MeshId>(entries_.size() - 1);
    scene::MeshId add(const std::vector<Vertex>& vertices,
                      const std::vector<std::uint16_t>& indices);

    // 1つのメッシュが持つGPUバッファ。
    struct Entry {
        std::unique_ptr<VertexBuffer> vertices;
        std::unique_ptr<IndexBuffer> indices;
    };

    [[nodiscard]] bool contains(scene::MeshId id) const;  // id < entries_.size()
    [[nodiscard]] const Entry& get(scene::MeshId id) const;  // 範囲外は例外（呼ぶ前に contains で確認）

private:
    GpuAllocator* allocator_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::vector<Entry> entries_;  // 添字が MeshId
};

// 組み込みジオメトリの生成ヘルパ（renderer.cpp の create_cube_mesh の頂点データを移設する）。
// TODO: 現状 Renderer::create_cube_mesh にべた書きされている 24 頂点 / 36 インデックスをここへ移す。
//       将来: 球・平面・カプセル等を追加する。
scene::MeshId add_cube_mesh(MeshRegistry& registry);

}  // namespace sq::graphics
```

### 2-2. `texture_registry.hpp`（新規）

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/texture.hpp"
#include "sq/scene/material.hpp"

namespace sq::graphics {

// TextureId → GPU実体（Texture + そのテクスチャ専用の VkDescriptorSet）のキャッシュ（phase12 D-3）。
// テクスチャは起動時に確定し以後不変なので、ディスクリプタセットはフレーム数とは無関係に
// テクスチャごと1個で足りる（set=1。phase12 D-5）。
class TextureRegistry {
public:
    // descriptor_pool / material_set_layout / sampler は Renderer が所有し、ここでは破棄しない。
    // material_set_layout は「binding=0: COMBINED_IMAGE_SAMPLER, FRAGMENT」のレイアウト。
    TextureRegistry(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
                    std::uint32_t queue_family, VkQueue queue,
                    VkDescriptorPool descriptor_pool,
                    VkDescriptorSetLayout material_set_layout,
                    VkSampler sampler);
    ~TextureRegistry();

    TextureRegistry(const TextureRegistry&) = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    // path の画像を読み込んで登録し TextureId を返す。
    // 同じ path が既に登録済みならロードせず既存の id を返す（by_path_ で判定）。
    // 手順:
    //   1. by_path_.find(path) → あればその id を返す
    //   2. make_unique<Texture>(physical_device_, device_, *allocator_, queue_family_, queue_, path)
    //   3. vkAllocateDescriptorSets(descriptor_pool_, material_set_layout_) で set=1 用を1つ確保
    //   4. VkDescriptorImageInfo{ SHADER_READ_ONLY_OPTIMAL, texture->view(), sampler_ } を
    //      binding=0 / COMBINED_IMAGE_SAMPLER として vkUpdateDescriptorSets
    //   5. entries_ に push、by_path_ に登録して id を返す
    scene::TextureId load(const std::string& path);

    // 描画時にバインドする set=1 のディスクリプタセット。
    [[nodiscard]] VkDescriptorSet descriptor_set(scene::TextureId id) const;
    [[nodiscard]] bool contains(scene::TextureId id) const;

    // Material::albedo が kInvalidTextureId のときに使う既定テクスチャ（最初に load したものを既定にする等）。
    [[nodiscard]] scene::TextureId default_texture() const;

private:
    struct Entry {
        std::unique_ptr<Texture> texture;
        VkDescriptorSet set = VK_NULL_HANDLE;  // プールから確保（個別破棄は不要）
    };

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    std::vector<Entry> entries_;                                  // 添字が TextureId
    std::unordered_map<std::string, scene::TextureId> by_path_;   // 同一パスの再ロード防止
    scene::TextureId default_texture_ = scene::kInvalidTextureId;
};

}  // namespace sq::graphics
```

---

## 3. シェーダの変更（`shaders/`）

### 3-1. [triangle.vert](../../shaders/triangle.vert)
```glsl
// push constant に base_color を追加する（ブロック定義は frag と完全一致させること。phase12 D-6）
layout(set = 0, binding = 0) uniform CameraUBO { mat4 view_proj; } camera;
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;   // vert では使わないが、宣言は frag と揃える
} pc;
// main() は変更不要（gl_Position = camera.view_proj * pc.model * vec4(in_position, 1.0)）
```

### 3-2. [triangle.frag](../../shaders/triangle.frag)
```glsl
// テクスチャを set=1, binding=0 へ移す（phase12 D-5）。base_color を乗算する。
layout(set = 1, binding = 0) uniform sampler2D tex_sampler;
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
} pc;

void main() {
    out_color = texture(tex_sampler, frag_uv) * pc.base_color;
}
```

> `frag_color`（頂点色）は現在未使用。`* vec4(frag_color, 1.0)` を掛けるかは好みで（掛けると cube の面ごとの頂点色が乗る）。

---

## 4. `GraphicsPipeline`（[graphics_pipeline.hpp](../../engine/include/sq/graphics/graphics_pipeline.hpp) / .cpp）

```cpp
// セットレイアウトを複数受け取れるようにする（set=0 カメラ / set=1 マテリアル）。
#include <vector>

class GraphicsPipeline {
public:
    // set_layouts: index が set 番号に対応する（[0]=カメラ, [1]=マテリアル）。
    //   呼び出し側が所有・破棄する（ここでは破棄しない）。
    GraphicsPipeline(VkDevice device, VkRenderPass render_pass, VkExtent2D viewport_extent,
                     const std::string& vert_spv_path, const std::string& frag_spv_path,
                     const std::vector<VkDescriptorSetLayout>& set_layouts,   // ← 単体から複数へ
                     const PipelineConfig& config);
    // ...
};
```

```cpp
// graphics_pipeline.cpp の変更点:
//  1. pipeline_layout_info.setLayoutCount = set_layouts.size();
//     pipeline_layout_info.pSetLayouts    = set_layouts.data();
//  2. push constant range を拡張する（phase12 D-6）:
//     push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
//     push_constant_range.offset = 0;
//     push_constant_range.size   = sizeof(PushConstants);   // 80 バイト（mat4 + vec4）
//     ※ サイズは 4 の倍数であること。maxPushConstantsSize（最小保証128）を超えないこと。
```

---

## 5. `Renderer`（[renderer.hpp](../../engine/include/sq/graphics/renderer.hpp) / [renderer.cpp](../../engine/src/graphics/renderer.cpp)）

### 5-1. push constant の構造体（`renderer.hpp` か新規ヘッダ）
```cpp
// シェーダの push_constant ブロックと**完全に一致**させる（phase12 D-6）。
struct PushConstants {
    glm::mat4 model;       // offset 0,  64 bytes
    glm::vec4 base_color;  // offset 64, 16 bytes
};
static_assert(sizeof(PushConstants) == 80, "シェーダの push_constant ブロックと一致させる");
```

### 5-2. メンバの変更（`renderer.hpp`）
```cpp
    // -- 削除 --
    // std::unique_ptr<VertexBuffer> triangle_mesh_;   // → MeshRegistry へ
    // std::unique_ptr<IndexBuffer>  cube_indices_;    // → MeshRegistry へ
    // std::unique_ptr<Texture>      texture_;         // → TextureRegistry へ
    // VkDescriptorSetLayout descriptor_set_layout_;   // → 2つに分割

    // -- 追加 --
    VkDescriptorSetLayout camera_set_layout_ = VK_NULL_HANDLE;    // set=0: binding0 UBO (VERTEX)
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;  // set=1: binding0 sampler (FRAGMENT)
    std::unique_ptr<MeshRegistry> meshes_;
    std::unique_ptr<TextureRegistry> textures_;

    // アセット登録用に公開する（main から renderer.meshes().add(...) / textures().load(...) を呼ぶ）。
public:
    [[nodiscard]] MeshRegistry& meshes();
    [[nodiscard]] TextureRegistry& textures();

private:
    // 収集した1件の描画情報（phase12 D-7）。
    struct DrawItem {
        glm::mat4 model;
        glm::vec4 base_color;
        scene::MeshId mesh = scene::kInvalidMeshId;
        scene::TextureId texture = scene::kInvalidTextureId;
        float distance_sq = 0.0f;  // 半透明ソート用（カメラからの2乗距離）
    };

    // items を順に記録する。直前にバインドした mesh / texture を覚えて、変化したときだけ
    // vkCmdBindVertexBuffers / vkCmdBindIndexBuffer / vkCmdBindDescriptorSets(set=1) を呼ぶ。
    void record_draw_items(VkCommandBuffer command_buffer,
                           const GraphicsPipeline& pipeline,
                           const std::vector<DrawItem>& items);
```

### 5-3. ディスクリプタ生成の変更（`renderer.cpp`）

```cpp
// create_descriptor_set_layout(): 1つのレイアウトに2 binding を詰めるのをやめ、2つ作る。
//   camera_set_layout_   : { binding=0, UNIFORM_BUFFER,         count=1, stage=VERTEX   }
//   material_set_layout_ : { binding=0, COMBINED_IMAGE_SAMPLER, count=1, stage=FRAGMENT }
//   ※ material 側の binding 番号は 1 ではなく **0**（別セットなので 0 から振り直す）。シェーダと合わせる。

// create_descriptor_pool(): マテリアル用セットの分を足す。
//   static constexpr std::uint32_t kMaxTextures = 64;   // 登録できるテクスチャ数の上限（要調整）
//   poolSizes = {
//     { UNIFORM_BUFFER,         kFramesInFlight },
//     { COMBINED_IMAGE_SAMPLER, kMaxTextures    },
//   };
//   maxSets = kFramesInFlight + kMaxTextures;

// create_descriptor_sets(): カメラUBO（set=0）だけを書き込む形に縮小する。
//   binding=1 のイメージ書き込みは削除（TextureRegistry::load が set=1 へ書き込む）。

// デストラクタ: descriptor_set_layout_ 1つの破棄を、camera_set_layout_ と material_set_layout_ の2つに。
//   破棄順序に注意: meshes_ / textures_ を device_（= GpuAllocator）より**前**に reset すること
//   （phase11 ③ の不変条件。Buffer/Texture はアロケータより先に壊す）。
```

### 5-4. 構築順序（`renderer.cpp` コンストラクタ）
```
 8. create_descriptor_set_layout()   // camera_set_layout_ / material_set_layout_ の2つを作る
 9. create_uniform_buffers()
10. create_sampler()                 // ★ TextureRegistry より前に必要（sampler を渡すため）
11. create_descriptor_pool()         // ★ TextureRegistry より前に必要（pool を渡すため）
    create_descriptor_sets()         // set=0（カメラUBO）のみ
12. pipeline_opaque_ / pipeline_transparent_
    // set_layouts = { camera_set_layout_, material_set_layout_ } を渡す
13. create_framebuffers()
14. command_buffers_ / 15. sync_objects_
16. meshes_ / textures_ を生成する（旧 create_cube_mesh / create_texture を置き換え）
    // meshes_   = make_unique<MeshRegistry>(device_->allocator(), device_->handle(), qf, queue);
    // textures_ = make_unique<TextureRegistry>(physical_device_, device_->handle(), device_->allocator(),
    //                                          qf, queue, descriptor_pool_, material_set_layout_, sampler_->handle());
    // TODO: 既定アセットをここで登録する（add_cube_mesh(*meshes_) / textures_->load("textures/default.png")）。
    //       アプリ固有のアセット登録は main 側で renderer.meshes()/textures() 経由で行う。
```

### 5-5. `draw_frame` の描画部（`command_buffers_->record` 内）

```cpp
// カメラは phase11 のまま（3段フォールバックで view_projection と cam_pos を得る）。
// set=0 をフレーム先頭で1回だけバインドする（レイアウトは2本のパイプラインで共通）:
//   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
//       pipeline_opaque_->layout(), 0, 1, &descriptor_sets_[current_frame_], 0, nullptr);

// ---- 1. 収集（phase12 D-7）----
std::vector<DrawItem> opaque_items;
std::vector<DrawItem> transparent_items;

// MeshHandle を持つエンティティのみが描画対象（Transform だけのカメラ等は除外される）。
registry.view<scene::Transform, scene::MeshHandle>().each(
    [&](ecs::Entity e, scene::Transform& t, scene::MeshHandle& mh) {
        // TODO:
        //  1. if (!meshes_->contains(mh.id)) return;              // 未登録メッシュはスキップ
        //  2. Material を引く（無ければ既定値）:
        //       scene::Material mat{};                            // albedo=invalid, white, opaque
        //       if (registry.has<scene::Material>(e)) { mat = registry.get<scene::Material>(e); }
        //  3. テクスチャ解決: scene::TextureId tex = textures_->contains(mat.albedo)
        //                        ? mat.albedo : textures_->default_texture();
        //  4. DrawItem を作る:
        //       glm::mat4 model = t.model();                      // TRS 合成（phase12 D-4）
        //       glm::vec3 d = t.position - cam_pos;
        //       DrawItem item{ model, mat.base_color, mh.id, tex, glm::dot(d, d) };
        //  5. mat.transparent ? transparent_items.push_back(item) : opaque_items.push_back(item);
    });

// ---- 2. ソート ----
// 不透明: 再バインドを減らすため (texture, mesh) でまとめる（順序は自由。深度テストが解決する）
// TODO: std::sort(opaque_items.begin(), opaque_items.end(),
//           [](const DrawItem& a, const DrawItem& b){
//               return std::tie(a.texture, a.mesh) < std::tie(b.texture, b.mesh); });
//
// 半透明: distance_sq 降順（遠い順）。★ 正しさが順序に依存するのでテクスチャでまとめてはいけない
// TODO: std::sort(transparent_items.begin(), transparent_items.end(),
//           [](const DrawItem& a, const DrawItem& b){ return a.distance_sq > b.distance_sq; });

// ---- 3. 記録 ----
// vkCmdBindPipeline(..., pipeline_opaque_->handle());
// record_draw_items(command_buffer, *pipeline_opaque_, opaque_items);
// vkCmdBindPipeline(..., pipeline_transparent_->handle());
// record_draw_items(command_buffer, *pipeline_transparent_, transparent_items);
```

### 5-6. `record_draw_items` の骨格
```cpp
void Renderer::record_draw_items(VkCommandBuffer command_buffer,
                                 const GraphicsPipeline& pipeline,
                                 const std::vector<DrawItem>& items) {
    // 直前にバインドしたものを覚えて、変化した時だけ再バインドする。
    scene::MeshId    bound_mesh    = scene::kInvalidMeshId;
    scene::TextureId bound_texture = scene::kInvalidTextureId;

    for (const DrawItem& item : items) {
        // TODO:
        //  1. if (item.texture != bound_texture) {
        //         VkDescriptorSet set = textures_->descriptor_set(item.texture);
        //         vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        //             pipeline.layout(), 1, 1, &set, 0, nullptr);   // ← firstSet = 1
        //         bound_texture = item.texture;
        //     }
        //  2. if (item.mesh != bound_mesh) {
        //         const MeshRegistry::Entry& mesh = meshes_->get(item.mesh);
        //         mesh.vertices->bind(command_buffer);
        //         mesh.indices->bind(command_buffer);
        //         bound_mesh = item.mesh;
        //     }
        //  3. PushConstants pc{ item.model, item.base_color };
        //     vkCmdPushConstants(command_buffer, pipeline.layout(),
        //         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        //         0, sizeof(pc), &pc);
        //  4. vkCmdDrawIndexed(command_buffer, meshes_->get(item.mesh).indices->index_count(), 1, 0, 0, 0);
        //     （index_count は**メッシュごと**に引く。cube 固定をやめる点が本フェーズの要）
    }
}
```

---

## 6. `main.cpp`（[sandbox_graphics/main.cpp](../../sandbox_graphics/main.cpp)）

```cpp
// Renderer 生成後にアセットを登録し、得た ID をコンポーネントへ入れる。
//   const auto cube = sq::graphics::add_cube_mesh(renderer.meshes());
//   const auto tex_a = renderer.textures().load("textures/hsr_icon_01/Blade 1.png");
//   const auto tex_b = renderer.textures().load("textures/default.png");
//
// エンティティごとに見た目を変える（本フェーズの成果を確認する）:
//   registry.add<MeshHandle>(e, { cube });
//   registry.add<Material>(e, Material{ .albedo = (i % 2 ? tex_a : tex_b),
//                                       .base_color = glm::vec4(1.0f, 0.6f, 0.6f, 0.5f),
//                                       .transparent = true });
//   registry.add<Transform>(e, Transform{ .position = {x, 0.0f, z},
//                                         .rotation = glm::angleAxis(angle, glm::vec3(0,1,0)),
//                                         .scale    = glm::vec3(0.5f) });
//
// 注意: アセット登録は loop() 内（Renderer 生成後）で行う必要がある。現状 registry の構築は
//       main() で Renderer より前に行っているため、
//         a) エンティティ生成を loop() 内に移す、または
//         b) main() で Renderer を先に作って registry と一緒に loop() へ渡す
//       のどちらかに整理する（b の方が main の見通しが良い）。
//
// 旧 Position / Velocity の付与は削除してよい（描画は Transform を見るようになる）。
```

---

## 7. CMakeLists への追加（[engine/CMakeLists.txt](../../engine/CMakeLists.txt)）
```
src/scene/transform.cpp        # Transform::model()
src/graphics/mesh_registry.cpp
src/graphics/texture_registry.cpp
```

---

## 実装手順（この順で、各ステップ完了ごとに動作確認）

1. **Transform の TRS 化**（1-3 + `transform.cpp`）
   - `draw_frame` を `view<Transform, Position>` → `view<Transform>` にし、`t.model()` を使う（この時点ではまだ共有メッシュ/テクスチャ）。
   - main で `Transform.position` を設定。→ **回転・スケールが効くことを確認**（従来と同じ位置に出て、`scale` を変えると大きさが変わる）。
2. **MeshRegistry**（1-1, 2-1）
   - `create_cube_mesh` の頂点データを `add_cube_mesh` へ移し、`MeshHandle` を持つエンティティを描画する形にする。
   - → **メッシュを2種類登録して、エンティティごとに形が変わることを確認**（cube と triangle 等）。
3. **ディスクリプタ分離 + シェーダ変更**（3, 4, 5-3）
   - `set=0`/`set=1` に分け、パイプラインレイアウトを2セット構成に。テクスチャは**まだ1枚**でよい。
   - → **セット分離後も従来通りテクスチャが貼れることを確認**（ここが壊れやすい。バリデーションを必ず見る）。
4. **TextureRegistry + Material 拡張**（1-2, 2-2）
   - → **エンティティごとに別テクスチャが貼れることを確認**。
5. **push constant に base_color**（5-1, 5-6 の該当部）
   - → **`base_color` の色 tint が効き、`a < 1` で半透明の濃さが変わることを確認**（Phase 11 ① の半透明パスと組み合わせて）。
6. **DrawItem 収集 + ソート + record_draw_items**（5-2, 5-5, 5-6）
   - → **不透明がテクスチャ単位でまとまり、半透明が back-to-front で正しく透けることを確認**。

> 3 は「動くものが一時的に壊れやすい」唯一のステップ。2 まで終えた状態を git のコミットで区切ってから着手するとよい。

---

## 検証観点（フェーズ全体）
- エンティティごとに**メッシュ**・**テクスチャ**・**色 tint**・**位置/回転/スケール**が独立に変わること。
- `MeshHandle` を持たないエンティティ（カメラ等）が描画対象に混ざらないこと。
- `Material` を持たないエンティティが既定マテリアル（既定テクスチャ・白・不透明）で描かれること（後方互換）。
- 同じメッシュ/テクスチャを 100 体が参照しても、GPU バッファ・`Texture` が**1組しか作られない**こと（レジストリのキャッシュが効いている）。
- 半透明が back-to-front で正しく透けること（Phase 11 ① の不変条件を維持）。
- 終了時にバリデーションのリーク報告が無いこと（`meshes_`/`textures_` が `Device`＝`GpuAllocator` より先に破棄されている）。
- リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab 復帰で描画とカメラ操作が継続すること。

---

## Phase 13 以降の将来課題
- **bindless テクスチャ**（`VK_EXT_descriptor_indexing`）: テクスチャ配列＋インデックスを push constant で渡し、マテリアルごとの `vkCmdBindDescriptorSets` を消す。
- **マテリアル UBO / dynamic offset**: push constant に収まらない属性（metallic/roughness/emissive 等）が増えたら移行。
- **インスタンシング**（`vkCmdDrawIndexed(..., instanceCount, ...)`）: 同一メッシュ＋同一マテリアルの塊を 1 ドローに。今回の `(texture, mesh)` ソートがそのまま前提になる。
- **Transform の行列キャッシュ + dirty フラグ**、および**親子階層**（`Parent` コンポーネントとワールド行列の伝播）。
- **アセットのアンロード / 世代付きハンドル**（`MeshId`/`TextureId` に generation を持たせる）。
- **モデルファイルのロード**（glTF / OBJ）→ `MeshRegistry::add` へ流し込む。
- **ミップマップ生成（`vkCmdBlitImage`）**（Phase 10/11 から継続の宿題）。
- **テクスチャイメージメモリの `GpuAllocator` 対応**（Phase 11 ③ ではバッファのみ対応）。
- **フラスタムカリング**: 収集フェーズで `ActiveCamera` の視錐台外を捨てる。`DrawItem` 収集構造がその置き場になる。
