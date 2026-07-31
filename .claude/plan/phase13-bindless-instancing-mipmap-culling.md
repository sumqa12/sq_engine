# C++ ゲームエンジン（学習目的）— Phase 13: bindless + インスタンシング + ミップマップ + イメージのアロケータ対応 + フラスタムカリング

## Context
Phase 12 で描画データのコンポーネント化（`MeshHandle` / `Material` / TRS な `Transform`、`MeshRegistry` / `TextureRegistry`、set 分離、push constant の `base_color`、収集→ソート→バッチ記録）まで到達した（[phase12-render-data-components.md](phase12-render-data-components.md) 参照）。エンティティごとに見た目を変えられる状態になり、**描画の構造は整った**。

Phase 13 では、その構造の上で**描画の効率と品質**を上げる。現状の「暫定形」:

- **マテリアルごとに `vkCmdBindDescriptorSets`**: [renderer.cpp](../../engine/src/graphics/renderer.cpp) の `record_draw_items` はテクスチャが変わるたび set=1 をバインドし直す。Phase 12 の `(texture, mesh)` ソートで回数は減らせたが、**バインド自体をなくす**のが bindless。
- **1エンティティ = 1ドローコール**: 同じメッシュ・同じマテリアルが100体あっても `vkCmdDrawIndexed` を100回発行している。
- **ミップマップなし**: [texture.cpp](../../engine/src/graphics/texture.cpp) は `mipLevels = 1`。縮小時にちらつく（エイリアシング）。
- **イメージのメモリだけアロケータ非対応**: Phase 11 ③ でバッファは `GpuAllocator` 経由になったが、`Texture` と [depth_image.cpp](../../engine/src/graphics/depth_image.cpp) は**個別 `vkAllocateMemory`** のまま。
- **カリングなし**: 画面外のエンティティも全部 `DrawItem` に積んで描画している。

例によって Claude は宣言・骨格・TODO コメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### D-0. 着手順は ④ → ③ → ⑤ → ① → ②
| 順 | 項目 | 理由 |
|---|---|---|
| 1 | **④ イメージのアロケータ対応** | `GpuAllocator` 内に閉じる。見た目が変わらず、後続の土台になる |
| 2 | **③ ミップマップ生成** | `Texture` と `Sampler` に閉じる。見た目は良くなるが構造は変えない |
| 3 | **⑤ フラスタムカリング** | 収集フェーズへの「フィルタ追加」。描画結果は不変（見えないものを描かないだけ） |
| 4 | **① bindless テクスチャ** | ディスクリプタ構造の作り直し。壊れやすい山場 |
| 5 | **② インスタンシング** | **① に依存する**（インスタンスごとにテクスチャを変えるには bindless のインデックスが要る） |

**①→② の依存**: インスタンシングは「1回のドローで N 体描く」ため、体ごとに異なるデータ（model / base_color / **どのテクスチャか**）をシェーダ側で引く必要がある。テクスチャをディスクリプタのバインドで切り替えている限り、1ドローに複数テクスチャを混ぜられない。**bindless でテクスチャが「配列の添字」になって初めて、インスタンス化できる**。

### D-1. イメージとバッファは同じブロックに混ぜない（bufferImageGranularity）【重要】
`GpuAllocator` を `VkImage` にも使う際の最大の落とし穴。

Vulkan では **linear なリソース（バッファ）と optimal tiling なリソース（イメージ）を同じ `VkDeviceMemory` に置く場合、`VkPhysicalDeviceLimits::bufferImageGranularity` の境界で分離しなければならない**。守らないと、片方の書き込みがもう片方を壊す（実装依存で、動くGPUと壊れるGPUがある＝発見しにくい）。

対策として **`Block` に `bool linear` を持たせ、linear/non-linear を別ブロックに分ける**（granularity のアライメント計算を各確保で行うより単純で確実）。`allocate()` に「このリソースは linear か」を渡す。

### D-2. ミップマップは `vkCmdBlitImage` の連鎖で生成する
- レベル数: `mipLevels = floor(log2(max(width, height))) + 1`
- レベル `i-1` → レベル `i` へ、サイズを半分にしながら `vkCmdBlitImage` を繰り返す。
- **前提条件**: フォーマットが `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT` をサポートすること（`vkGetPhysicalDeviceFormatProperties` で確認）。非対応なら mip 1枚にフォールバックする。
- `VkImageCreateInfo::usage` に **`VK_IMAGE_USAGE_TRANSFER_SRC_BIT` を追加**する（自分自身が blit の転送元になるため）。
- 既存の `Texture::transition_image_layout` は `levelCount = 1` 固定・遷移パターン2種のハードコードなので、**mip レベル範囲を指定できる形へ拡張**する。
- `Sampler` の `maxLod` を `mipLevels` に、`mipmapMode` を `VK_SAMPLER_MIPMAP_MODE_LINEAR` にする（サンプラーは全テクスチャ共有なので、**最大のレベル数**に合わせる）。

### D-3. フラスタムカリングは「メッシュのバウンディング球」で行う
- `MeshRegistry::add()` の時点で**ローカル空間のバウンディング球**（中心・半径）を計算して `Entry` に持たせる。
- 収集フェーズで、`Transform` からワールド空間の球を求めて 6 平面と判定する。
  - 中心: `model * vec4(local_center, 1)`
  - 半径: `local_radius * max(scale.x, scale.y, scale.z)`（回転は球を変えない。非等方スケールは最大成分で保守的に）
- 平面は **view-projection 行列から Gribb–Hartmann 法で抽出**する。
- **★ 本プロジェクトは `GLM_FORCE_DEPTH_ZERO_TO_ONE`**（深度 [0,1]）なので、**near 平面の式が教科書と違う**（後述 ⑤-1）。ここを間違えると「近くのものが消える／遠くのものが残る」バグになる。
- 球は保守的（実際より大きめ）なので、**描画結果は変わらない**（見えるものが消えてはいけない）。これが検証の指針。

### D-4. bindless は「テクスチャ配列 + 添字」
- `set=1` を「テクスチャごとに1セット」から、**`binding=0` が `sampler2D` の配列である単一セット**に変える。セットは**全体で1つ**になり、フレーム先頭で1回バインドするだけになる。
- Vulkan 1.3 を要求しているので **descriptor indexing はコア機能**（拡張不要）。ただし**機能フラグの有効化が必要**（`VkPhysicalDeviceVulkan12Features`）。
- 使うフラグ:
  - `runtimeDescriptorArray`: シェーダ側で `sampler2D textures[]`（サイズ未指定）を書ける
  - `descriptorBindingPartiallyBound`: 配列の**一部しか埋まっていなくてよい**（64枠中3枚だけ登録、等）
  - `shaderSampledImageArrayNonUniformIndexing`: **同一ドロー内で添字が変わってよい**（インスタンシングに必須）
  - `descriptorBindingSampledImageUpdateAfterBind`: バインド済みセットへ後から書き込める（起動後にテクスチャを追加登録する場合に必要）
- シェーダ側は `#extension GL_EXT_nonuniform_qualifier : require` と `nonuniformEXT(index)`。
- `TextureId` が**そのまま配列の添字**になる（Phase 12 で `TextureId` を `entries_` の添字にしておいたのがここで効く）。

### D-5. インスタンシングは per-instance データを SSBO で渡す
- push constant は「1ドロー1組」なので、N体を1ドローで描くと使えない。**フレームごとのストレージバッファ（SSBO）に配列で置き、`gl_InstanceIndex` で引く**。
- `InstanceData { mat4 model; vec4 base_color; uint texture_index; }` を `std430` で配列に。
- **★ フレームインフライト分だけバッファを複製する**（`kFramesInFlight` 個）。1つを使い回すと、GPU が読んでいる最中に CPU が次フレームを書き込んで壊れる。カメラUBOと同じ理由。
- ドローは `vkCmdDrawIndexed(index_count, instanceCount, 0, 0, firstInstance)`。**Vulkan の `gl_InstanceIndex` は `firstInstance` を含む**ので、シェーダ側は `instances[gl_InstanceIndex]` でそのまま引ける。
- バッチの切れ目は **メッシュが変わるところだけ**（テクスチャは bindless で per-instance になるため、もはや切れ目にならない）。→ Phase 12 の不透明ソートは **mesh 優先**に変える。
- **半透明はインスタンス化できない**（描画順が正しさそのもので、まとめると順序が壊れる）。半透明パスは1体1ドローのまま。

---

## ④ テクスチャイメージメモリの `GpuAllocator` 対応

### ④-1. `GpuAllocator` に linear/non-linear の区別を入れる（[gpu_allocator.hpp](../../engine/include/sq/graphics/gpu_allocator.hpp) / [.cpp](../../engine/src/graphics/gpu_allocator.cpp)）

```cpp
// gpu_allocator.hpp
class GpuAllocator {
public:
    // linear: このリソースが linear tiling か（バッファ = true / optimal tiling のイメージ = false）。
    //   bufferImageGranularity の制約により、linear と non-linear を同じブロックに混ぜてはいけない（phase13 D-1）。
    //   既定 true は既存のバッファ呼び出しをそのまま通すため。
    [[nodiscard]] Allocation allocate(const VkMemoryRequirements& reqs,
                                      VkMemoryPropertyFlags properties,
                                      bool linear = true);
private:
    struct Block {
        // ...既存...
        bool linear = true;  // このブロックが linear 用か（混在させない）
    };
    // TODO: 空きブロック探索の条件に `block.linear == linear` を追加する。
    //       create_block にも linear を渡して Block へ記録する。
};
```

> `Allocation` 自体に変更は不要（`free()` は `block_index` から引くため）。

### ④-2. `Texture` をアロケータ経由にする（[texture.hpp](../../engine/include/sq/graphics/texture.hpp) / [.cpp](../../engine/src/graphics/texture.cpp)）

```cpp
// texture.hpp: メンバの VkDeviceMemory を Allocation に置き換える。
#include "sq/graphics/gpu_allocator.hpp"   // 前方宣言をやめて実体を include（Allocation を値で持つため）

class Texture {
    // ...
private:
    GpuAllocator* allocator_ = nullptr;  // 破棄時に free するため保持（所有しない）
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    Allocation allocation_{};   // ← VkDeviceMemory memory_ を置換
    VkImageView view_ = VK_NULL_HANDLE;
};
```

```cpp
// texture.cpp の変更点:
//   確保:  vkGetImageMemoryRequirements(device_, image_, &reqs);
//          allocation_ = allocator.allocate(reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
//                                           /*linear=*/false);   // ★ optimal tiling なので false
//          vkBindImageMemory(device_, image_, allocation_.memory, allocation_.offset);  // offset に注意
//   破棄:  vkDestroyImageView → vkDestroyImage → allocator_->free(allocation_)
```

### ④-3. `DepthImage` も同様にする（[depth_image.hpp](../../engine/include/sq/graphics/depth_image.hpp) / [.cpp](../../engine/src/graphics/depth_image.cpp)）
コンストラクタに `GpuAllocator&` を追加し、`vkAllocateMemory`/`vkFreeMemory` を `allocate(..., /*linear=*/false)` / `free()` に置き換える。
**注意**: `DepthImage` はスワップチェーン再生成のたびに作り直される（[renderer.cpp](../../engine/src/graphics/renderer.cpp) `recreate_swapchain`）。**確保と解放が繰り返される唯一のケース**なので、`GpuAllocator::free()` の実装（Phase 11 ③ で「隣接空きのマージは将来課題」とした部分）がここで効いてくる。リサイズを繰り返すと**空きリストが断片化して確保に失敗し得る**。

> **→ 隣接空きのマージ（coalescing）を実装する好機**。`free()` で、戻す区間と隣接する空き区間を1つに統合する（`free_ranges` を offset 順に保つと実装しやすい）。

### ④-4. 検証
- 起動時の `vkAllocateMemory` 呼び出し回数がさらに減ること（イメージも大ブロックへ相乗り）。
- **ウィンドウリサイズを何十回も繰り返しても確保に失敗しないこと**（断片化のテスト）。
- バリデーションでメモリ関連のエラー・リーク報告が無いこと。

---

## ③ ミップマップ生成

### ③-1. `Texture` に mip 対応を入れる（[texture.hpp](../../engine/include/sq/graphics/texture.hpp) / [.cpp](../../engine/src/graphics/texture.cpp)）

```cpp
// texture.hpp
class Texture {
public:
    // ...既存...
    [[nodiscard]] std::uint32_t mip_levels() const;  // Sampler の maxLod 決定に使う

private:
    // old_layout -> new_layout のイメージメモリバリアを記録する。
    // phase13 ③: mip レベル範囲を指定できるようにした（生成中はレベルごとに遷移させるため）。
    static void transition_image_layout(VkCommandBuffer command_buffer, VkImage image,
                                        VkImageLayout old_layout, VkImageLayout new_layout,
                                        std::uint32_t base_mip_level, std::uint32_t level_count);

    // ミップマップを vkCmdBlitImage の連鎖で生成する（phase13 ③ / D-2）。
    // 手順（レベル i = 1 .. mip_levels-1 について）:
    //   1. レベル i-1 を TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL へ遷移
    //   2. VkImageBlit を作る:
    //        srcSubresource.mipLevel = i-1, srcOffsets = {{0,0,0}, {w, h, 1}}
    //        dstSubresource.mipLevel = i,   dstOffsets = {{0,0,0}, {max(w/2,1), max(h/2,1), 1}}
    //      vkCmdBlitImage(cmd, image, TRANSFER_SRC_OPTIMAL, image, TRANSFER_DST_OPTIMAL,
    //                     1, &blit, VK_FILTER_LINEAR);
    //   3. レベル i-1 を TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL へ遷移（もう使わないので確定させる）
    //   4. w = max(w/2, 1); h = max(h/2, 1);
    // ループ後: 最後のレベル（mip_levels-1）を TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL へ遷移
    void generate_mipmaps(VkCommandBuffer command_buffer,
                          std::int32_t width, std::int32_t height);

    // フォーマットがリニアフィルタ blit に対応しているか（phase13 D-2）。
    // vkGetPhysicalDeviceFormatProperties の optimalTilingFeatures に
    // VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT が立っているかで判定する。
    // 非対応なら mip_levels_ = 1 にフォールバックする。
    [[nodiscard]] static bool supports_linear_blit(VkPhysicalDevice physical_device, VkFormat format);

    std::uint32_t mip_levels_ = 1;
};
```

```cpp
// texture.cpp のコンストラクタ変更点:
//   1. mip_levels_ を決める:
//        mip_levels_ = supports_linear_blit(physical_device, VK_FORMAT_R8G8B8A8_SRGB)
//            ? static_cast<std::uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1
//            : 1;
//   2. VkImageCreateInfo:
//        image_create_info.mipLevels = mip_levels_;
//        image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // ★ blit の転送元になる
//   3. 転送:
//        transition_image_layout(cmd, image_, UNDEFINED, TRANSFER_DST_OPTIMAL, 0, mip_levels_);
//        vkCmdCopyBufferToImage(...);            // レベル0だけ埋まる
//        if (mip_levels_ > 1) { generate_mipmaps(cmd, width, height); }
//        else { transition_image_layout(cmd, image_, TRANSFER_DST_OPTIMAL, SHADER_READ_ONLY_OPTIMAL, 0, 1); }
//        cmd.submit_and_wait();
//   4. VkImageViewCreateInfo:
//        view_create_info.subresourceRange.levelCount = mip_levels_;   // ★ 全レベルを見せる
```

### ③-2. `Sampler` を mip 対応にする（[sampler.hpp](../../engine/include/sq/graphics/sampler.hpp) / .cpp）

```cpp
// サンプラーは全テクスチャで共有するため、maxLod は「登録され得る最大のレベル数」に合わせる。
// VK_LOD_CLAMP_NONE を使えば実際のイメージの levelCount で自然に頭打ちになるので、
// テクスチャごとにサンプラーを分けなくて済む。
//   sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  // レベル間も補間（トライリニア）
//   sampler_info.minLod = 0.0f;
//   sampler_info.maxLod = VK_LOD_CLAMP_NONE;
//   sampler_info.mipLodBias = 0.0f;
```

### ③-3. 検証
- テクスチャを貼った板／キューブを**遠ざけたときのちらつき（モアレ）が消える**こと。
- 近づいたときの見え方が従来と変わらないこと。
- バリデーションでレイアウト遷移のエラーが出ないこと（**mip 生成中の遷移漏れが出やすい**）。
- `supports_linear_blit` が false になる環境でも落ちずに動くこと（mip 1枚で従来通り）。

---

## ⑤ フラスタムカリング

### ⑤-1. 新規 `frustum.hpp` / `frustum.cpp`（`engine/include/sq/scene/` と `engine/src/scene/`）

```cpp
#pragma once

#include <array>

#include <glm/glm.hpp>

namespace sq::scene {

// 軸に依存しない境界球（メッシュのローカル空間）。
struct BoundingSphere {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

// view-projection 行列から抽出した視錐台の6平面。
// 平面は vec4(a, b, c, d) で ax + by + cz + d = 0、法線 (a,b,c) は**内側向き**に正規化する。
class Frustum {
public:
    // view_projection から6平面を抽出する（Gribb–Hartmann 法）。
    //
    // GLMは列優先なので、行 row_i = (m[0][i], m[1][i], m[2][i], m[3][i]) を取り出して使う。
    //   left   = row3 + row0
    //   right  = row3 - row0
    //   bottom = row3 + row1
    //   top    = row3 - row1
    //   far    = row3 - row2
    //   near   = row2               ★ 深度 [0,1] の場合（GLM_FORCE_DEPTH_ZERO_TO_ONE）
    //                                 深度 [-1,1] の教科書式 (row3 + row2) ではない点に注意
    // 各平面は length(a,b,c) で割って正規化する（半径との比較を距離で行うため）。
    //
    // 注: Camera::view_projection() は proj[1][1] *= -1 でY反転している。
    //     これにより top/bottom の役割が入れ替わるが、6平面の集合としては同じなので判定に影響しない。
    [[nodiscard]] static Frustum from_view_projection(const glm::mat4& view_projection);

    // ワールド空間の球が視錐台と交差する（= 描画すべき）かを返す。
    // いずれかの平面について dot(plane.xyz, center) + plane.w < -radius なら完全に外側。
    // 球は保守的（実際の形状より大きい）なので、見えるものを誤って捨てることはない。
    [[nodiscard]] bool intersects(const glm::vec3& center, float radius) const;

private:
    std::array<glm::vec4, 6> planes_{};
};

}  // namespace sq::scene
```

### ⑤-2. `MeshRegistry` に境界球を持たせる（[mesh_registry.hpp](../../engine/include/sq/graphics/mesh_registry.hpp) / [.cpp](../../engine/src/graphics/mesh_registry.cpp)）

```cpp
// Entry に境界球を追加する。add() の中で頂点から計算する。
struct Entry {
    std::unique_ptr<VertexBuffer> vertices;
    std::unique_ptr<IndexBuffer> indices;
    scene::BoundingSphere bounds;  // ローカル空間。phase13 ⑤
};

// add() の中で計算する:
//   1. 全頂点の position の AABB（最小・最大）を求める
//   2. center = (min + max) * 0.5f
//   3. radius = max over vertices of length(v.position - center)
//      （AABBの対角長/2 でも可だが、頂点から直接求めた方が締まる）
```

### ⑤-3. 収集フェーズにカリングを入れる（[renderer.cpp](../../engine/src/graphics/renderer.cpp) `draw_frame`）

```cpp
// カメラ解決の直後に視錐台を作る:
//   const scene::Frustum frustum = scene::Frustum::from_view_projection(view_projection);
//
// 収集ラムダの中、DrawItem を作る前に判定する:
//   const auto& entry = meshes_->get(mh.id);
//   const glm::mat4 model = t.model();
//   // ワールド空間の球（D-3）
//   const glm::vec3 world_center = glm::vec3(model * glm::vec4(entry.bounds.center, 1.0f));
//   const float max_scale = std::max({ t.scale.x, t.scale.y, t.scale.z });
//   const float world_radius = entry.bounds.radius * max_scale;
//   if (!frustum.intersects(world_center, world_radius)) { return; }   // 画面外なのでスキップ
//
// TODO（任意）: カリングした数／した数を数えて表示すると効果が確認しやすい。
```

### ⑤-4. 検証
- **カメラを回しても、見えているものが消えないこと**（これが最重要。消えるなら near 平面の式か法線の向きが誤り）。
- 視野外のエンティティが `DrawItem` に積まれないこと（カウンタで確認）。
- エンティティを大量（数千体）に増やしたとき、視野外が多い角度で**CPU側の処理時間が減る**こと。
- カメラを切り替えても（Phase 10 の複数カメラ）正しくカリングされること。

---

## ① bindless テクスチャ

### ①-1. デバイス機能の有効化（[device.hpp](../../engine/include/sq/graphics/device.hpp) / [.cpp](../../engine/src/graphics/device.cpp)）

```cpp
// device.cpp の VkDeviceCreateInfo に descriptor indexing 機能を繋ぐ（phase13 D-4）。
//
//   VkPhysicalDeviceVulkan12Features features12{};
//   features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
//   features12.runtimeDescriptorArray = VK_TRUE;
//   features12.descriptorBindingPartiallyBound = VK_TRUE;
//   features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
//   features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
//   device_create_info.pNext = &features12;
//
// ★ 注意: pEnabledFeatures（既存の samplerAnisotropy）はそのまま使ってよい。
//   pNext に VkPhysicalDeviceFeatures2 を繋ぐ場合だけ pEnabledFeatures を nullptr にする必要がある。
//   VkPhysicalDeviceVulkan12Features を直接繋ぐ分には併用できる。
//
// TODO: 対応状況を vkGetPhysicalDeviceFeatures2 で問い合わせ、非対応なら例外を投げるか
//       従来方式にフォールバックする（学習段階では「非対応なら throw」で十分）。
//       PhysicalDeviceSelector::is_suitable の条件に加えてもよい。
```

### ①-2. `set=1` をテクスチャ配列にする（[renderer.cpp](../../engine/src/graphics/renderer.cpp) `create_descriptor_set_layout` / `create_descriptor_pool`）

```cpp
// レイアウト（set=1, binding=0 を配列にする）:
//   VkDescriptorSetLayoutBinding b{};
//   b.binding = 0;
//   b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
//   b.descriptorCount = kMaxTextures;          // ★ 1 ではなく配列長
//   b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
//
//   // 部分的にしか埋まっていなくてよい／バインド後に更新してよい
//   VkDescriptorBindingFlags flags =
//       VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
//       VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
//   VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
//   flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
//   flags_info.bindingCount = 1;
//   flags_info.pBindingFlags = &flags;
//
//   layout_info.pNext = &flags_info;
//   layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;  // ★ 必須
//
// プール（UPDATE_AFTER_BIND を使うならプールにもフラグが要る）:
//   pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
//   // sampler の descriptorCount は kMaxTextures のまま、maxSets は
//   // kFramesInFlight（set=0）+ 1（set=1 は全体で1つ）に減らせる。
```

### ①-3. `TextureRegistry` を「共有1セットの配列要素を書く」形に変える（[texture_registry.hpp](../../engine/include/sq/graphics/texture_registry.hpp) / [.cpp](../../engine/src/graphics/texture_registry.cpp)）

```cpp
// texture_registry.hpp の変更:
//   - Entry から VkDescriptorSet set を削除（セットはテクスチャごとに持たない）
//   - コンストラクタで「共有の set=1」を1つだけ確保して保持する
//   - descriptor_set(id) を廃止し、代わりに全体で1つのセットを返す:
class TextureRegistry {
public:
    // bindless の共有ディスクリプタセット（set=1）。フレーム先頭で1回バインドするだけでよい。
    [[nodiscard]] VkDescriptorSet bindless_set() const;

    // load() は変更なし（TextureId を返す）。ただし内部の書き込み先が変わる:
    //   vkUpdateDescriptorSets で dstSet = bindless_set_, dstBinding = 0,
    //   ★ dstArrayElement = id（TextureId がそのまま配列の添字）
    //   descriptorCount = 1 で「配列の id 番目」に書き込む。

private:
    struct Entry {
        std::unique_ptr<Texture> texture;   // set は持たない
    };
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;  // 全テクスチャ共有の1セット
};
```

### ①-4. シェーダの変更（[triangle.frag](../../shaders/triangle.frag)）

```glsl
#version 450
// bindless: サイズ未指定の配列と非一様添字（phase13 D-4）
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    uint texture_index;   // ★ 追加（どのテクスチャを使うか）
} pc;

void main() {
    // nonuniformEXT: 同一ドロー内で添字が変わり得ることをコンパイラへ伝える。
    // push constant 由来ならドロー内で一様だが、②インスタンシングで
    // per-instance の添字になると必須になるので最初から付けておく。
    out_color = texture(textures[nonuniformEXT(pc.texture_index)], frag_uv) * pc.base_color;
}
```

### ①-5. push constant に `texture_index` を追加（[push_constants.hpp](../../engine/include/sq/graphics/push_constants.hpp)）

```cpp
struct PushConstants {
    glm::mat4 model;          // offset  0, 64
    glm::vec4 base_color;     // offset 64, 16
    std::uint32_t texture_index;  // offset 80, 4   ← 追加
};
static_assert(sizeof(PushConstants) == 84 || sizeof(PushConstants) == 96,
              "パディングに注意。シェーダのブロックと一致させること");
// ★ std430/push constant のレイアウト規則により末尾にパディングが入り得る。
//   静的アサートで実測し、シェーダ側の宣言と一致させること。
```

### ①-6. `record_draw_items` の簡素化（[renderer.cpp](../../engine/src/graphics/renderer.cpp)）
```cpp
// テクスチャごとの vkCmdBindDescriptorSets が**丸ごと不要**になる。
//   - フレーム先頭で set=0（カメラ）と set=1（bindless）を1回ずつバインドする
//   - ループ内は「メッシュが変わったらバインド」+ push constant + draw だけ
//   - bound_texture の追跡が不要になる
// 不透明ソートの基準も (texture, mesh) → **mesh のみ**でよくなる（テクスチャは切れ目でなくなる）。
```

### ①-7. 検証
- 見た目が Phase 12 と**完全に同じ**であること（テクスチャの割り当てが変わらない）。
- `vkCmdBindDescriptorSets` の呼び出しがフレームあたり2回（set=0/set=1）に減ること。
- バリデーションで descriptor indexing 関連のエラーが出ないこと（**機能の有効化漏れが出やすい**）。
- テクスチャを `kMaxTextures` 近くまで登録しても動くこと。

---

## ② インスタンシング

### ②-1. per-instance データのバッファ（新規 `instance_buffer.hpp` / `.cpp`、または `Buffer` 派生）

```cpp
// engine/include/sq/graphics/instance_data.hpp（新規）
#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace sq::graphics {

// 1インスタンス分の描画データ（phase13 D-5）。SSBO の配列要素になる。
// ★ シェーダの std430 レイアウトと完全に一致させること。
//   vec4 は16バイト境界、mat4 は16バイト境界の vec4 x4。
struct InstanceData {
    glm::mat4 model;              // offset  0, 64
    glm::vec4 base_color;         // offset 64, 16
    std::uint32_t texture_index;  // offset 80, 4
    std::uint32_t _pad[3];        // offset 84, 12（16バイト境界に揃える）
};

static_assert(sizeof(InstanceData) == 96, "std430 のレイアウトと一致させること");

}  // namespace sq::graphics
```

```cpp
// Buffer 派生として「HOST_VISIBLE な STORAGE_BUFFER で永続マップ」を作る。
// UniformBuffer とほぼ同じ構造（usage が VK_BUFFER_USAGE_STORAGE_BUFFER_BIT になるだけ）。
class InstanceBuffer : public Buffer {
public:
    InstanceBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_instances);
    ~InstanceBuffer();
    void update(const InstanceData* data, std::size_t count);  // memcpy（HOST_COHERENT）
    [[nodiscard]] std::size_t capacity() const;
private:
    void* mapped_ = nullptr;
    std::size_t capacity_ = 0;
};
```

> **★ `kFramesInFlight` 個作る**（カメラUBOと同じ理由。GPU が読んでいる最中に上書きしない）。

### ②-2. ディスクリプタに SSBO を追加（[renderer.cpp](../../engine/src/graphics/renderer.cpp)）
```cpp
// set=0 に binding=1 として STORAGE_BUFFER を追加する（カメラと同じ「フレームごとに変わるもの」なので同居が自然）。
//   VkDescriptorSetLayoutBinding instance_binding{};
//   instance_binding.binding = 1;
//   instance_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
//   instance_binding.descriptorCount = 1;
//   instance_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
//
// プールに STORAGE_BUFFER × kFramesInFlight を追加。
// create_descriptor_sets で instance_buffers_[i] を binding=1 へ書き込む。
```

### ②-3. シェーダの変更
```glsl
// triangle.vert
struct InstanceData {
    mat4 model;
    vec4 base_color;
    uint texture_index;
};
layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 3) out flat uint frag_texture_index;   // frag へ添字を渡す
layout(location = 4) out vec4 frag_base_color;

void main() {
    // ★ Vulkan の gl_InstanceIndex は firstInstance を含む
    InstanceData inst = instances[gl_InstanceIndex];
    gl_Position = camera.view_proj * inst.model * vec4(in_position, 1.0);
    frag_uv = in_uv;
    frag_texture_index = inst.texture_index;
    frag_base_color = inst.base_color;
}
```

```glsl
// triangle.frag: push constant をやめ、vert から受け取った値を使う
layout(location = 3) in flat uint frag_texture_index;
layout(location = 4) in vec4 frag_base_color;

void main() {
    // ★ ここで nonuniformEXT が本当に必要になる（インスタンスごとに添字が変わるため）
    out_color = texture(textures[nonuniformEXT(frag_texture_index)], frag_uv) * frag_base_color;
}
```

> push constant は不要になる（`PushConstants` を廃止するか、デバッグ用途に残すかは選択）。
> 廃止するなら `VkPushConstantRange` も 0 にする。

### ②-4. `record_draw_items` をバッチドローに書き換え（[renderer.cpp](../../engine/src/graphics/renderer.cpp)）

```cpp
// 不透明パスの記録手順（phase13 ②）:
//   1. opaque_items_ を **mesh でソート**する（テクスチャはもはや切れ目にならない）
//   2. InstanceData の配列を作り、instance_buffers_[current_frame_]->update(...) で一括転送する
//      （DrawItem の並び順 = InstanceData の並び順にする）
//   3. 同じ mesh が連続する範囲を数え、範囲ごとに1回だけドローする:
//        vkCmdDrawIndexed(command_buffer, index_count,
//                         instance_count,    // ← その範囲の体数
//                         0, 0,
//                         first_instance);   // ← InstanceData 配列の開始位置
//      メッシュのバインドは範囲の先頭で1回。
//
// ★ 半透明パスはインスタンス化しない（描画順が正しさそのもの。D-5）。
//   ただし InstanceData 経由でデータを渡す形は共通化できる
//   （instanceCount = 1, firstInstance = そのアイテムの添字 で1件ずつ描く）。
//   こうするとシェーダを1本に保てる。
//
// TODO: max_instances を超えた場合の扱い（切り捨て + 警告、またはバッファ再確保）を決める。
```

### ②-5. 検証
- 見た目が ① の時点と**完全に同じ**であること。
- **`vkCmdDrawIndexed` の呼び出し回数が激減**すること（同じメッシュのエンティティ数 → メッシュ種類数）。
  カウンタを出すか、RenderDoc でドローコール数を確認する。
- 同じメッシュでもエンティティごとに**別テクスチャ・別色・別TRS**が保たれること（これが崩れたら `gl_InstanceIndex` か `firstInstance` の扱いが誤り）。
- 半透明の back-to-front が維持されていること。
- エンティティを数千体にしても破綻しないこと（`max_instances` の上限確認）。

---

## 実装手順（この順で、各ステップ完了ごとに動作確認・コミット）

1. **④ イメージのアロケータ対応** — `GpuAllocator` に linear 区別を追加 → `Texture` → `DepthImage` → 隣接空きマージ → **リサイズ連打テスト**
2. **③ ミップマップ** — `transition_image_layout` の mip 対応 → `generate_mipmaps` → `Sampler` の maxLod → **遠景のちらつき解消を確認**
3. **⑤ フラスタムカリング** — `Frustum`/`BoundingSphere` → `MeshRegistry` の bounds → 収集フェーズに判定 → **見えるものが消えないことを確認**
4. **① bindless** — デバイス機能 → レイアウト/プール → `TextureRegistry` → シェーダ → `record_draw_items` 簡素化 → **見た目不変 + バインド回数減を確認**
5. **② インスタンシング** — `InstanceData`/`InstanceBuffer` → ディスクリプタ追加 → シェーダ → バッチドロー → **見た目不変 + ドローコール激減を確認**

> **4 が唯一の「壊れやすい山場」**（Phase 12 の手順3と同じ性質）。3 まで終えた状態でコミットして区切ってから着手すること。

---

## 検証観点（フェーズ全体）
- **見た目が Phase 12 と変わらないこと**（③のミップマップによる遠景の改善を除く）。①②⑤ はいずれも「同じ絵をより速く描く」変更であり、絵が変わったらバグ。
- ドローコール数・ディスクリプタバインド回数・`vkAllocateMemory` 回数がいずれも減っていること。
- リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab 復帰で描画とカメラ操作が継続すること。
- 終了時にバリデーションのリーク報告が無いこと。
- エンティティ数を大きく増やしたとき（数千体）に破綻しないこと。

---

## Phase 14 で行うこと（このフェーズではやらない・確定事項）

以下は **plan14** で実装する。いずれもデータ構造・アセット管理側の整備で、Phase 13 の描画効率化とは独立している。

1. **マテリアルUBO**
   - Phase 13 ② で per-instance データは SSBO に載るが、マテリアル固有のパラメータ（metallic / roughness / emissive / alpha_cutoff 等）が増えると `InstanceData` が肥大化する。
   - 「マテリアル定義」を別バッファに分離し、`InstanceData` は `material_index` だけを持つ形へ。同じマテリアルを共有する体でデータを重複させない。

2. **アセットのアンロード / 世代付きハンドル**
   - `MeshId` / `TextureId` は現状「`entries_` の添字」で、**登録のみ・解放なし**（Phase 12 D-1 で明記した割り切り）。
   - 解放を入れると添字が再利用され、古いハンドルが別アセットを指す危険（ABA問題）が生じる。`{ index, generation }` のペアにして解決する。
   - 空きスロットのフリーリスト、参照カウント（誰も使っていないテクスチャの破棄）も検討。

3. **モデルファイルのロード（glTF / OBJ）**
   - 現状 `add_cube_mesh` / `add_plane_mesh` の手書きジオメトリのみ。
   - glTF（推奨: PBRマテリアル・階層・複数プリミティブを持つ）を読み、`MeshRegistry::add` / `TextureRegistry::load` へ流し込む。
   - ノード階層をそのまま扱うには 4 の親子階層が必要になる。

4. **Transform の行列キャッシュ + dirty フラグ、および親子階層**
   - 現状 `Transform::model()` は**毎フレーム毎エンティティで T*R*S を合成**している（Phase 12 D-4 で「将来課題」とした部分）。
   - 変更があったときだけ再計算する（dirty フラグ）。
   - `Parent` コンポーネントでワールド行列を伝播させる。親が動いたら子も dirty にする必要があり、**更新順序（トポロジカル順）**の管理が要る。
   - glTF のノード階層をそのまま表現できるようになる。

## さらに先の将来課題（plan15以降）
- **専用トランスファーキュー**（キューファミリ跨ぎの所有権移譲）。現状は起動時に graphics キューへ submit して待つだけ。
- **`GpuAllocator` → VMA 差し替え**（Phase 11 ③ で確保点を1箇所に閉じてある）。
- **OIT（順序独立透過）** — CPU ソートに依らない半透明。Phase 13 ② で半透明だけインスタンス化できない制約の解消にもなる。
- **キーコンフィグの設定ファイル入出力（JSON等）**（Phase 10 からの継続課題）。
- **エンティティの生成・破棄を行う System**（`Registry&` を非constで受ける形への拡張）。
- **オクルージョンカリング / LOD** — Phase 13 ⑤ のカリング基盤の延長。
