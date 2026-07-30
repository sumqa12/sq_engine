# C++ ゲームエンジン（学習目的）— Phase 11: 描画最適化（DEVICE_LOCAL化 + 自前サブアロケータ + 半透明ソート）

## Context
Phase 10 で System 抽象・キーコンフィグ層・Orbit スキーム・`ActiveCamera` 選択・`SingleTimeCommands` まで到達した（[phase10-system-abstraction-keyconfig-camera.md](phase10-system-abstraction-keyconfig-camera.md) 参照）。Phase 11 では、その末尾で予告した **描画パイプラインと GPU メモリ管理の最適化** 3 項目を実装する。3 項目は互いに独立している。

現状の「暫定形」:

- **頂点/インデックスバッファが `HOST_VISIBLE | HOST_COHERENT` に直書き**（[mesh.cpp](../../engine/src/graphics/mesh.cpp)）。CPU から見えるメモリは GPU ローカルより遅い。DEVICE_LOCAL へ staging 転送すべき。
- **小さなバッファごとに個別 `vkAllocateMemory`**（[buffer.cpp](../../engine/src/graphics/buffer.cpp) の `Buffer` コンストラクタ）。Phase 7 で観測した `small-dedicated-allocation` 警告の原因。1 つの大きな `VkDeviceMemory` を確保してサブアロケートすべき。
- **半透明が未ソート・`depthWrite=TRUE` のまま**（[phase8-texture-mapping.md](phase8-texture-mapping.md) の「3. 深度書き込みと描画順」）。視点角度によって背後が正しく透けない。不透明パス → 半透明パス（back-to-front ソート）に分離すべき。

例によって Claude は宣言・骨格・TODO コメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### A. 着手順は ② → ③ → ①
機械的で確実な **② DEVICE_LOCAL化** から入り、次に **③ 自前サブアロケータ** でメモリ確保を整理し、最後に設計の重い **① 半透明ソート** を行う。理由: ②③ は `Buffer` 基底クラス周辺に閉じ、既存の描画結果を変えない（見た目は同じまま内部を高速化）。土台を固めてから、描画順という「見た目が変わる」変更に進む。

**② → ③ の関係（シグネチャ変更が 2 回起きる点に注意）**:
- ② で `VertexBuffer`/`IndexBuffer` のコンストラクタに `queue_family` と `queue` を追加する（staging 転送に `SingleTimeCommands` が必要なため）。この時点では従来通り `VkPhysicalDevice` を使って `find_memory_type` する。
- ③ で `Buffer` 基底のメモリ確保を `GpuAllocator` に置き換える。ここで **全バッファのコンストラクタから `VkPhysicalDevice` が消え、`GpuAllocator&` に替わる**。② で触った mesh のシグネチャも再度変わる。
- 一度に両方やらず、②を完成・確認してから③に進むこと（各ステップで描画が壊れていないことを確認できる）。

### B. DEVICE_LOCAL 化は staging + `SingleTimeCommands`（Phase 10 の資産を再利用）
- `StagingBuffer`（`TRANSFER_SRC`, `HOST_VISIBLE`）にデータを書き込む → `vkCmdCopyBuffer` で DEVICE_LOCAL バッファへコピー → `SingleTimeCommands::submit_and_wait()`。
- 転送手順は `VertexBuffer` と `IndexBuffer` で同一なので、**`Buffer` 基底に protected ヘルパ `upload_with_staging()` を1つ用意**して両者から呼ぶ（重複を作らない。texture.cpp の `vkCmdCopyBufferToImage` とは対象が違う=バッファなので別ヘルパ）。
- 転送は起動時の一度きり。`vkQueueWaitIdle` で CPU をブロックしてよい（毎フレームの描画では使わない）。

### C. 自前サブアロケータ（将来 VMA へ差し替え可能な設計）
- **`GpuAllocator`**: メモリタイプごとに大きな `VkDeviceMemory` ブロックを確保し、その中の部分区間（offset）を貸し出す。空き管理は単純な **フリーリスト**（学習目的で自作。将来 vcpkg の `vulkan-memory-allocator` に差し替える）。
- **`Allocation`**: 「どのブロックの、どの offset の、何バイトか」を表す値型。`Buffer` は生の `VkDeviceMemory memory_` の代わりに `Allocation allocation_` を持つ。
- **差し替え点を `Buffer` のメモリ確保部分だけに閉じる**（Phase 6 の `Buffer` 共通化がここで効く）。`vkAllocateMemory`/`vkFreeMemory` を `allocator.allocate()`/`allocator.free()` に置換し、`vkBindBufferMemory(buffer, alloc.memory, alloc.offset)` にする。
- **所有権**: `GpuAllocator` は `Device` が持つ（`VkPhysicalDevice`+`VkDevice` を握っているため自然）。`device_->allocator()` で公開。破棄順序は「全バッファ解放 → `Device` 破棄（=allocator 破棄）」。現在の `Renderer` デストラクタは既にこの順（`triangle_mesh_.reset()` 等が `device_.reset()` より前）なので、この不変条件を **崩さないこと**。
- **HOST_VISIBLE ブロックは生成時に1回だけ全体を map して保持**（persistent mapping）。`map(alloc)` は `block_base_ptr + alloc.offset` を返すだけにする。`UniformBuffer` の永続マップと相性が良い。
- **alignment**: `allocate()` は `VkMemoryRequirements::alignment` を満たす offset を返さねばならない。ブロック内 offset を切り上げる。
- **大きすぎる要求**（ブロックサイズ超）は、そのブロックだけ専用確保にフォールバックする（学習段階では「要求サイズ > 既定ブロックサイズなら専用の `VkDeviceMemory` を1個確保して丸ごと1 Allocation にする」で十分）。

### D. 半透明は `Material` コンポーネント + 2 パイプライン + back-to-front ソート
- **`sq::scene::Material`**: `bool transparent`（+将来のベースカラー/テクスチャID の置き場）。`Camera` などと同じ ECS コンポーネントとして `sq::scene` に置く。`Material` を持たないエンティティは **不透明扱い**（後方互換）。
- **パイプライン 2 本**: `depthWriteEnable` はコア Vulkan では動的ステートにできない（`VK_EXT_extended_dynamic_state` が要る）ので、動的切替ではなく **不透明用・半透明用の 2 本の `VkPipeline`** を持つ。
  - 不透明: `depthTestEnable=TRUE`, `depthWriteEnable=TRUE`, ブレンド無効。
  - 半透明: `depthTestEnable=TRUE`, `depthWriteEnable=FALSE`, アルファブレンド有効。
  - `GraphicsPipeline` のコンストラクタに **設定構造体 `PipelineConfig`**（depth write / blend の有無）を追加し、1 クラスで両方を作れるようにする。
- **描画順**（`draw_frame` 内、`ActiveCamera` の位置を基準に）:
  1. **収集**: `view<Transform, Position>` を回し、各エンティティを「不透明」「半透明」に振り分ける（`Material` を持ち `transparent==true` なら半透明）。
  2. **不透明パス**: 不透明パイプラインをバインドし、不透明エンティティを順不同で描画（深度テストが前後関係を解決）。
  3. **半透明パス**: 半透明パイプラインをバインドし、半透明エンティティを **カメラ位置からの距離で降順（遠い順）にソート** してから描画。
- ソートの距離基準は **Phase 10 で確定した `ActiveCamera` の `position`**（どのカメラからの距離かが一意に決まる）。

---

## ② 頂点/インデックスバッファの DEVICE_LOCAL 化

### ②-1. `Buffer` に staging 転送ヘルパを追加（[buffer.hpp](../../engine/include/sq/graphics/buffer.hpp) / [buffer.cpp](../../engine/src/graphics/buffer.cpp)）

```cpp
// buffer.hpp の Buffer に protected メソッドを追加する:
protected:
    // 既に (TRANSFER_DST | ... , DEVICE_LOCAL) で生成済みのこのバッファへ、
    // staging 経由で data を size バイト転送する。起動時アセット用（vkQueueWaitIdle でブロック）。
    // 手順:
    //   1. StagingBuffer staging(physical_device, device_, data, size);  // TRANSFER_SRC/HOST_VISIBLE
    //   2. SingleTimeCommands cmd(device_, queue_family, queue);
    //   3. VkBufferCopy region{ .srcOffset=0, .dstOffset=0, .size=size };
    //      vkCmdCopyBuffer(cmd.handle(), staging.handle(), buffer_, 1, &region);
    //   4. cmd.submit_and_wait();   // staging はスコープ末尾で破棄
    void upload_with_staging(VkPhysicalDevice physical_device,
                             std::uint32_t queue_family, VkQueue queue,
                             const void* data, VkDeviceSize size);
```

> 注: ③ で `Buffer` から `VkPhysicalDevice` が消えるため、`upload_with_staging` の第1引数も③で `GpuAllocator&`（staging 用）へ変わる。②の時点では `physical_device` 引数のままでよい。

### ②-2. `VertexBuffer` / `IndexBuffer` のコンストラクタ変更（[mesh.hpp](../../engine/include/sq/graphics/mesh.hpp) / [mesh.cpp](../../engine/src/graphics/mesh.cpp)）

```cpp
// mesh.hpp: 両クラスのコンストラクタに queue_family と queue を追加する。
class VertexBuffer : public Buffer {
public:
    VertexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                 std::uint32_t queue_family, VkQueue queue,
                 const std::vector<Vertex>& vertices);
    // ...
};

class IndexBuffer : public Buffer {
public:
    IndexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                std::uint32_t queue_family, VkQueue queue,
                const std::vector<std::uint16_t>& indices);
    // ...
};
```

```cpp
// mesh.cpp: 基底 Buffer に渡す usage / properties を変更し、直書き memcpy をやめて staging 転送にする。
VertexBuffer::VertexBuffer(VkPhysicalDevice physical_device, VkDevice device,
                           std::uint32_t queue_family, VkQueue queue,
                           const std::vector<Vertex>& vertices)
    : Buffer(physical_device, device,
             sizeof(Vertex) * vertices.size(),
             // TRANSFER_DST を追加、properties を DEVICE_LOCAL に変更
             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      vertex_count_(static_cast<std::uint32_t>(vertices.size())) {
    // TODO: 旧 map()/memcpy()/unmap() を削除し、以下に置き換える:
    //   upload_with_staging(physical_device, queue_family, queue, vertices.data(), size());
    // （DEVICE_LOCAL は map() できないので直書きは不可）
}
// IndexBuffer も同様（VK_BUFFER_USAGE_INDEX_BUFFER_BIT | TRANSFER_DST, DEVICE_LOCAL）。
```

### ②-3. 呼び出し側の更新（[renderer.cpp](../../engine/src/graphics/renderer.cpp) `create_cube_mesh`）

```cpp
// renderer.cpp: VertexBuffer / IndexBuffer 生成に queue_family と queue を渡す。
triangle_mesh_ = std::make_unique<VertexBuffer>(
    physical_device_, device_->handle(),
    *queue_family_indices_.graphics_family, device_->graphics_queue(), vertices);
cube_indices_ = std::make_unique<IndexBuffer>(
    physical_device_, device_->handle(),
    *queue_family_indices_.graphics_family, device_->graphics_queue(), indices);
```

### ②-4. 検証
- ビルドして、キューブが従来通り描画されること（見た目は変わらない）。
- バリデーションレイヤーで新規エラーが出ないこと（`TRANSFER_DST` 未指定での copy 先エラー等が無い）。

---

## ③ 自前サブアロケータ

### ③-1. 新規ファイル `gpu_allocator.hpp` / `gpu_allocator.cpp`（`engine/include/sq/graphics/` と `engine/src/graphics/`）

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// GpuAllocator が貸し出す部分区間。値型（コピー可）。
// free() に必要な内部情報（ブロック番号・空きリストのノード等）は実装が解釈する。
struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;  // 所属ブロックの VkDeviceMemory
    VkDeviceSize offset = 0;                 // ブロック内オフセット（alignment 済み）
    VkDeviceSize size = 0;                   // 占有バイト数（alignment 切り上げ後）
    std::uint32_t memory_type_index = 0;     // どのメモリタイプのブロックか
    std::uint32_t block_index = 0;           // そのメモリタイプ内のブロック番号
    void* mapped = nullptr;                  // HOST_VISIBLE ならブロックの map 先 + offset。DEVICE_LOCAL は nullptr
};

// 1 つの大きな VkDeviceMemory を確保して部分区間を貸し出す単純なサブアロケータ（学習用・自作）。
// メモリタイプごとにブロック群を持ち、各ブロック内はフリーリストで管理する。
// 将来は vcpkg の vulkan-memory-allocator に差し替える前提（allocate/free/map の3点に責務を閉じる）。
class GpuAllocator {
public:
    GpuAllocator(VkPhysicalDevice physical_device, VkDevice device);
    ~GpuAllocator();  // 全ブロックを unmap（HOST_VISIBLE）してから vkFreeMemory

    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    // reqs（vkGet*MemoryRequirements の結果）と properties を満たす区間を確保する。
    // 手順:
    //   1. find_memory_type(reqs.memoryTypeBits, properties) でメモリタイプ index を決める
    //   2. そのメモリタイプの既存ブロックから、alignment を満たし size が収まる空きを探す
    //   3. 無ければ新しいブロックを1つ確保（既定サイズ or 要求サイズの大きい方）。
    //      HOST_VISIBLE なら生成直後に vkMapMemory で全体を map して保持する
    //   4. 空き区間を offset で切り出して Allocation を返す
    [[nodiscard]] Allocation allocate(const VkMemoryRequirements& reqs, VkMemoryPropertyFlags properties);

    // allocation を空きリストへ戻す（隣接空きはマージすると断片化を抑えられる=将来課題でよい）。
    void free(const Allocation& allocation);

private:
    // メモリタイプ index → そのメモリタイプのブロック配列。
    // 1 ブロック = { VkDeviceMemory, VkDeviceSize size, void* mapped, フリーリスト }。
    // TODO: Block 構造体と、空き区間 {offset, size} のリストを定義する。

    static constexpr VkDeviceSize kDefaultBlockSize = 64ull * 1024 * 1024;  // 64 MiB（要調整）

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    // std::vector<...> blocks_by_type_;  // TODO
};

}  // namespace sq::graphics
```

> `find_memory_type` は既に [buffer.cpp](../../engine/src/graphics/buffer.cpp:88) に static であるので、`GpuAllocator` からもそれを呼ぶ（重複実装しない）。あるいは `GpuAllocator` に移して `Buffer` 側は委譲する形でもよい（どちらか一方に一本化する）。

### ③-2. `Device` に `GpuAllocator` を持たせる（[device.hpp](../../engine/include/sq/graphics/device.hpp) / device.cpp）

```cpp
// device.hpp: メンバに GpuAllocator を追加し、アクセサを公開する。
// #include "sq/graphics/gpu_allocator.hpp"
class Device {
public:
    // ...既存...
    [[nodiscard]] GpuAllocator& allocator();  // Buffer 生成時に渡す

private:
    // ...既存...
    std::unique_ptr<GpuAllocator> allocator_;  // 論理デバイス生成後に作る。デバイス破棄より前に破棄される
};
```

> Device コンストラクタで `vkCreateDevice` 成功後に `allocator_ = std::make_unique<GpuAllocator>(physical_device, handle())` を作る。Device デストラクタでは `allocator_.reset()` を `vkDestroyDevice` より **前** に呼ぶ（メモリを解放してからデバイスを壊す）。

### ③-3. `Buffer` 基底をアロケータ経由に改修（[buffer.hpp](../../engine/include/sq/graphics/buffer.hpp) / [buffer.cpp](../../engine/src/graphics/buffer.cpp)）

```cpp
// buffer.hpp: コンストラクタから VkPhysicalDevice を外し、GpuAllocator& を受ける。
class Buffer {
public:
    Buffer(GpuAllocator& allocator, VkDevice device,
           VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    ~Buffer();  // vkDestroyBuffer → allocator_->free(allocation_) の順

protected:
    [[nodiscard]] void* map();   // HOST_VISIBLE: allocation_.mapped を返すだけ（vkMapMemory しない）
    void unmap();                // persistent map なので何もしない（no-op）に変わる

    GpuAllocator* allocator_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    Allocation allocation_{};     // 旧 VkDeviceMemory memory_ を置き換え
    VkDeviceSize size_ = 0;
};
```

```cpp
// buffer.cpp: コンストラクタの手順を差し替える。
//   1. vkCreateBuffer（従来通り）
//   2. vkGetBufferMemoryRequirements(device_, buffer_, &reqs)
//   3. allocation_ = allocator.allocate(reqs, properties);   // ← vkAllocateMemory の置換
//   4. vkBindBufferMemory(device_, buffer_, allocation_.memory, allocation_.offset);  // offset に注意
// デストラクタ:
//   vkDestroyBuffer(device_, buffer_, nullptr);
//   allocator_->free(allocation_);   // ← vkFreeMemory の置換
```

**波及**: `Buffer` を継承/使用する全箇所のシグネチャが変わる。
- `StagingBuffer(GpuAllocator&, VkDevice, const void* data, VkDeviceSize size)`
- `UniformBuffer(GpuAllocator&, VkDevice, VkDeviceSize size)`
- `VertexBuffer` / `IndexBuffer`: ②で追加した `physical_device` を `GpuAllocator&` に置換（`queue_family`/`queue` は残す。staging 転送に使うため）。`upload_with_staging` の第1引数も `GpuAllocator&` に。
- 呼び出し側:
  - [renderer.cpp](../../engine/src/graphics/renderer.cpp) `create_uniform_buffers` / `create_cube_mesh` → `device_->allocator()` を渡す。
  - [texture.cpp](../../engine/src/graphics/texture.cpp:41) の `StagingBuffer` 生成 → `device_->allocator()` を渡す（テクスチャ本体のイメージメモリは今回は対象外。バッファのみアロケータ経由）。

### ③-4. 検証
- 起動時に Phase 7 で出ていた `small-dedicated-allocation` 系の性能警告が消える（または減る）こと。
- 描画結果が変わらないこと。`vkDeviceWaitIdle` 後の終了でバリデーションのメモリリーク報告が無いこと（全 `Allocation` が free され、全ブロックが `vkFreeMemory` される）。
- `UniformBuffer` の毎フレーム `update()` が persistent map 経由で正しく効くこと（カメラが動く）。

---

## ① 半透明の描画順ソート + `Material` コンポーネント

### ①-1. `Material` コンポーネント新設（`engine/include/sq/scene/material.hpp`）

```cpp
#pragma once

#include <glm/glm.hpp>

namespace sq::scene {

// 描画マテリアル（ECSコンポーネント）。今は半透明フラグのみ。
// 将来: ベースカラー・テクスチャID・両面描画フラグ等の置き場にする。
// Material を持たないエンティティは「不透明」として扱う（後方互換）。
struct Material {
    bool transparent = false;   // true なら半透明パスで back-to-front 描画
    // glm::vec4 base_color{1.0f}; // 将来: 色 tint（push constant か UBO で渡す）
};

}  // namespace sq::scene
```

### ①-2. `GraphicsPipeline` に設定構造体を追加（[graphics_pipeline.hpp](../../engine/include/sq/graphics/graphics_pipeline.hpp) / graphics_pipeline.cpp）

```cpp
// graphics_pipeline.hpp: depth write / blend を切り替えられるようにする。
struct PipelineConfig {
    bool depth_write_enable = true;   // 不透明=true / 半透明=false
    bool blend_enable = false;        // 不透明=false / 半透明=true（アルファブレンド）
};

class GraphicsPipeline {
public:
    GraphicsPipeline(VkDevice device, VkRenderPass render_pass, VkExtent2D viewport_extent,
                     const std::string& vert_spv_path, const std::string& frag_spv_path,
                     VkDescriptorSetLayout descriptor_set_layout,
                     const PipelineConfig& config);   // ← 追加
    // ...
};
```

```cpp
// graphics_pipeline.cpp:
//   - VkPipelineDepthStencilStateCreateInfo::depthWriteEnable = config.depth_write_enable
//   - VkPipelineColorBlendAttachmentState::blendEnable = config.blend_enable
//     （blend 有効時: srcColor=SRC_ALPHA, dstColor=ONE_MINUS_SRC_ALPHA,
//       srcAlpha=ONE, dstAlpha=ZERO, colorBlendOp=ADD, alphaBlendOp=ADD）
//   それ以外（頂点入力・ラスタライザ・レイアウト等）は現状のまま共有する。
```

### ①-3. `Renderer` にパイプラインを 2 本持たせる（[renderer.hpp](../../engine/include/sq/graphics/renderer.hpp) / [renderer.cpp](../../engine/src/graphics/renderer.cpp)）

```cpp
// renderer.hpp: pipeline_ を 2 本に分ける。
std::unique_ptr<GraphicsPipeline> pipeline_opaque_;       // depthWrite=TRUE,  blend=OFF
std::unique_ptr<GraphicsPipeline> pipeline_transparent_;  // depthWrite=FALSE, blend=ON
```

```cpp
// renderer.cpp コンストラクタ 手順12: 2 本生成する（同じ SPV・同じレイアウト、config だけ変える）。
pipeline_opaque_ = std::make_unique<GraphicsPipeline>(
    device_->handle(), render_pass_->handle(), swapchain_->extent(),
    "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
    descriptor_set_layout_, PipelineConfig{ .depth_write_enable = true,  .blend_enable = false });
pipeline_transparent_ = std::make_unique<GraphicsPipeline>(
    device_->handle(), render_pass_->handle(), swapchain_->extent(),
    "shaders/triangle.vert.spv", "shaders/triangle.frag.spv",
    descriptor_set_layout_, PipelineConfig{ .depth_write_enable = false, .blend_enable = true });
// デストラクタでも両方 reset する。
```

### ①-4. `draw_frame` の描画ループを 2 パスに再構成（[renderer.cpp:203-212](../../engine/src/graphics/renderer.cpp:203)）

現在は `view<Transform, Position>().each(...)` で 1 パス描画している。これを **収集 → 不透明パス → 半透明ソート → 半透明パス** に置き換える。

```cpp
// 事前に ActiveCamera の位置を取得しておく（distance ソート用）。
// draw_frame 冒頭で解決済みの camera_entity から:
//   glm::vec3 cam_pos = registry.get<scene::Camera>(camera_entity).position;  // null 時は default 位置

// --- 1. 収集 ---
// 半透明の描画情報。model 行列と距離を持つ。
struct DrawItem {
    glm::mat4 model;
    float distance;  // cam_pos からの距離（の2乗で可。sqrt 不要）
};
std::vector<DrawItem> transparent_items;   // 半透明のみ貯める

// --- 2. 不透明パス ---
vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_opaque_->handle());
// descriptor set / mesh のバインドは従来通り（layout は両パイプラインで共通なので使い回せる）
registry.view<scene::Transform, scene::Position>().each(
    [&](ecs::Entity e, scene::Transform& t, scene::Position& pos) {
        t.model = glm::translate(glm::mat4(1.0f), glm::vec3(pos.x, pos.y, pos.z));

        // Material を持ち transparent なら、この場では描かず transparent_items に回す:
        //   const bool is_transparent = registry.has<scene::Material>(e)
        //                               && registry.get<scene::Material>(e).transparent;
        //   if (is_transparent) { transparent_items.push_back({ t.model, 距離2乗 }); return; }

        // 不透明はその場で描画:
        vkCmdPushConstants(command_buffer, pipeline_opaque_->layout(),
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &t.model);
        vkCmdDrawIndexed(command_buffer, cube_indices_->index_count(), 1, 0, 0, 0);
    });

// --- 3. 半透明ソート（遠い順 = distance 降順）---
// std::sort(transparent_items.begin(), transparent_items.end(),
//           [](const DrawItem& a, const DrawItem& b){ return a.distance > b.distance; });

// --- 4. 半透明パス ---
// vkCmdBindPipeline(..., pipeline_transparent_->handle());
// for (const DrawItem& item : transparent_items) {
//     vkCmdPushConstants(..., pipeline_transparent_->layout(), ..., &item.model);
//     vkCmdDrawIndexed(command_buffer, cube_indices_->index_count(), 1, 0, 0, 0);
// }
```

> 注意点:
> - `vkCmdBindDescriptorSets` は layout がパイプライン間で共通なら 1 回で足りる（`pipeline_opaque_->layout()` を使う。2 本は同じ `descriptor_set_layout_` から作るのでレイアウト互換）。念のため各パスでバインドし直しても正しい。
> - push constant のステージ/オフセットも両パイプラインで一致させること。
> - 距離は `glm::distance2(cam_pos, world_pos)`（2乗距離）で十分。`world_pos` は `pos`（現状 model は平行移動のみ）。将来 model が回転/スケールを含むなら `model[3]` の並進成分を使う。

### ①-5. フラグメントシェーダのアルファ
半透明が実際に透けるには、フラグメントシェーダが `< 1.0` のアルファを出力する必要がある。現状のテクスチャ（RGBA）のアルファをそのまま出しているか確認する。もし常に不透明（a=1）なら、テスト用に `Material::base_color.a` を push constant/UBO で渡してアルファを乗算する拡張が要る（**このフェーズでは最小構成: テクスチャのアルファをそのまま使い、アルファ付き PNG で確認**でよい）。

### ①-6. main で確認用エンティティを用意（[main.cpp](../../sandbox_graphics/main.cpp)）
- 一部のエンティティに `registry.add<scene::Material>(e, { .transparent = true })` を付ける。
- 半透明キューブを不透明キューブの前後に配置し、視点を回して **背後が正しく透ける**こと・**視点角度を変えても破綻しない**ことを確認する。

### ①-7. 検証
- 半透明エンティティ越しに背後の不透明・半透明が正しく見えること。
- カメラを周回させても（Orbit/FreeFly）描画順が破綻しないこと（遠い半透明が先に描かれる）。
- `Material` を持たない従来エンティティが不透明として正しく描かれること（後方互換）。

---

## 実装手順（この順で、各ステップ完了ごとに動作確認）

1. **② DEVICE_LOCAL化**
   - `Buffer::upload_with_staging()` を追加（②-1）
   - `VertexBuffer`/`IndexBuffer` を DEVICE_LOCAL + staging に変更、コンストラクタに `queue_family`/`queue` 追加（②-2）
   - `renderer.cpp` の生成箇所を更新（②-3）→ **見た目が変わらないことを確認**（②-4）
2. **③ 自前サブアロケータ**
   - `gpu_allocator.hpp/cpp` を新設（③-1）
   - `Device` に `GpuAllocator` を持たせ `allocator()` を公開、破棄順序を担保（③-2）
   - `Buffer` 基底を `GpuAllocator` 経由に改修、全派生・全呼び出し側を更新（③-3）→ **警告が消え・描画が変わらないことを確認**（③-4）
3. **① 半透明ソート**
   - `Material` コンポーネント新設（①-1）
   - `GraphicsPipeline` に `PipelineConfig` 追加（①-2）、`Renderer` を 2 パイプライン化（①-3）
   - `draw_frame` を 2 パス + ソートに再構成（①-4）
   - フラグメントシェーダのアルファ確認（①-5）、main に確認用半透明エンティティ（①-6）→ **透けを確認**（①-7）

---

## 検証観点（フェーズ全体）
- 手順1完了時: 頂点/インデックスが DEVICE_LOCAL になり、描画結果が従来と同一。バリデーションエラー無し。
- 手順2完了時: `small-dedicated-allocation` 警告が解消（個別 `vkAllocateMemory` が大ブロックのサブアロケートに置き換わる）。終了時のメモリリーク報告無し。
- 手順3完了時: 不透明→半透明の 2 パスで、半透明が back-to-front で正しく透ける。`Material` 無しは不透明。
- 全体: リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab 復帰で描画とカメラ操作が継続すること（Phase 10 の不変条件を壊していない）。

---

## Phase 12 以降の将来課題
- **アロケータの `GpuAllocator` → VMA 差し替え**（vcpkg `vulkan-memory-allocator`）。今回 `Buffer` の確保点を1箇所に閉じたので差し替えが局所化される。
- **フリーリストの隣接空きマージ / defrag**（③では最小実装。断片化対策は将来）。
- **テクスチャイメージメモリもアロケータ経由に**（今回はバッファのみ。イメージは `vkGetImageMemoryRequirements` → `allocate` で同様に載せられる）。
- **`Material` の拡張**: ベースカラー tint、複数テクスチャ、両面描画、アルファカットオフ（`discard`）。
- **半透明の OIT（順序独立透過）** など、CPU ソートに依らない手法。
- **ミップマップ生成（`vkCmdBlitImage`）**（Phase 10 の将来課題から継続）。
- **Mesh のコンポーネント化**（エンティティごとに別メッシュ/インデックス）。現状は全エンティティが共有 1 メッシュ。
