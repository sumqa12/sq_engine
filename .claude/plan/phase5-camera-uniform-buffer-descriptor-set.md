# C++ ゲームエンジン（学習目的）— Phase 5: カメラ（Uniform Buffer + Descriptor Set / 3D透視 / ECS Cameraコンポーネント）

## Context
Phase 4までで、ECSの各エンティティの`model`行列（`mat4`）をプッシュ定数で渡して複数の三角形を描画できるようになった（[phase4-resize-ecs-rendering.md](phase4-resize-ecs-rendering.md)参照）。Phase 5ではカメラを導入し、**全エンティティで共有するview-projection行列**をUniform Buffer + Descriptor Setでシェーダーへ渡す。これはVulkanの重要概念（ディスクリプタ）の学習であり、カメラ移動・ズームの土台になる。

方針の決定事項:
- **射影方式: 3D透視（glm::perspective）**。頂点は当面2D（`vec2`, z=0平面）のままでよい（透視カメラをz方向に引いて平面上の三角形を見る）。**深度バッファは今回は導入しない**（重なり合う立体を描くようになった段階で別途対応）。
- **Cameraの持ち方: ECSのCameraコンポーネント**（`sq::scene::Camera`）。`registry.view<Camera>()`で取得し、最初に見つかったものをアクティブカメラとして使う（複数カメラの選択ロジックは将来課題）。

学習目的（CLAUDE.md）のため、本セッションで作成するのは**宣言＋TODOコメントの骨格まで**。実装本体はユーザーが書く。

## 実装対象

### 1. 新規 `UniformBuffer` クラス（`mesh.cpp`の`VertexBuffer`と同じRAIIパターン）
- ファイル: `engine/include/sq/graphics/uniform_buffer.hpp` / `src/graphics/uniform_buffer.cpp`
- `VkBuffer` + `VkDeviceMemory`を保持。`usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`、メモリは`HOST_VISIBLE | HOST_COHERENT`。
- 毎フレームCPUから書き換えるため、**永続マップ**（コンストラクタで一度`vkMapMemory`して`void* mapped_`を保持、デストラクタで`vkUnmapMemory`）。
- メソッド: `void update(const void* data, std::size_t size);`（`std::memcpy`）、`VkBuffer handle() const;`、`VkDeviceSize size() const;`
- `find_memory_type`は`VertexBuffer`と同一ロジック（共通化は将来課題）。

### 2. 新規 Cameraコンポーネント `sq::scene::Camera`
- ファイル: `engine/include/sq/scene/camera.hpp` / `src/scene/camera.cpp`（`transform.hpp`と同じ`sq::scene`名前空間）
- 透視パラメータ: `position`, `target`, `up`, `fov_y_radians`, `near_plane`, `far_plane`。
- `glm::mat4 view_projection(float aspect) const;`（実装: `glm::perspective(...) * glm::lookAt(...)`）。
- **Vulkan固有の注意（コメントで明記）**:
  - `GLM_FORCE_DEPTH_ZERO_TO_ONE`（VulkanのNDC深度は[0,1]、GLM既定はOpenGLの[-1,1]）。CMakeで全体定義。
  - `glm::perspective`の結果はYが上下反転する（VulkanはY下向き）。`proj[1][1] *= -1;` で補正。
- `struct CameraUBO { glm::mat4 view_projection; };` も同ヘッダに置く（シェーダーの`layout(set=0, binding=0)`と一致させる）。

### 3. `GraphicsPipeline` にディスクリプタセットレイアウトを渡す
- コンストラクタに `VkDescriptorSetLayout descriptor_set_layout` 引数を追加。
- `setLayoutCount = 1; pSetLayouts = &descriptor_set_layout;` に変更（現状は0）。プッシュ定数レンジ（model行列）は維持。
- レイアウト自体の所有・破棄は呼び出し側（Renderer）が行う。

### 4. `Renderer` にディスクリプタ一式を追加
新規メンバ（**kFramesInFlight = 2 個ずつ**、スワップチェーン画像枚数には非依存）:
- `VkDescriptorSetLayout descriptor_set_layout_`（set=0, binding=0, UNIFORM_BUFFER, VERTEX）
- `VkDescriptorPool descriptor_pool_`
- `std::vector<std::unique_ptr<UniformBuffer>> camera_ubos_`
- `std::vector<VkDescriptorSet> descriptor_sets_`

私有メソッド: `create_descriptor_set_layout()`, `create_uniform_buffers()`, `create_descriptor_pool()`, `create_descriptor_sets()`

コンストラクタ順序（**パイプライン作成の前**にレイアウトが必要）:
1. `create_descriptor_set_layout()`
2. `create_uniform_buffers()`（UBOをフレーム数分）
3. `create_descriptor_pool()`
4. `create_descriptor_sets()`（`vkAllocateDescriptorSets` → `VkDescriptorBufferInfo`で各UBOを`vkUpdateDescriptorSets`）
5. `GraphicsPipeline`生成に`descriptor_set_layout_`を渡す

`draw_frame(registry)`内（per-entityループの**前**）:
- `registry.view<sq::scene::Camera>()` でアクティブカメラ取得（最初の1つ、無ければ単位行列フォールバック）。
- `aspect = extent.width / (float)extent.height`（`swapchain_->extent()`）。
- `CameraUBO ubo{ camera.view_projection(aspect) };` を `camera_ubos_[current_frame_]->update(&ubo, sizeof(ubo));`
- `vkCmdBindDescriptorSets(cb, ..., pipeline_->layout(), 0, 1, &descriptor_sets_[current_frame_], 0, nullptr);`
- 以降は既存のper-entity `vkCmdPushConstants(model)` + `vkCmdDraw`。

**リサイズ対応**: aspectを毎フレーム`swapchain_->extent()`から再計算するので、`recreate_swapchain()`でディスクリプタ/UBOを作り直す必要はない。

**破棄順序（デストラクタ）**: `vkDeviceWaitIdle`後、`vkDestroyDescriptorPool`（セットも解放）→ `camera_ubos_.clear()` → `vkDestroyDescriptorSetLayout`。パイプラインより後、デバイス破棄より前。

### 5. シェーダー `shaders/triangle.vert`
```glsl
layout(set = 0, binding = 0) uniform CameraUBO { mat4 view_projection; } ubo;
layout(push_constant) uniform PushConstants { mat4 model; } pc;
...
gl_Position = ubo.view_projection * pc.model * vec4(in_position, 0.0, 1.0);
```

### 6. `sandbox_graphics/main.cpp`
- エンティティを1つ作り `sq::scene::Camera` を`add`（例: position=(0,0,3), target=(0,0,0), up=(0,1,0), fov=45°, near=0.1, far=10）。
- 既存のTransform付き三角形群はそのまま。

### 7. CMake
- `engine/CMakeLists.txt` に `src/graphics/uniform_buffer.cpp` と `src/scene/camera.cpp` を追加。
- `target_compile_definitions(sq_engine_lib PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)` を追加。

## 本セッションでの実施結果（骨格作成・完了）
CLAUDE.mdのルール（コード生成は宣言まで）に従い以下を作成。**実装本体はユーザーが記述するため、この時点ではビルドは通らない**（`renderer.cpp`/`graphics_pipeline.cpp`が旧シグネチャのままの中間状態）。

- **新規**: [uniform_buffer.hpp](../../engine/include/sq/graphics/uniform_buffer.hpp) / [uniform_buffer.cpp](../../engine/src/graphics/uniform_buffer.cpp)（TODOスケルトン）、[camera.hpp](../../engine/include/sq/scene/camera.hpp)（`Camera`+`CameraUBO`）/ [camera.cpp](../../engine/src/scene/camera.cpp)（`view_projection`のTODO）
- **宣言変更**:
  - [renderer.hpp](../../engine/include/sq/graphics/renderer.hpp): ディスクリプタ関連メンバ4つ + 私有メソッド4つを追加、`uniform_buffer.hpp`をinclude
  - [graphics_pipeline.hpp](../../engine/include/sq/graphics/graphics_pipeline.hpp): コンストラクタに`VkDescriptorSetLayout`引数を追加
  - [engine/CMakeLists.txt](../../engine/CMakeLists.txt): `uniform_buffer.cpp`/`camera.cpp`を追加、`GLM_FORCE_DEPTH_ZERO_TO_ONE`を全体定義

## ユーザーが実装する本体（TODO・推奨順）
1. `uniform_buffer.cpp`（`vkCreateBuffer`〜永続`vkMapMemory`、`update`のmemcpy、`find_memory_type`。`<cstring>`要）
2. `camera.cpp`（`glm::perspective`のY反転補正 × `glm::lookAt`）
3. `graphics_pipeline.cpp`（コンストラクタ引数追加、`setLayoutCount=1`/`pSetLayouts`）
4. `renderer.cpp`（コンストラクタでディスクリプタ生成→パイプラインへレイアウト受け渡し、`draw_frame`でUBO更新+`vkCmdBindDescriptorSets`、デストラクタで破棄）
5. `shaders/triangle.vert`（`set=0, binding=0`のUBO追加）
6. `sandbox_graphics/main.cpp`（`Camera`コンポーネント付きエンティティ追加）

## 検証方法
- `cmake --build build --config Debug` でビルドが通ること。
- `sandbox_graphics` 実行で、カメラのview-projection越しに三角形群が描画されること（modelだけの時と見え方が変わる）。
- カメラの`position`を変えると見え方（位置・大きさ）が変わること。
- ウィンドウをリサイズしてもアスペクト比が破綻せず、検証レイヤーのエラーが出ないこと。
- `ctest` で既存ECSテストが引き続き全パスすること。

## 補足・将来課題
- 深度バッファ（`VkImage`深度アタッチメント）: 立体が重なる描画を始めたら導入。
- 頂点の`vec3`化・3Dメッシュ: 本格3Dに進む際。
- `find_memory_type`の`VertexBuffer`/`UniformBuffer`間の共通化。
- 複数カメラのアクティブ選択ロジック。
