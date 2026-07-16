# C++ ゲームエンジン（学習目的）— Phase 6: リファクタリング（SyncObjectsバグ修正 / Buffer共通化 / カメラフォールバック / クリーンアップ）

## Context
Phase 5（カメラ + UBO + Descriptor Set、[phase5-camera-uniform-buffer-descriptor-set.md](phase5-camera-uniform-buffer-descriptor-set.md)参照）完了後、記録されていた技術的負債を調査した。

調査結果:
- swapchainの残課題（`oldSwapchain`・`images_`/`image_views_`のclear漏れ・画像枚数`std::max(minImageCount, 2u)`）は**ユーザー実装で解消済み**だった。
- **未解消のUBバグを発見**: `renderer.cpp`は`render_finished(image_index)`を呼ぶが、`SyncObjects`は`render_finished`セマフォを`frames_in_flight`(2)個しか作っていない。スワップチェーン画像3枚の環境では`image_index == 2`で**vectorの範囲外アクセス（未定義動作）**。Phase 3のバグ7で記録された設計変更が未適用だった。
- `find_memory_type`はmesh.cpp / uniform_buffer.cppで完全同一。バッファ生成フロー（vkCreateBuffer→requirements→find_memory_type→vkAllocateMemory→vkBindBufferMemory）も約70%重複。→ **ユーザー決定: 生成ロジック全体をRAII基底クラス`Buffer`（継承）で共通化**。
- カメラ不在時、`View::front()`がデフォルト構築`Entity{}`を返し、続く`registry.get<Camera>()`が未定義動作。`Entity::is_null()`が既にあるためECS本体は無変更で対応可能。

## 実装対象

### 1. SyncObjects の render_finished セマフォ数（バグ修正・最優先）
- `image_available` / `in_flight_fence`: `frames_in_flight`個、`current_frame_`でインデックス（CPU側フレームペーシング用）。
- `render_finished`: **`swapchain_image_count`個、`image_index`でインデックス**（presentが待つ「画像単位」のセマフォのため）。
- コンストラクタを2ループに分割: ループ1=`frames_in_flight`回でimage_available+fence、ループ2=`swapchain_image_count`回でrender_finished。
- Renderer側は既に`render_finished(image_index)`で呼んでおり、`recreate_swapchain()`も`vkDeviceWaitIdle`後にSyncObjectsを再作成しているため、**Renderer側の変更は不要**。

### 2. バッファ生成の共通化 — RAII基底クラス `Buffer`（継承）
- 新規 [buffer.hpp](../../engine/include/sq/graphics/buffer.hpp) / [buffer.cpp](../../engine/src/graphics/buffer.cpp)。
- `Buffer(physical_device, device, size, usage, properties)`が生成フロー5段を担う。`handle()`/`size()`公開、`map()`/`unmap()`/`find_memory_type`はprotected。
- デストラクタは**非virtual**（派生型を直接所有する前提。`Buffer*`経由のdelete禁止をコメント明記）。C++の破棄順序（派生→基底）により、UniformBufferのunmap→基底のvkDestroyBuffer/vkFreeMemoryの順が自動保証される（学習ポイント）。
- `VertexBuffer : public Buffer`: `vertex_count_`と`bind()`/`vertex_count()`のみ残す。コンストラクタ本体はmap→memcpy→unmap。
- `UniformBuffer : public Buffer`: `mapped_`と`update()`のみ残す。コンストラクタで`mapped_ = map()`（永続マップ）、デストラクタでunmapのみ。
- usageフラグ/メモリプロパティは派生コンストラクタの基底初期化子リストに記載済み。

### 3. アクティブカメラ不在時のフォールバック（ECS無変更）
- `renderer.cpp`の`draw_frame`内: `camera_entity.is_null()`をチェックし、無効なら`view_projection`を単位行列`glm::mat4{1.0f}`にフォールバック（phase5プランの方針）。

### 4. 小規模クリーンアップ
- swapchain.cpp: formats空配列時のガード（TODOコメント追加、throwはユーザー実装）。
- renderer.cpp: `(void)width;`等3行削除、二重セミコロン修正（適用済み）。
- sandbox_graphics/CMakeLists.txt・engine/CMakeLists.txt: add_executable/add_libraryに紛れ込んだscene系ヘッダを除去（適用済み）。

## 本セッションでの実施結果（骨格作成・完了）
CLAUDE.mdのルール（コード生成は宣言まで）に従い以下を実施。**骨格はすべてコンパイル可能な構成**にしてあり、全ターゲットのビルドとctest全パスを確認済み。ただし`Buffer`コンストラクタ等がTODOスタブのため、**ユーザー実装完了までsandbox_graphicsは正常に描画されない**。

- **新規**: [buffer.hpp](../../engine/include/sq/graphics/buffer.hpp)（宣言＋日本語コメント）/ [buffer.cpp](../../engine/src/graphics/buffer.cpp)（TODOスケルトン。生成フロー5段・map/unmap/find_memory_typeの手順をコメントで明記）
- **書き換え**: [mesh.hpp](../../engine/include/sq/graphics/mesh.hpp) / [uniform_buffer.hpp](../../engine/include/sq/graphics/uniform_buffer.hpp)（継承形に変更、重複宣言・メンバを削除）、[mesh.cpp](../../engine/src/graphics/mesh.cpp) / [uniform_buffer.cpp](../../engine/src/graphics/uniform_buffer.cpp)（コンストラクタ=基底初期化子リスト＋本体TODO。既存実装のbind()/vertex_count()/update()は保持、find_memory_type・旧デストラクタの重複を削除）
- **宣言変更**: [sync_objects.hpp](../../engine/include/sq/graphics/sync_objects.hpp)（クラスコメントで個数/インデックス対応を明記、`render_finished`の仮引数を`image_index`に変更）、[sync_objects.cpp](../../engine/src/graphics/sync_objects.cpp)（2ループ分割のTODOコメント、`(void)`削除。既存ループは残置＝現時点でもコンパイル可）
- **TODOコメント追加**: [renderer.cpp](../../engine/src/graphics/renderer.cpp)（カメラ不在フォールバック）、[swapchain.cpp](../../engine/src/graphics/swapchain.cpp)（formats空ガード)
- **クリーンアップ適用済み**: renderer.cppの`(void)`3行と二重セミコロン、CMakeLists 2ファイルのヘッダ混入除去、[engine/CMakeLists.txt](../../engine/CMakeLists.txt)に`buffer.cpp`追加

## ユーザーが実装する本体（TODO・推奨順）
1. `sync_objects.cpp`: コンストラクタの2ループ分割（既存コードの再配置。render_finishedを`swapchain_image_count`個に）
2. `renderer.cpp`: カメラ不在時の単位行列フォールバック（`is_null()`チェック）
3. `swapchain.cpp`: formats空（count==0）ガードのthrow 1行
4. `buffer.cpp`: コンストラクタ（生成フロー5段。**vkAllocateMemoryの戻り値チェックも今回追加** — 旧コードは未チェックだった）、デストラクタ、`map()`/`unmap()`、`find_memory_type`
5. `mesh.cpp`: コンストラクタ本体（`map()`→`std::memcpy`→`unmap()`。`<cstring>`要）
6. `uniform_buffer.cpp`: コンストラクタ（`mapped_ = map();`）とデストラクタ（`unmap()`）

## 検証方法
- ビルド: CLion同梱のcmakeを使うこと（buildツリーがCMake 4.2構成のため、VS同梱の4.3やPATH上の別バージョンでは再構成に失敗する）:
  `& "C:\Users\user\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe" --build build --config Debug`
- SyncObjects修正後: 実行して描画継続、**ウィンドウリサイズ・最小化復帰を繰り返して**セマフォ関連のバリデーションエラーが出ないこと。
- カメラフォールバック後: `sandbox_graphics/main.cpp`のカメラエンティティ追加を一時コメントアウトして起動→クラッシュせず描画されること（単位行列=クリップ空間直描き）。
- Buffer実装後: 三角形描画・カメラ旋回がPhase 5と同一に動作すること。
- `ctest`で既存ECSテストが全パスすること（骨格時点で確認済み）。

## スコープ外（注記のみ）
- `VK_ERROR_OUT_OF_DATE_KHR`で早期returnした際の`image_available`セマフォ信号残留（直後にSyncObjects再作成されるため実害なし）
- 複数カメラのアクティブ選択ロジック（「最初の1つ」方針を維持）
- ステージングバッファ・StorageBuffer対応（`Buffer`基底はその土台になる）
