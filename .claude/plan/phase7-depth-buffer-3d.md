# C++ ゲームエンジン（学習目的）— Phase 7: 深度バッファ + 3D化（vec3頂点・インデックスバッファ・立方体）

## Context
Phase 5でカメラ（UBO + Descriptor Set）、Phase 6でリファクタリング（`Buffer`基底クラス等）、その後フルスクリーン対応（[fullscreen-exclusive.md](fullscreen-exclusive.md)）まで完了した。現在の描画は「2D頂点（vec2, z=0平面）の三角形を透視カメラで見る」状態。

Phase 7では本格的な3D描画に進む:
1. **深度バッファ** — 立体が重なったとき、手前のものが正しく手前に描かれるようにする（現状は描画順で上書きされるだけ）
2. **頂点のvec3化** — z座標を持つ本物の3Dメッシュを扱えるようにする
3. **インデックスバッファ** — 立方体（8頂点・12三角形）を効率よく表現する。Phase 6で作った`Buffer`基底クラスの再利用例にもなる
4. **立方体の描画** — 回転カメラから見て遮蔽関係が正しい立方体群を出す

`GLM_FORCE_DEPTH_ZERO_TO_ONE` はPhase 5でCMakeに定義済み。カメラのY反転補正も済んでいるため、深度範囲まわりの下準備は整っている。

例によってClaudeは宣言・骨格・TODOコメントまで、実装本体はユーザーが書く。

## 実装対象

### 1. 深度フォーマットの選択（`PhysicalDeviceSelector` または新規ヘルパー）

```cpp
// 候補から、optimal tilingで DEPTH_STENCIL_ATTACHMENT に使える最初のフォーマットを返す。
// 候補: VK_FORMAT_D32_SFLOAT → VK_FORMAT_D32_SFLOAT_S8_UINT → VK_FORMAT_D24_UNORM_S8_UINT
// vkGetPhysicalDeviceFormatProperties の optimalTilingFeatures に
// VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT が立っているかで判定。
[[nodiscard]] static VkFormat find_depth_format(VkPhysicalDevice physical_device);
```

### 2. 新規 `DepthImage` クラス（`engine/include/sq/graphics/depth_image.hpp` / `src/graphics/depth_image.cpp`）

`VkImage` + `VkDeviceMemory` + `VkImageView` を所有するRAIIクラス。`Buffer`はVkBuffer専用なので継承はしない（VkImageは `vkCreateImage`/`vkGetImageMemoryRequirements`/`vkBindImageMemory` と別系統のAPI）。ただし `find_memory_type` は再利用したいので、**`Buffer::find_memory_type` を `public static` に昇格**して呼ぶ。

```cpp
class DepthImage {
public:
    DepthImage(VkPhysicalDevice physical_device, VkDevice device,
               VkExtent2D extent, VkFormat depth_format);
    ~DepthImage();  // view → image → memory の順で破棄

    // コピー禁止

    [[nodiscard]] VkImageView view() const;
    [[nodiscard]] VkFormat format() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
};
```

コンストラクタの手順（TODO骨格に明記する内容）:
1. `VkImageCreateInfo`（imageType=2D, extent, mipLevels=1, arrayLayers=1, format, tiling=OPTIMAL, usage=`VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, samples=1, initialLayout=UNDEFINED）→ `vkCreateImage`
2. `vkGetImageMemoryRequirements` → `Buffer::find_memory_type(..., DEVICE_LOCAL)` → `vkAllocateMemory` → `vkBindImageMemory`
3. `VkImageViewCreateInfo`（aspectMask=`VK_IMAGE_ASPECT_DEPTH_BIT`）→ `vkCreateImageView`
- レイアウト遷移は明示的には不要（レンダーパスの initialLayout=UNDEFINED → 自動遷移に任せる）

### 3. `RenderPass` に深度アタッチメントを追加

コンストラクタを `RenderPass(VkDevice, VkFormat color_format, VkFormat depth_format)` に変更。

- `VkAttachmentDescription depth_attachment`: format=depth_format, loadOp=CLEAR, **storeOp=DONT_CARE**（深度はフレームを跨いで使わない）, stencil両方DONT_CARE, initialLayout=UNDEFINED, finalLayout=`VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`
- `VkAttachmentReference depth_ref{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL }`
- `subpass.pDepthStencilAttachment = &depth_ref;`
- `pAttachments` を配列2要素（color, depth）に変更
- **依存関係の拡張（重要・検証レイヤーに指摘される定番ポイント）**:
  - `srcStageMask` / `dstStageMask` に `VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT` を追加
  - `dstAccessMask` に `VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT` を追加
  - 理由: 前フレームの深度テスト完了前に今フレームが深度をクリアしないようにするため

### 4. `GraphicsPipeline` に深度ステンシルステートを追加

```cpp
VkPipelineDepthStencilStateCreateInfo depth_stencil{};
depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
depth_stencil.depthTestEnable = VK_TRUE;
depth_stencil.depthWriteEnable = VK_TRUE;
depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;  // 小さい深度=手前が勝つ
depth_stencil.depthBoundsTestEnable = VK_FALSE;
depth_stencil.stencilTestEnable = VK_FALSE;

pipeline_info.pDepthStencilState = &depth_stencil;  // 現状nullptrのフィールド
```

ついでに [graphics_pipeline.cpp の cullMode](../../engine/src/graphics/graphics_pipeline.cpp) が `VK_FRONT_FACE_COUNTER_CLOCKWISE`（enum取り違え、偶然CULL_NONE）のままなら、立方体導入と同時に `VK_CULL_MODE_BACK_BIT` + `frontFace` の整合を取ると裏面カリングの学習になる（立方体の頂点巻き順と合わせること。最初はCULL_NONEで表示を確認してからカリングを有効化する手順を推奨）。

### 5. `Renderer` の変更

- メンバ追加: `std::unique_ptr<DepthImage> depth_image_;` と `VkFormat depth_format_;`
- コンストラクタ: swapchain作成後に `find_depth_format` → `DepthImage` 生成 → RenderPassへcolor+depth両フォーマットを渡す
- `create_framebuffers()`: attachmentsを `{ color_view, depth_image_->view() }` の2要素に。**深度は1枚を全スワップチェーン画像で共有**（フレーム間でGPU実行が直列化されるため衝突しない。frames in flight=2でも、サブパス依存関係で守られる）
- `recreate_swapchain()`: `destroy_framebuffers()` の後・`create_framebuffers()` の前に `depth_image_` を新extentで作り直す
- `draw_frame()`: クリア値を2要素に:

```cpp
std::array<VkClearValue, 2> clear_values{};
clear_values[0].color = { {0.2f, 0.2f, 0.2f, 1.0f} };
clear_values[1].depthStencil = { 1.0f, 0 };  // far=1.0でクリア（GLM_FORCE_DEPTH_ZERO_TO_ONE前提）
render_pass_begin_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
render_pass_begin_info.pClearValues = clear_values.data();
```

- デストラクタ: `depth_image_` はフレームバッファ破棄の後・RenderPass破棄の前あたりで `reset()`

### 6. 頂点のvec3化 + シェーダー

- [mesh.hpp](../../engine/include/sq/graphics/mesh.hpp): `struct Vertex { glm::vec3 position; glm::vec3 color; };`
- [graphics_pipeline.cpp](../../engine/src/graphics/graphics_pipeline.cpp): attribute 0 を `VK_FORMAT_R32G32B32_SFLOAT` に（strideは `sizeof(Vertex)` のままでOK）
- [triangle.vert](../../shaders/triangle.vert): `layout(location=0) in vec3 in_position;` / `gl_Position = camera.view_proj * pc.model * vec4(in_position, 1.0);`

### 7. 新規 `IndexBuffer` クラス（`Buffer` 基底の再利用例）

`mesh.hpp` に追加（`VertexBuffer` と並べる）:

```cpp
// インデックスデータをGPUメモリに保持するRAIIラッパー。Buffer基底の3例目。
class IndexBuffer : public Buffer {
public:
    IndexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                const std::vector<std::uint16_t>& indices);  // usage=INDEX_BUFFER_BIT

    void bind(VkCommandBuffer command_buffer) const;  // vkCmdBindIndexBuffer(..., VK_INDEX_TYPE_UINT16)
    [[nodiscard]] std::uint32_t index_count() const;

private:
    std::uint32_t index_count_ = 0;
};
```

コンストラクタ本体は `VertexBuffer` と同型（`map()` → memcpy → `unmap()`）。

### 8. 立方体メッシュ

`Renderer::create_triangle_mesh()` を `create_cube_mesh()` に改め（または併設）、頂点8個 + インデックス36個（12三角形）で立方体を定義。面ごとに色を変えると遮蔽関係の確認がしやすい（その場合は頂点24個・面ごとに4頂点）。描画は:

```cpp
triangle_mesh_->bind(cb);          // 頂点
cube_indices_->bind(cb);           // インデックス
vkCmdDrawIndexed(cb, cube_indices_->index_count(), 1, 0, 0, 0);
```

### 9. `sandbox_graphics/main.cpp`

- 既存のPosition/Velocityの回転ロジックはそのまま流用可能（Positionは既にvec3）
- エンティティのY座標をばらけさせる（例: `y = (i % 3 - 1) * 1.5f`）と、立方体同士の遮蔽が全方位で確認できる

### 10. CMake

- `engine/CMakeLists.txt` に `src/graphics/depth_image.cpp` を追加

## 実装順序（推奨）

1. **深度バッファを先に単独で**（1〜5）: 既存の三角形のまま導入し、ビルド・実行・リサイズ・フルスクリーン切替で検証エラーが出ないことを確認（見た目は変わらない）
2. **vec3化**（6）: 三角形のまま頂点をvec3に。表示が変わらないことを確認
3. **IndexBuffer + 立方体**（7〜9): 立方体を出し、回転カメラで遮蔽関係が正しいことを確認
4. カリング有効化（4の後半）: 巻き順を確認しながら `CULL_MODE_BACK` に

## 検証方法

- 手順1完了時: 従来の三角形描画が維持され、リサイズ・最小化・フルスクリーン切替・Alt+Tab復帰すべてで検証レイヤーのエラーが出ないこと（特にレンダーパス依存関係のsync系VUID）
- 手順3完了時: 立方体が回転カメラから見て正しく遮蔽される（奥の面・奥の立方体が手前に透けない）こと
- 深度クリア漏れの確認: 数分放置してもゴミが蓄積しないこと
- `ctest` 全パス（ECS非変更）

## 将来課題（このフェーズではやらない）

- テクスチャ（VkSampler / combined image sampler、ステージングバッファ転送）
- ステージングバッファによる DEVICE_LOCAL 頂点/インデックスバッファ（`Buffer` の転送対応）
- Meshコンポーネント化（エンティティごとに別メッシュを持つ。現在は共有メッシュ+Transform）
- 深度フォーマットのステンシル対応（S8を選んだ場合のaspectMask等）
