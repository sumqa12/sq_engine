# C++ ゲームエンジン（学習目的）— Phase 14: マテリアルバッファ + Transform 階層 + 世代付きハンドル + glTF ローダー

## Context
Phase 13 で描画効率化が一通り完了した（[phase13-bindless-instancing-mipmap-culling.md](phase13-bindless-instancing-mipmap-culling.md) 参照）。bindless テクスチャ・インスタンシング・ミップマップ・イメージのアロケータ対応・フラスタムカリングが入り、3375 体のシーンで**不透明が2ドロー**（メッシュ2種）まで畳まれることを確認した。

Phase 14 は**データ構造とアセット管理の整備**で、描画効率とは独立している。現状の「暫定形」:

- **`InstanceData` にマテリアルが直接埋まっている**: [instance_data.hpp](../../engine/include/sq/graphics/instance_data.hpp) は `base_color` と `texture_index` を体ごとに持つ。metallic / roughness / emissive / 法線マップ…と増えるたびに**全インスタンスぶん重複する**。
- **アセットは登録のみ・解放なし**: `MeshId` / `TextureId` は `entries_` の添字そのもの（Phase 12 D-1 の割り切り）。解放を入れると添字が再利用され、古いハンドルが別アセットを指す（ABA 問題）。
- **ジオメトリが手書きのみ**: [mesh_registry.cpp](../../engine/src/graphics/mesh_registry.cpp) の `add_cube_mesh` / `add_plane_mesh` しかない。
- **`Transform::model()` が毎フレーム毎エンティティで T*R*S を合成**（Phase 12 D-4 で「将来課題」とした部分）。親子関係も無い。

例によって Claude は宣言・骨格・TODO コメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### D-0. 着手順は ① → ④ → ② → ③
| 順 | 項目 | 理由 |
|---|---|---|
| 1 | **① マテリアルバッファ** | Phase 13 で作った `InstanceBuffer` の延長。既存構造の中で閉じる足慣らし |
| 2 | **④ Transform キャッシュ + 親子階層** | ③ の glTF ノード階層に**必須**。ECS の更新順序という別種の難所 |
| 3 | **② 世代付きハンドル** | glTF が大量のアセットを登録し始める**前に**基盤を固める |
| 4 | **③ glTF ローダー** | ①②④ の全部を使う。このフェーズの成果物 |

**④→③ の依存**: glTF のノードは親子で入れ子になっており、子ノードの変換は親の変換を含む。`Parent` コンポーネントとワールド行列の伝播が無いと、階層を持つモデルを正しく置けない。

**②→③ の依存**: glTF 1ファイルで数十〜数百のテクスチャ・マテリアルが登録され得る。ハンドルの設計を後から変えると、glTF ローダー側も作り直しになる。

### D-1. マテリアルは SSBO に置き、`InstanceData` は `material_index` だけを持つ
- `InstanceData { mat4 model; uint material_index; }` に痩せる（96 → 80 バイト）。
- マテリアル本体は **`MaterialRegistry` が持つ SSBO の配列**。`MaterialId` がそのまま添字になる（`TextureId` と同じ方針）。
- **UBO ではなく SSBO を選ぶ**理由: Phase 13 ② で作った `InstanceBuffer` と**同じ形**（std430・永続マップ）にでき、レイアウト規則を1つ覚えるだけで済む。UBO は `maxUniformBufferRange` の最小保証が 16KiB でマテリアル数に上限が出る。
- **★ per-instance の `base_color`（色 tint）は廃止する**。同じマテリアルを共有する体でデータを重複させないのが本項目の目的なので、色差は「マテリアルを複数作る」で表現する。[main.cpp](../../sandbox_graphics/main.cpp) の検証シーンは**格子座標 → 色 → マテリアル**ではなく**格子座標 → マテリアル選択**に変える必要がある（後述 ①-5）。

### D-2. set の役割を「不変か否か」で分ける【重要】
Phase 13 で set=0 / set=1 の性格が分かれた。ここを明文化して、マテリアル SSBO の置き場を決める。

| set | 性格 | 中身 | 個数 |
|---|---|---|---|
| **set=0** | **フレームごとに変わる** | binding=0: カメラUBO<br>binding=1: インスタンス SSBO | `kFramesInFlight` 個 |
| **set=1** | **起動後は不変（アセット）** | binding=0: bindless テクスチャ配列<br>binding=1: **マテリアル SSBO**（← 今回追加） | 全体で1個 |

マテリアルは起動時に確定して以後変わらないので、テクスチャと同じ set=1 が自然。フレーム複製が不要になり、バッファは1本で済む。

> **注意**: set=1 のレイアウトには `UPDATE_AFTER_BIND_POOL_BIT` が付いている。binding=1 に `UPDATE_AFTER_BIND_BIT` を**付けなければ**通常のバインディングとして扱われるので、追加の機能フラグ（`descriptorBindingStorageBufferUpdateAfterBind`）は要らない。プールの `UPDATE_AFTER_BIND_BIT` は set 単位なのでそのままでよい。

### D-3. Transform は private 化して dirty フラグを持たせる
- 現状 `position` / `rotation` / `scale` は public で、[main.cpp](../../sandbox_graphics/main.cpp) が designated initializer で直接書いている。**public のままでは変更を検知できない**（書き換えられても dirty にできない）。
- **private + setter/getter** にする。designated initializer が使えなくなるので、3引数のコンストラクタを用意する。
- キャッシュは `mutable` にする。**`Renderer::draw_frame` は `const Registry&` を受ける**ため、const 経路から `model()` を呼んで再計算できる必要がある。
  - なお `Registry::get<T>() const` は `T&`（非const参照）を返す設計なので、システム側からの書き換えは今でも可能。ここは `mutable` を使わない選択肢もあるが、「読み取りに見える操作でキャッシュだけ更新する」意図を型で示すため `mutable` にする。

### D-4. ワールド行列は別コンポーネント、伝播はメモ化付き再帰
- `Parent { ecs::Entity entity; }`（親を指すコンポーネント。持たないものはルート）
- `WorldTransform { glm::mat4 matrix; }`（合成済みワールド行列。**描画対象には必ず付ける**）
- 更新は「親 → 子」の順でなければならない。トポロジカル順のリストを毎フレーム作るのは重いので、**メモ化付き再帰**にする:
  - `resolve_world(entity)`: 自分が解決済みならそのまま返す。未解決なら親を先に `resolve_world` してから `parent_world * local` を求めて記録する。
  - フレーム先頭で「解決済みフラグ」を全クリアし、各エンティティについて呼ぶだけ。順序を気にしなくてよくなる。
- **★ ECS の罠**: `View::each()` のラムダ内で `add` / `remove` するとアーキタイプ間移動が起きて反復中のストレージが壊れる（[camera.hpp](../../engine/include/sq/scene/camera.hpp) の `set_active_camera` に同じ注意書きがある）。**伝播処理の中で `WorldTransform` を後付けしてはいけない**。エンティティ生成時に必ず付ける規約にする。
- **★ 循環参照**（A の親が B、B の親が A）に入ると無限再帰でスタックが溢れる。再帰の深さ上限か「解決中」フラグで検出して打ち切る。

### D-5. ハンドルは `{ index, generation }` の値型にする
- `MeshId` / `TextureId` / `MaterialId` を `std::uint32_t` の別名から**構造体**へ変える。
```
struct AssetHandle { std::uint32_t index; std::uint32_t generation; };
```
- レジストリはスロットごとに `generation` を持ち、解放時にインクリメントする。`is_valid(handle)` は `slots_[index].generation == handle.generation && slots_[index].alive` で判定する。
- **★ ECS の `Entity` が既に同じ仕組みを持っている**（[registry.hpp](../../engine/include/sq/ecs/registry.hpp) の `EntityRecord::generation` と `free_ids_`）。**同じ設計をアセット側に持ち込む**という理解でよい。実装の参考にすること。
- **★ bindless の添字は `index` だけを渡す**。`TextureId` がそのまま配列の添字だった前提（Phase 13 D-4）が崩れるので、シェーダへ渡す箇所は `handle.index` に読み替える。
- **★ GPU が使用中のリソースを即座に破棄してはいけない**。`unload()` を呼んだフレームではまだ GPU が読んでいる可能性がある。**遅延解放キュー**（`kFramesInFlight` フレーム待ってから実際に破棄する）が要る（後述 ②-4）。ここが本項目で最も事故りやすい。
- 参照カウント（誰も使っていないテクスチャの自動破棄）は**このフェーズではやらない**。明示的な `unload()` のみ。

### D-6. glTF は tinygltf、対応範囲を絞る
- ライブラリは **tinygltf**（vcpkg。ヘッダオンリー。内部で nlohmann-json と stb を使う。stb は既に依存にある）。
- **対応する**: `.gltf` / `.glb`、メッシュのプリミティブ（`POSITION` / `NORMAL` / `TEXCOORD_0` / インデックス）、`baseColorTexture`、`baseColorFactor`、`metallicFactor` / `roughnessFactor`、ノード階層、TRS とマトリクス両方のノード変換。
- **対応しない（明記して割り切る）**: アニメーション、スキニング、モーフターゲット、カメラ、ライト、KHR 拡張全般、スパースアクセサ、`TEXCOORD_1` 以降、頂点カラー。
- **★ 巻き順（winding）が逆**: glTF の仕様は**反時計回り（CCW）が表**。現在のパイプラインは `frontFace = VK_FRONT_FACE_CLOCKWISE`（[mesh_registry.cpp](../../engine/src/graphics/mesh_registry.cpp) のコメント参照）。**どちらかに揃えないとモデルが裏返って見える**（面が全部消える）。→ 後述 ③-4。
- **★ インデックスが uint16 固定**: [mesh.hpp](../../engine/include/sq/graphics/mesh.hpp) の `IndexBuffer` は `std::uint16_t` 前提。glTF は 65536 頂点を超えると uint32 を使う。→ `IndexBuffer` に `VkIndexType` を持たせる（後述 ③-2）。

### D-7. `Vertex` の `color` を `normal` に置き換える
- 現在の `Vertex { position, color, uv }` の `color` は、Phase 12 で `Material::base_color` が入って以降**どこからも使われていない**（vert が frag へ渡しているが frag が読んでいない）。
- glTF は頂点カラーを持たないことが多く、代わりに `NORMAL` を持つ。ライティングは Phase 15 の課題だが、**今のうちに法線を運ぶ器にしておく**方が、後で頂点フォーマットを作り直さずに済む。
- 影響範囲: `Vertex` 構造体、[graphics_pipeline.cpp](../../engine/src/graphics/graphics_pipeline.cpp) の attribute description、`add_cube_mesh` / `add_plane_mesh` の頂点データ、`triangle.vert` / `triangle.frag`。

---

## ① マテリアルバッファ（SSBO）

### ①-1. GPU 側のマテリアルデータ（新規 `material_data.hpp`）

```cpp
// engine/include/sq/graphics/material_data.hpp（新規）
#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace sq::graphics {

// マテリアル1件のGPU側レイアウト（phase14 ① / D-1）。SSBO の配列要素になる。
// ★ シェーダの std430 ブロックと完全に一致させること（InstanceData と同じ規則）。
//   vec4 は16バイト境界。uint を並べるときは4個で16バイトに揃うことを利用する。
struct MaterialData {
    glm::vec4 base_color{1.0f};      // offset  0, 16  テクスチャに乗算する色（a は不透明度）
    glm::vec4 emissive{0.0f};        // offset 16, 16  自己発光（w は未使用。将来 strength）
    float metallic = 0.0f;           // offset 32,  4
    float roughness = 1.0f;          // offset 36,  4
    float alpha_cutoff = 0.5f;       // offset 40,  4  （MASK モード用。将来使う）
    std::uint32_t albedo_index = 0;  // offset 44,  4  bindless テクスチャ配列の添字
    // 将来: normal_index / metallic_roughness_index / occlusion_index / emissive_index
    // ここに uint を足すときは、合計が16の倍数になるようパディングを調整すること。
};

static_assert(sizeof(MaterialData) == 48, "std430 のレイアウトと一致させること");

}  // namespace sq::graphics
```

### ①-2. `InstanceData` を痩せさせる（[instance_data.hpp](../../engine/include/sq/graphics/instance_data.hpp)）

```cpp
struct InstanceData {
    glm::mat4 model;                 // offset  0, 64
    std::uint32_t material_index;    // offset 64,  4
    std::uint32_t _pad[3]{};         // offset 68, 12
};
static_assert(sizeof(InstanceData) == 80, "std430 のレイアウトと一致させること");
```

> `base_color` と `texture_index` が消える。両方とも `MaterialData` 側へ移る。

### ①-3. `MaterialRegistry`（新規 `material_registry.hpp` / `.cpp`）

```cpp
// TextureRegistry と同じ形（添字がID・起動時に登録・以後不変）。
// 違いは「ディスクリプタの配列要素ではなく、SSBO の配列要素を書く」点。
class MaterialRegistry {
public:
    // descriptor_set は Renderer が所有する set=1（TextureRegistry::bindless_set() と同じもの）。
    // ここへ binding=1 として自分のバッファを書き込む。
    MaterialRegistry(GpuAllocator& allocator, VkDevice device,
                     VkDescriptorSet asset_set, std::uint32_t max_materials);
    ~MaterialRegistry();

    // マテリアルを登録し MaterialId を返す。
    // TODO: 内容が完全に同じマテリアルの重複排除（TextureRegistry の by_path_ 相当）を入れるか決める。
    //   glTF が同じマテリアルを何度も参照するので、効果は大きい。
    [[nodiscard]] scene::MaterialId add(const MaterialData& data);

    [[nodiscard]] bool contains(scene::MaterialId id) const;

    // 登録済みの内容を1件だけ書き換える（デバッグ・エディタ用途）。
    // ★ GPU が読んでいる最中に書き換えると壊れる。呼ぶ側が vkDeviceWaitIdle するか、
    //   フレーム複製する必要がある。このフェーズでは「起動時のみ」の前提で使うこと。
    void update(scene::MaterialId id, const MaterialData& data);

private:
    // TODO: InstanceBuffer と同型の永続マップ SSBO を1本持つ。
    //   フレーム複製は不要（起動後不変のため。D-2）。
    std::unique_ptr<Buffer> buffer_;
    void* mapped_ = nullptr;
    std::uint32_t max_materials_ = 0;
    std::vector<MaterialData> entries_;  // CPU側の控え（添字が MaterialId）
};
```

### ①-4. `Material` コンポーネントを差し替える（[material.hpp](../../engine/include/sq/scene/material.hpp)）

```cpp
// scene::Material は「どのマテリアルを使うか」だけを持つ形に痩せる。
// 見た目のパラメータは graphics::MaterialData（GPU側）へ移った（phase14 ①）。
struct Material {
    MaterialId id = kInvalidMaterialId;

    // ★ transparent はここに残す。CPU 側の描画パス振り分け（不透明/半透明）に使うもので、
    //   シェーダには渡らないため MaterialData には入れない。
    bool transparent = false;
};
```

> **★ 移行の注意**: `Material::albedo` と `Material::base_color` を参照している箇所が
> [renderer.cpp](../../engine/src/graphics/renderer.cpp) の収集フェーズと [main.cpp](../../sandbox_graphics/main.cpp) にある。両方同時に直すこと。

### ①-5. 収集フェーズと検証シーンの移行

```cpp
// renderer.cpp の収集フェーズ:
//   InstanceData の組み立てが { model, material_index } だけになる。
//   テクスチャのフォールバック（textures_->contains ? : default_texture）は
//   MaterialRegistry::add の時点へ移動する（マテリアルが添字を持つため）。
//
// main.cpp の検証シーン（phase13 で作った 15^3 の格子）:
//   「格子座標 → base_color」をやめ、「格子座標 → マテリアル選択」に変える。
//   例: 起動時に 8〜16 個のマテリアルを作っておき、格子座標から選ぶ。
//   ★ これにより per-instance の色グラデーションは段階的になる。
//     phase13 で使った「ずれると縞になる」検証は、マテリアル数を増やせば同様に機能する。
```

### ①-6. シェーダの変更

```glsl
// triangle.vert
struct InstanceData { mat4 model; uint material_index; };
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer { InstanceData instances[]; };

layout(location = 2) out flat uint frag_material_index;   // texture_index の代わり

// triangle.frag
struct MaterialData {
    vec4 base_color;
    vec4 emissive;
    float metallic;
    float roughness;
    float alpha_cutoff;
    uint albedo_index;
};
layout(std430, set = 1, binding = 1) readonly buffer MaterialBuffer { MaterialData materials[]; };

layout(location = 2) in flat uint frag_material_index;

void main() {
    MaterialData m = materials[frag_material_index];
    // ★ nonuniformEXT は引き続き必要（インスタンスごとに albedo_index が変わるため）
    out_color = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv) * m.base_color;
}
```

> **★ `frag_base_color` の受け渡しが不要になる**（マテリアルから直接引くため）。vert / frag 双方の `location` を詰め直すこと。

### ①-7. 検証
- 見た目が Phase 13 と**同等**であること（マテリアル数を絞ったぶん色の段階は粗くなる）。
- `sizeof(InstanceData)` が 80 になり、4096 体ぶんのバッファが 384KiB → 320KiB に減ること。
- 同じマテリアルを共有する体が増えても SSBO のサイズが増えないこと（これが本項目の目的）。
- バリデーションで std430 レイアウト関連のエラーが出ないこと。

---

## ④ Transform の行列キャッシュ + 親子階層

### ④-1. `Transform` を private 化する（[transform.hpp](../../engine/include/sq/scene/transform.hpp)）

```cpp
// phase14 ④ / D-3: 変更を検知するためメンバを private にし、setter 経由でのみ書き換える。
// designated initializer が使えなくなるので、3引数のコンストラクタを用意する。
class Transform {
public:
    Transform() = default;
    Transform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale);

    [[nodiscard]] const glm::vec3& position() const;
    [[nodiscard]] const glm::quat& rotation() const;
    [[nodiscard]] const glm::vec3& scale() const;

    // 書き換えると dirty_ が立ち、次の local() で再計算される。
    void set_position(const glm::vec3& v);
    void set_rotation(const glm::quat& q);
    void set_scale(const glm::vec3& v);

    // ローカル変換行列（T * R * S）。dirty のときだけ合成し直す。
    // ★ const だがキャッシュを書き換える（mutable）。Renderer が const Registry& から
    //   呼ぶため、非const にはできない（D-3）。
    [[nodiscard]] const glm::mat4& local() const;

    // 親のワールド行列が変わったときに、子側から明示的に汚す。
    void mark_dirty() const;
    [[nodiscard]] bool is_dirty() const;

private:
    glm::vec3 position_{0.0f};
    glm::quat rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale_{1.0f};

    mutable glm::mat4 cached_local_{1.0f};
    mutable bool dirty_ = true;   // 初回は必ず計算する
};
```

> 旧 `model()` は `local()` に改名する（ワールド行列と区別するため）。呼び出し箇所は
> [renderer.cpp](../../engine/src/graphics/renderer.cpp) の収集フェーズ1箇所のみ。

### ④-2. 階層コンポーネント（新規 `hierarchy.hpp`）

```cpp
// engine/include/sq/scene/hierarchy.hpp（新規）
namespace sq::scene {

// 親を指すコンポーネント。持たないエンティティはルート（phase14 ④ / D-4）。
// ★ 循環（A→B→A）を作らないこと。作ると伝播が無限再帰になる。
struct Parent {
    ecs::Entity entity;
};

// 合成済みのワールド変換行列。
// ★ 描画対象には必ず付けること。伝播処理の中で後付けすると、
//   View::each() の反復中にアーキタイプ移動が起きてストレージが壊れる（D-4）。
struct WorldTransform {
    glm::mat4 matrix{1.0f};
    bool resolved = false;   // このフレームで解決済みか（毎フレーム先頭でクリアする）
};

}  // namespace sq::scene
```

### ④-3. 伝播システム（新規 `transform_system.hpp` / `.cpp`）

```cpp
// engine/include/sq/scene/transform_system.hpp（新規）
namespace sq::scene {

// Transform（ローカル）と Parent から WorldTransform を求めるシステム（phase14 ④ / D-4）。
// 収集フェーズ（Renderer::draw_frame）より前に、毎フレーム1回実行する。
class TransformSystem {
public:
    // 全 WorldTransform の resolved を false にしてから、全エンティティを解決する。
    //   1. registry.view<WorldTransform>().each(...) で resolved = false
    //   2. registry.view<Transform, WorldTransform>().each(...) で resolve_world(e) を呼ぶ
    // ★ ここで add/remove は絶対にしない（D-4 の罠）。
    static void update(ecs::Registry& registry);

private:
    // entity のワールド行列を求めて WorldTransform へ書き込み、resolved を立てる。
    // 手順:
    //   1. 既に resolved なら何もしない（メモ化）
    //   2. Parent を持たない → world = transform.local()
    //   3. Parent を持つ    → 親を先に resolve_world（再帰）してから
    //                          world = parent_world.matrix * transform.local()
    //   4. resolved = true
    //
    // ★ 循環参照の検出: depth を引数に持ち、kMaxDepth（例 64）を超えたら
    //   単位行列を入れて打ち切り、spdlog::error を出す。無限再帰でスタックを溢れさせない。
    // ★ 親が Transform / WorldTransform を持たない場合は単位行列として扱う（壊れたハンドル対策）。
    static void resolve_world(ecs::Registry& registry, ecs::Entity entity, int depth);

    static constexpr int kMaxDepth = 64;
};

}  // namespace sq::scene
```

### ④-4. 収集フェーズの変更（[renderer.cpp](../../engine/src/graphics/renderer.cpp)）

```cpp
// 収集の view を <Transform, MeshHandle> から <WorldTransform, MeshHandle> に変える。
//   const glm::mat4& model = wt.matrix;          // t.model() を呼ばない
//
// ★ フラスタムカリングのスケール取得に注意（phase13 ⑤-3）。
//   これまで t.scale の最大成分を使っていたが、ワールド行列には親のスケールも掛かっている。
//   ワールド行列の 3 本の基底ベクトルの長さから求め直すこと:
//     const float sx = glm::length(glm::vec3(model[0]));
//     const float sy = glm::length(glm::vec3(model[1]));
//     const float sz = glm::length(glm::vec3(model[2]));
//     const float world_radius = entry.bounds.radius * std::max({sx, sy, sz});
//   ★ ここを直し忘れると、親でスケールした子が画面端で消える（phase13 で踏んだのと同じ症状）。
//
// ★ 半透明ソートの距離基準も t.position ではなくワールド位置にする:
//     const glm::vec3 world_pos = glm::vec3(model[3]);
```

### ④-5. 検証
- **見た目が ① の時点と変わらないこと**（階層を使っていないうちは、ワールド行列 = ローカル行列）。
- 親子を組んだとき、親を動かすと子が追従すること。親を回すと子が親を中心に回ること。
- 親を非等方スケールしたとき、子がカリングで消えないこと（④-4 の注意点）。
- `Transform` を書き換えないフレームで T*R*S の再計算が走らないこと（`dirty_` にカウンタを仕込んで確認）。
- 循環参照を意図的に作ってもクラッシュせず、エラーログが出ること。

---

## ② アセットのアンロード / 世代付きハンドル

### ②-1. ハンドル型（新規 `asset_handle.hpp`）

```cpp
// engine/include/sq/scene/asset_handle.hpp（新規）
namespace sq::scene {

// 世代付きアセットハンドル（phase14 ② / D-5）。
// index だけだと、解放されたスロットが再利用されたとき古いハンドルが
// 別のアセットを指してしまう（ABA 問題）。generation を併せ持って検出する。
//
// ★ ecs::Entity が同じ仕組みを持っている。実装はそちらを参考にすること。
struct AssetHandle {
    std::uint32_t index = kInvalidIndex;
    std::uint32_t generation = 0;

    static constexpr std::uint32_t kInvalidIndex = ~0u;

    [[nodiscard]] bool is_null() const { return index == kInvalidIndex; }
    friend bool operator==(const AssetHandle&, const AssetHandle&) = default;
};

// 型で取り違えないよう、用途ごとに別型にする（強い typedef）。
// ★ 単なる using だと MeshId を TextureId として渡してもコンパイルが通ってしまう。
struct MeshId     : AssetHandle {};
struct TextureId  : AssetHandle {};
struct MaterialId : AssetHandle {};

}  // namespace sq::scene
```

> **★ ソートで使えるようにする**: [renderer.cpp](../../engine/src/graphics/renderer.cpp) の不透明ソートが
> `a.mesh < b.mesh` を使っている。`operator<`（`index` 比較でよい）を定義するか、
> ソート側を `a.mesh.index < b.mesh.index` に変えること。
>
> **★ `std::unordered_map` のキーにするなら** `std::hash` の特殊化も要る
> （`TextureRegistry::by_path_` は値側なので現状は不要）。

### ②-2. スロット管理をレジストリへ入れる

```cpp
// MeshRegistry / TextureRegistry / MaterialRegistry に共通で入れる構造:
//
//   struct Slot {
//       /* 実体 */          // MeshRegistry なら Entry、TextureRegistry なら unique_ptr<Texture>
//       std::uint32_t generation = 0;
//       bool alive = false;
//   };
//   std::vector<Slot> slots_;
//   std::vector<std::uint32_t> free_indices_;   // 解放済みスロットの再利用リスト
//
// add/load の手順:
//   1. free_indices_ が空でなければ末尾から取り出して再利用、空なら push_back で新規
//   2. slot.generation は**インクリメントしない**（解放時に上げる）
//   3. slot.alive = true にして { index, slot.generation } を返す
//
// contains(handle) の手順:
//   handle.index < slots_.size() && slots_[handle.index].alive
//     && slots_[handle.index].generation == handle.generation
//
// ★ 共通部分をテンプレート基底（AssetRegistry<T>）に括り出すかは好みで決めてよい。
//   3つとも書いてみてから共通項を抜く方が、何を共通化すべきかが見えて学習になる。
```

### ②-3. `unload()` の追加

```cpp
// 各レジストリに追加する:
//   void unload(HandleType handle);
//
// 手順:
//   1. contains(handle) でなければ何もしない（二重解放の防止）
//   2. 実体を**遅延解放キューへ移す**（②-4。ここで直接 destroy しない）
//   3. slot.alive = false; slot.generation++;   // 以後、古いハンドルは弾かれる
//   4. free_indices_.push_back(handle.index);
//
// TextureRegistry では追加で:
//   5. bindless 配列の該当要素を**既定テクスチャで上書き**する。
//      ★ PARTIALLY_BOUND があっても、破棄済みの VkImageView を指したままの要素を
//        シェーダが引くと未定義動作。空にするのではなく、有効な view で埋め直すこと。
```

### ②-4. 遅延解放キュー【本項目の最難所】

```cpp
// 新規 engine/include/sq/graphics/deletion_queue.hpp
//
// unload() を呼んだ時点では、そのリソースを参照するコマンドバッファが
// まだ GPU で実行中かもしれない（kFramesInFlight 枚が飛んでいる）。
// 即座に vkDestroy* すると「使用中のリソースを破棄した」でバリデーションが叫ぶか、
// 環境によっては黙って壊れる。
//
// 対策: 「今から kFramesInFlight フレーム後に破棄する」キューに積み、
//   フレーム先頭で期限の来たものだけ実際に破棄する。
//
// class DeletionQueue {
// public:
//     // 破棄処理を frames_to_wait フレーム後に実行するよう予約する。
//     void push(std::function<void()> destroyer);
//
//     // フレーム先頭で1回呼ぶ。カウンタを進め、期限の来たものを実行する。
//     void flush_expired();
//
//     // 終了時に全件を即実行する（vkDeviceWaitIdle の後に呼ぶこと）。
//     void flush_all();
// private:
//     struct Pending { std::function<void()> destroyer; std::uint32_t remaining; };
//     std::vector<Pending> pending_;
// };
//
// ★ Renderer が1つ持ち、draw_frame の先頭（フェンス待機の直後）で flush_expired() を呼ぶ。
// ★ デストラクタでは vkDeviceWaitIdle → flush_all() の順。
// ★ std::function に unique_ptr をムーブキャプチャすると copy-constructible でなくなり
//   std::function に入らない。shared_ptr にするか、専用の型消去を書くこと（詰まりどころ）。
```

### ②-5. 検証
- アセットを `unload` した直後のフレームでバリデーションエラーが出ないこと（**遅延解放が効いているか**）。
- `unload` 済みのハンドルで `contains()` が false になること。
- `unload` → 新規 `load` でスロットが再利用され、**古いハンドルが新しいアセットを指さない**こと（generation の要）。
- テクスチャを `unload` したあと、そのIDを参照していたエンティティが既定テクスチャで描かれること（クラッシュしない）。
- 大量に load / unload を繰り返してもメモリが単調増加しないこと（`GpuAllocator` の空きマージが効くか。Phase 13 ④-3 の延長）。

---

## ③ glTF ローダー（tinygltf）

### ③-1. 依存の追加

```json
// vcpkg.json の dependencies に追加する
"tinygltf"
```

```cmake
# engine/CMakeLists.txt
# tinygltf はヘッダオンリー。実装の実体化は1つの翻訳単位でのみ行う
# （stb_image と同じ扱い。texture.cpp が既に STB_IMAGE_IMPLEMENTATION を定義しているので、
#  TINYGLTF_NO_STB_IMAGE_WRITE / TINYGLTF_NO_INCLUDE_STB_IMAGE の扱いに注意）。
find_package(tinygltf CONFIG REQUIRED)
```

> **★ stb の二重定義に注意**: tinygltf は既定で stb_image の実装を自前で展開する。
> [texture.cpp](../../engine/src/graphics/texture.cpp) が既に `STB_IMAGE_IMPLEMENTATION` を定義しているため、
> リンクエラー（多重定義）になる。`TINYGLTF_NO_INCLUDE_STB_IMAGE` を定義して既存の stb を使わせるか、
> tinygltf 側に任せて texture.cpp の定義を外すか、**どちらか一方に寄せること**。

### ③-2. `IndexBuffer` を uint16 / uint32 両対応にする（[mesh.hpp](../../engine/include/sq/graphics/mesh.hpp)）

```cpp
class IndexBuffer : public Buffer {
public:
    // phase14 ③ / D-6: glTF は 65536 頂点を超えると uint32 のインデックスを使う。
    IndexBuffer(GpuAllocator& allocator, VkDevice device,
                std::uint32_t queue_family, VkQueue queue,
                const std::vector<std::uint32_t>& indices);
    // 既存の uint16 版も残す（組み込みジオメトリ用）

    void bind(VkCommandBuffer command_buffer) const;  // index_type_ を vkCmdBindIndexBuffer へ渡す
    [[nodiscard]] std::uint32_t index_count() const;

private:
    std::uint32_t index_count_ = 0;
    VkIndexType index_type_ = VK_INDEX_TYPE_UINT16;   // ← 追加
};
```

### ③-3. `Vertex` の変更（[mesh.hpp](../../engine/include/sq/graphics/mesh.hpp)、D-7）

```cpp
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;   // ← color を置換（phase14 D-7）
    glm::vec2 uv;
};
// ★ graphics_pipeline.cpp の attribute description は offset だけ合っていれば
//   そのまま通ってしまう（vec3 → vec3 なので）。**気付かずに法線を色として使う**事故に注意。
//   シェーダ側の変数名も in_color → in_normal に必ず直すこと。
```

### ③-4. ローダー（新規 `gltf_loader.hpp` / `.cpp`）

```cpp
// engine/include/sq/assets/gltf_loader.hpp（新規。名前空間 sq::assets）
namespace sq::assets {

// glTF ファイルを読み込んだ結果。呼び出し側がシーンへ展開するために使う。
struct LoadedModel {
    // glTF のノード1つ分。parent は loaded_nodes 内の添字（ルートは kNoParent）。
    struct Node {
        std::size_t parent = kNoParent;
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        // このノードが描画するメッシュ（複数プリミティブは複数要素になる）。
        std::vector<std::pair<scene::MeshId, scene::MaterialId>> primitives;
        static constexpr std::size_t kNoParent = ~static_cast<std::size_t>(0);
    };
    std::vector<Node> nodes;
};

// path の .gltf / .glb を読み、メッシュ・テクスチャ・マテリアルを各レジストリへ登録して
// ノード階層を返す（phase14 ③）。エンティティの生成は行わない（呼び出し側の責務）。
//
// 手順:
//   1. tinygltf::TinyGLTF::LoadASCIIFromFile / LoadBinaryFromFile で model を読む
//      （拡張子で分岐。警告・エラー文字列は spdlog へ出す）
//   2. テクスチャ: model.images を走査し TextureRegistry::load へ。
//      ★ glTF の画像はファイル参照・埋め込み base64・GLB バッファの3形態がある。
//        tinygltf が既にデコードして image.image（RGBA バイト列）に入れてくれるので、
//        **パスではなくピクセル列から Texture を作れる経路**が要る
//        （Texture のコンストラクタにオーバーロードを足す）。
//   3. マテリアル: model.materials を走査し MaterialData を組んで MaterialRegistry::add へ。
//      baseColorFactor → base_color、baseColorTexture.index → albedo_index（★ TextureId.index）、
//      metallicFactor / roughnessFactor、emissiveFactor → emissive、alphaCutoff。
//      alphaMode == "BLEND" なら呼び出し側で transparent を立てられるよう記録しておく。
//   4. メッシュ: model.meshes[].primitives[] ごとに
//      POSITION / NORMAL / TEXCOORD_0 のアクセサを読んで Vertex 配列を組み、
//      インデックスアクセサを componentType に応じて uint16 / uint32 で読む。
//      ★ アクセサは bufferView の byteStride を持つ（詰まっているとは限らない）。
//        必ず stride を見て読むこと。ここを飛ばすと「頂点がぐちゃぐちゃになる」。
//      ★ NORMAL が無いプリミティブがある。無ければ面法線から生成するか、
//        暫定で {0,1,0} を入れる（Phase 15 のライティングまでは見た目に影響しない）。
//      ★ primitive.mode が TRIANGLES 以外（STRIP / FAN / POINTS）のものは
//        このフェーズでは読み飛ばして警告を出す。
//   5. ノード: model.nodes を走査。TRS 形式（translation/rotation/scale）と
//      matrix 形式の両方がある。matrix の場合は分解が要る
//      （glm::decompose、または平行移動＝4列目・スケール＝各基底の長さ・回転＝正規化した基底）。
//   6. 巻き順（D-6）: glTF は CCW が表。現行パイプラインは frontFace = CLOCKWISE。
//      ★ 対処は2択。**パイプラインを COUNTER_CLOCKWISE に変え、
//        組み込みジオメトリ（add_cube_mesh / add_plane_mesh）のインデックス順を反転させる**方が、
//        以後 glTF が標準になるので筋が良い。
//        （ローダー側でインデックスを反転させる手もあるが、読み込むたびに手を入れることになる）
[[nodiscard]] LoadedModel load_gltf(const std::string& path,
                                    graphics::MeshRegistry& meshes,
                                    graphics::TextureRegistry& textures,
                                    graphics::MaterialRegistry& materials);

}  // namespace sq::assets
```

### ③-5. シーンへの展開（[main.cpp](../../sandbox_graphics/main.cpp) または新規ヘルパ）

```cpp
// LoadedModel からエンティティを生成するヘルパ:
//   1. nodes と同じ数の Entity を先に作る（親を指すために全部の Entity が要る）
//   2. 各ノードについて Transform / WorldTransform を付ける
//   3. parent != kNoParent なら Parent{ entities[parent] } を付ける
//   4. primitives の各要素について MeshHandle / Material を付ける
//      ★ 1ノードが複数プリミティブを持つ場合、**子エンティティに分ける**
//        （1エンティティ = 1メッシュ = 1マテリアルの前提を保つため）
//   5. ルートに全体のスケール・位置を設定すれば、モデル全体を動かせる（④ の成果）
```

### ③-6. 検証
- 単純な glTF（Khronos の `Box.gltf` / `BoxTextured.gltf`）が正しく表示されること。
- **モデルが裏返らないこと**（③-4 手順6 の巻き順。裏返っていると面が消える）。
- 階層を持つモデル（`SimpleMeshes.gltf` など）でノードの入れ子が反映されること。
- `.glb`（バイナリ）でも読めること。
- 65536 頂点を超えるモデル（uint32 インデックス）が崩れないこと。
- テクスチャ付きモデルで UV が正しいこと（上下反転していないか。**glTF の UV 原点は左上**）。
- ノードを `matrix` 形式で持つモデルでも正しく配置されること。
- 組み込みジオメトリ（cube / plane）が従来どおり表示されること（巻き順を変えた影響）。

---

## 実装手順（この順で、各ステップ完了ごとに動作確認・コミット）

1. **① マテリアルバッファ** — `MaterialData` → `MaterialRegistry` → set=1 binding=1 → `InstanceData` を痩せさせる → `Material` コンポーネント差し替え → シェーダ → main.cpp の移行 → **見た目が同等であることを確認**
2. **④ Transform キャッシュ + 階層** — `Transform` の private 化 → `Parent` / `WorldTransform` → `TransformSystem` → 収集フェーズの差し替え（**カリングのスケール取得に注意**）→ **親子を組んで追従を確認**
3. **② 世代付きハンドル** — `AssetHandle` → 各レジストリのスロット化 → `DeletionQueue` → `unload()` → **load/unload の反復でリークしないことを確認**
4. **③ glTF ローダー** — vcpkg 追加（**stb の二重定義に注意**）→ `IndexBuffer` の型対応 → `Vertex` の法線化 → 巻き順の統一 → ローダー本体 → シーン展開 → **Khronos のサンプルモデルで確認**

> **3 と 4 が難所**。3 は `DeletionQueue` の型消去と「いつ破棄してよいか」の判断、4 は glTF 仕様の細部（stride / componentType / 巻き順 / UV 原点）が地雷原になる。2 まで終えた状態でコミットして区切ってから着手すること。

---

## 検証観点（フェーズ全体）
- Phase 13 で確認した性能特性が**維持**されていること（不透明がメッシュ種類数のドローに畳まれる、カリングが効く、半透明の順序が保たれる）。
- リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab 復帰で描画とカメラ操作が継続すること。
- 終了時にバリデーションのリーク報告が無いこと。
- glTF モデルを大量（数十体）に配置しても破綻しないこと。
- アセットの load / unload を繰り返してもメモリが単調増加しないこと。

---

## Phase 15 で行うこと（このフェーズではやらない・確定事項）

1. **ライティング** — Phase 14 D-7 で `Vertex` に法線を持たせ、`MaterialData` に metallic / roughness を用意したのはこのため。方向光 + PBR（Cook-Torrance）から始める。ライトを ECS コンポーネントとして扱う設計が要る。
2. **法線マップ・その他のテクスチャスロット** — `MaterialData` に `normal_index` 等を追加する。glTF は既に持っているので、ローダー側は読み足すだけ。接空間（tangent）の生成が必要。
3. **参照カウントによるアセットの自動破棄** — Phase 14 ② で明示的 `unload()` のみにした部分。

## さらに先の将来課題（plan16以降）
- **専用トランスファーキュー**（キューファミリ跨ぎの所有権移譲）。現状は起動時に graphics キューへ submit して待つだけ。
- **`GpuAllocator` → VMA 差し替え**（Phase 11 ③ で確保点を1箇所に閉じてある）。
- **OIT（順序独立透過）** — CPU ソートに依らない半透明。Phase 13 ② の「半透明だけインスタンス化できない」制約の解消にもなる。
- **キーコンフィグの設定ファイル入出力（JSON等）**（Phase 10 からの継続課題）。tinygltf が nlohmann-json を引き込むので、ここで使えるようになる。
- **エンティティの生成・破棄を行う System**（`Registry&` を非constで受ける形への拡張）。
- **オクルージョンカリング / LOD** — Phase 13 ⑤ のカリング基盤の延長。
- **アニメーション / スキニング** — Phase 14 ③ で対応外にした部分。④ の階層が土台になる。
