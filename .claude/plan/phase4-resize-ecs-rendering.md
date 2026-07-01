# C++ ゲームエンジン（学習目的）— Phase 4: ウィンドウリサイズ処理 + ECS-描画連携

## Context
Phase 3まででVulkanによる三角形描画（ハードコード頂点・`gl_VertexIndex`方式）が動作するようになった（[vulkan-pipeline-skeleton.md](vulkan-pipeline-skeleton.md)参照）。Phase 4では2つの課題に取り組む。

1. **ウィンドウリサイズ処理**: 現状リサイズに非対応（`Window`が`width_`/`height_`を初期化しておらず、GLFWのリサイズコールバックも未登録、`draw_frame`が`VK_ERROR_OUT_OF_DATE_KHR`等を無視、`recreate_swapchain()`がどこからも呼ばれていない）。
2. **ECSと描画の連携**: Phase 1の自作ECS（`sq::ecs::Registry`）とPhase 2-3の描画を接続し、エンティティ単位で三角形を描けるようにする。

学習目的のため、**CLAUDE.mdのルールに従いコード生成は関数の宣言までに留める**。本セッションでは骨格（宣言＋TODOコメント）のみを追加し、実装本体はユーザー自身が後続で書く。

## 技術選定（Phase 4で決めた方針）
- **頂点データ転送**: 頂点バッファ方式（`VkBuffer`を実際に作成し頂点データをGPUへ転送）。学習段階ではステージングバッファ無しの`HOST_VISIBLE | HOST_COHERENT`版とし、最適化は将来課題。
- **Transformコンポーネント**: 最初から`glm::mat4`モデル行列で設計（将来の3D化を見据える）。エンティティごとの変換はプッシュ定数（`vkCmdPushConstants`）でシェーダーへ渡す。
- 依存関係の向き: `sq::graphics`が`sq::ecs`/`sq::scene`に依存する一方向。ECS本体は変更しない（Rendererにregistryを注入する形）。

---

## A. ウィンドウリサイズ処理

### A-1. `window.hpp` / `window.cpp`
- コンストラクタで`width_`/`height_`を正しく初期化（現状`(void)`で握りつぶしている）。
- `glfwSetWindowUserPointer(window_, this)` + `glfwSetFramebufferSizeCallback(window_, &Window::framebuffer_resize_callback)`を登録。
- 静的コールバック`framebuffer_resize_callback(GLFWwindow*, int, int)`で`width_`/`height_`を更新し`resized_`フラグを立てる。
- 追加メソッド:
  - `bool consume_resized_flag()`（取得と同時にfalseへリセット）
  - `void wait_while_minimized() const`（最小化中=幅か高さが0の間、`glfwGetFramebufferSize`+`glfwWaitEvents`でブロック）

### A-2. `renderer.cpp` の `draw_frame()`
- `vkAcquireNextImageKHR`の戻り値が`VK_ERROR_OUT_OF_DATE_KHR`なら`recreate_swapchain()`を呼んで即`return`。
- `vkQueuePresentKHR`の戻り値が`VK_ERROR_OUT_OF_DATE_KHR` **または** `VK_SUBOPTIMAL_KHR`、**または**`window_->consume_resized_flag()`が`true`なら`recreate_swapchain()`。
  - 補足: `VkResult`は単一値なので条件は`||`（OR）。`OUT_OF_DATE`は「画像が使えない→即再構築」、`SUBOPTIMAL`は「成功扱いだが次に備えて再構築予約」という意味の違いがあるため、acquire側は即return・present側は再構築のみ、という扱いの差になる。

### A-3. `recreate_swapchain()` の拡張
- 冒頭で`window_->wait_while_minimized()`（最小化中は再構築をブロックして待つ）。
- `vkDeviceWaitIdle` → `destroy_framebuffers()` → `swapchain_->recreate(...)` → `create_framebuffers()`。
- 画像枚数がリサイズ前後で変わりうるため、`create_framebuffers()`の後に`command_buffers_`と`sync_objects_`を新しい`framebuffers_.size()`で作り直す。
  - `framebuffers_.size() == swapchain_->image_views().size() == images_.size()` がスワップチェーン画像枚数。
  - `unique_ptr`への代入は暗黙に古いオブジェクトを破棄（reset相当）するが、破棄前に`vkDeviceWaitIdle`でGPU使用中でないことを保証する必要がある（冒頭のwaitで担保）。

### A-4. `swapchain.cpp` の細かい修正（Phase 3で残った課題）
- `destroy()`に`images_.clear()`を追加（現状`image_views_`はクリアされるが`images_`が漏れている）。
- `oldSwapchain`の扱い・`image_format_`フォールバックの整理も必要に応じて。

### A-5. `sync_objects` の設計変更（リサイズ対応に必須）
- `image_available`セマフォ・`in_flight_fence`は`kFramesInFlight`個のまま`current_frame_`でインデックス。
- `render_finished`セマフォだけはスワップチェーン画像枚数分用意し`image_index`でインデックス（コンストラクタに`swapchain_image_count`引数を追加済み）。
  - 理由: `render_finished`はプレゼンテーション（画像単位の操作）が待つセマフォ。フレームインフライト数（2）と画像枚数（3）がずれると「まだ使用中のセマフォを再シグナル」するバリデーションエラーになる。

---

## B. ECS-描画連携（頂点バッファ方式 + mat4 Transform）

### B-1. 新規 `engine/include/sq/graphics/mesh.hpp` / `src/graphics/mesh.cpp`
- `struct Vertex { glm::vec2 position; glm::vec3 color; };`
- `class VertexBuffer`（RAII）: `VkBuffer` + `VkDeviceMemory`を保持。
  - コンストラクタ: `vkCreateBuffer`→`vkGetBufferMemoryRequirements`→`find_memory_type(...)`でメモリタイプ選択→`vkAllocateMemory`→`vkBindBufferMemory`→`vkMapMemory`で頂点データ書き込み（HOST_COHERENTなのでflush不要）。
  - `void bind(VkCommandBuffer) const;`（`vkCmdBindVertexBuffers`）
  - `std::uint32_t vertex_count() const;`
  - `static std::uint32_t find_memory_type(VkPhysicalDevice, type_filter, properties);`（`vkGetPhysicalDeviceMemoryProperties`で条件に合うインデックスを探す）

### B-2. 新規 `engine/include/sq/scene/transform.hpp`
- `namespace sq::scene { struct Transform { glm::mat4 model{1.0f}; }; }`
- ECS自体は変更不要（`Registry::add<Transform>(entity, ...)`がそのまま使える）。

### B-3. `graphics_pipeline.cpp`
- 頂点入力ステートを`Vertex`のレイアウトに合わせて設定（binding stride = `sizeof(Vertex)`、attribute0 = position(vec2)、attribute1 = color(vec3)）。
- パイプラインレイアウトに`VkPushConstantRange`（`VK_SHADER_STAGE_VERTEX_BIT`, offset 0, `sizeof(glm::mat4)`）を追加。

### B-4. シェーダー `shaders/triangle.vert`
- ハードコードの`positions[]`/`colors[]`配列を削除し、`layout(location=0) in vec2 in_position; layout(location=1) in vec3 in_color;`の実頂点属性に変更。
- `layout(push_constant) uniform PushConstants { mat4 model; } pc;`を追加し、`gl_Position = pc.model * vec4(in_position, 0.0, 1.0);`。

### B-5. `renderer.hpp` / `renderer.cpp`
- `Renderer`が`VertexBuffer triangle_mesh_`（デモ用の共有メッシュ）を所有。`create_triangle_mesh()`をDevice作成後に呼ぶ。
- `run(sq::ecs::Registry&)` / `draw_frame(const sq::ecs::Registry&)`にシグネチャ変更（ECSを注入）。
- コマンド記録ラムダ内: パイプラインバインド→`triangle_mesh_->bind(cb)`→`registry.view<sq::scene::Transform>().each([&](Entity, Transform& t){ vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &t.model); vkCmdDraw(cb, triangle_mesh_->vertex_count(), 1, 0, 0); })`。

### B-6. `sandbox_graphics/main.cpp`
- `sq::ecs::Registry registry;`を作成し、3〜5体のエンティティを生成。`Transform`に異なる`glm::translate`行列を設定して三角形を横に並べる。
- `Renderer renderer(...); renderer.run(registry);`

### B-7. `engine/CMakeLists.txt`
- `src/graphics/mesh.cpp`を`sq_engine_lib`のソース一覧に追加。

---

## 本セッションでの実施結果（骨格作成・完了）
CLAUDE.mdのルール（コード生成は宣言まで）に従い、以下を作成。ビルドが通ることを確認済み（三角形は暫定でハードコードのまま表示）。

- **新規**: [mesh.hpp](../../engine/include/sq/graphics/mesh.hpp)（`Vertex`/`VertexBuffer`の宣言）、[mesh.cpp](../../engine/src/graphics/mesh.cpp)（各関数はTODOコメントのみ）、[transform.hpp](../../engine/include/sq/scene/transform.hpp)
- **変更（最小限）**:
  - [renderer.hpp](../../engine/include/sq/graphics/renderer.hpp): `run`/`draw_frame`を`Registry&`受け取りに変更、`triangle_mesh_`メンバ・`create_triangle_mesh()`宣言を追加
  - [renderer.cpp](../../engine/src/graphics/renderer.cpp): シグネチャ変更に追従（既存の動作コードは保持）、ECS連携部分・`create_triangle_mesh()`はTODOコメントで実装手順を明記
  - [graphics_pipeline.cpp](../../engine/src/graphics/graphics_pipeline.cpp): 頂点入力ステート・プッシュ定数レンジの追加箇所にTODOコメント
  - [triangle.vert](../../shaders/triangle.vert): 実頂点属性・プッシュ定数化の手順をTODOコメント
  - [sandbox_graphics/main.cpp](../../sandbox_graphics/main.cpp): `Registry`生成して`run(registry)`へ、エンティティ生成はTODO
  - [engine/CMakeLists.txt](../../engine/CMakeLists.txt): `mesh.cpp`をビルド対象に追加
  - ※A-1〜A-5（リサイズ処理・sync_objects設計変更）は本セッション以前に一部反映済み（`draw_frame`のOUT_OF_DATE/SUBOPTIMAL分岐、`recreate_swapchain`での`sync_objects_`再生成、`wait_while_minimized`呼び出し等）

## 検証方法
- `cmake --preset=default && cmake --build build --config Debug`でビルドが通ることを確認（済）。
- （実装後）`sandbox_graphics`を実行し、複数の三角形が`Transform`の位置に応じて異なる場所に描画されることを目視確認。
- （実装後）ウィンドウをドラッグしてリサイズし、検証レイヤーのエラーなく描画が継続する（スワップチェーン再構築）ことを確認。
- （実装後）最小化→復元でクラッシュしないこと（`wait_while_minimized()`）を確認。
- `ctest`で既存ECSテストが引き続き全パスすること（ECS本体は非変更）を確認。

## 次に着手する順序（推奨）
1. `mesh.cpp`の`VertexBuffer`実装（`vkCreateBuffer`〜`vkMapMemory`、`find_memory_type`）
2. `graphics_pipeline.cpp`の頂点入力ステート＋プッシュ定数レンジ
3. `triangle.vert`を実頂点属性＋プッシュ定数に書き換え
4. `renderer.cpp`の`create_triangle_mesh()`と`draw_frame`のview反復描画
5. `sandbox_graphics/main.cpp`でエンティティ生成
6. リサイズ・最小化の動作確認
