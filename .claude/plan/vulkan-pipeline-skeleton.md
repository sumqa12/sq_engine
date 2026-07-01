# C++ ゲームエンジン（学習目的）— Phase 2: Vulkan描画パイプラインの骨格設計

## Context
Phase 1で自作ECS（Archetype/SoA方式、`sq_engine_lib`）を実装済み（[c-2d-3d-streamed-axolotl.md](c-2d-3d-streamed-axolotl.md)参照）。次は2D/3D両対応を見据えたVulkan描画パイプラインに着手する。**学習目的のため、このフェーズでは実際のVulkan API呼び出しコードは書かない**。クラス/関数の宣言（インターフェースの骨格）とディレクトリ構成だけを用意し、各ファイルが何を担うかをコメントで明示する。実装そのものはユーザー自身が後続のセッションで一つずつ書いていく。

Vulkan SDK 1.4.341.1が`D:\VulkanSDK\1.4.341.1`に導入済み・`VULKAN_SDK`環境変数も設定済みのため、SDKの追加インストールは不要だった。

## 技術選定
- ウィンドウ管理: **GLFW**（vcpkgで導入。`glfw3`）
- 数学: **GLM**（vcpkgで導入。`glm`）
- Vulkanバインディング: vcpkgの`vulkan`ポート（`find_package(Vulkan REQUIRED)`、ヘッダ/ローダーはVulkan SDK由来）
- 検証レイヤー: Vulkan SDKに同梱の`VK_LAYER_KHRONOS_validation`をDebugビルドで有効化。`VkDebugUtilsMessengerEXT`によるコールバックの骨格をこの段階で用意
- C++20 / `/utf-8`は既存設定を継続利用

## ディレクトリ構成（新規追加分）
```
sq_engine/
├── engine/
│   ├── include/sq/graphics/
│   │   ├── window.hpp              # GLFWウィンドウのRAIIラッパー宣言
│   │   ├── vulkan_instance.hpp     # VkInstance生成 + 検証レイヤー設定の宣言
│   │   ├── debug_messenger.hpp     # VkDebugUtilsMessengerEXTのセットアップ宣言
│   │   ├── physical_device.hpp     # GPU選択・キューファミリ検索の宣言
│   │   ├── device.hpp              # 論理デバイス・キュー取得の宣言
│   │   ├── swapchain.hpp           # サーフェス・スワップチェーン管理の宣言
│   │   ├── render_pass.hpp         # レンダーパス定義の宣言
│   │   ├── graphics_pipeline.hpp   # シェーダーステージ・パイプライン構築の宣言
│   │   ├── command_buffers.hpp     # コマンドプール/バッファ管理の宣言
│   │   ├── sync_objects.hpp        # セマフォ・フェンス管理の宣言
│   │   └── renderer.hpp            # 上記を束ねるRenderer本体の宣言（フレーム描画API）
│   └── src/graphics/
│       └── （上記それぞれに対応する.cpp、すべて宣言通りのスケルトン＋TODOコメント）
├── shaders/
│   ├── triangle.vert               # 学習用の最初のシェーダー（TODOコメントのみ）
│   └── triangle.frag
└── sandbox_graphics/
    ├── CMakeLists.txt              # Vulkan描画確認用の新規実行ターゲット
    └── main.cpp                    # 宣言のみ呼び出すmain（中身はTODO）
```

既存の`sandbox/`（ECSのみのCPUシミュレーション）は変更せず残した。Vulkan版は`sandbox_graphics/`として分離。

## ファイル内容の方針
- 各`.hpp`: クラス定義・メンバ関数の宣言・必要なメンバ変数の型まで記述。実装本体は書かない。クラスの責務をクラスコメントで1〜2行説明。
- 各`.cpp`: `#include`と関数スケルトン（本体は`// TODO: ...`コメントで、何のVulkan API呼び出しをどの順で行うべきかを具体的に記載）。
- `Renderer`クラスは各サブシステム（Window/Instance/Device/Swapchain/Pipeline/CommandBuffers/Sync）を所有し、`run()`, `draw_frame()`のような最上位APIだけ実装の入り口を持つ。コンストラクタのTODOコメントに初期化順序（Window→Instance→DebugMessenger→Surface→PhysicalDevice→Device→Swapchain→RenderPass→Pipeline→Framebuffers→CommandBuffers→Sync）を明記。

## CMake変更点
- `vcpkg.json`に`glfw3`, `glm`, `vulkan`を追加（既存の`builtin-baseline`は維持）
- `engine/CMakeLists.txt`: `find_package(Vulkan REQUIRED)`, `find_package(glfw3 CONFIG REQUIRED)`, `find_package(glm CONFIG REQUIRED)`を追加し、`sq_engine_lib`に`src/graphics/*.cpp`一式を追加、`target_link_libraries`に`Vulkan::Vulkan`, `glfw`, `glm::glm`を追加
- ルート`CMakeLists.txt`に`add_subdirectory(sandbox_graphics)`を追加
- 新規`sandbox_graphics/CMakeLists.txt`: `sandbox`と同様の構成で`sq_engine_lib`をリンク

## このフェーズ（骨格設計時点）で作らないとしていたもの
- 実際のVulkan API呼び出し実装（インスタンス生成〜三角形描画までの実コード）→ **Phase 3で実装完了**
- シェーダーのコンパイル（glslangValidator/glslcの統合、SPIR-Vビルドステップ）→ **Phase 3で実装完了**
- ECSとの連携（Mesh/Transformコンポーネントの導入、Registryからの描画データ抽出）→ 引き続き未実施

## 検証方法
- `cmake --preset=default`でvcpkgの新規依存（glfw3, glm, vulkan）が解決され、構成が通ることを確認
- `cmake --build build`で新規ファイル群（宣言のみ、空実装）がコンパイルエラーなく通ることを確認
- `sandbox_graphics`実行ファイルが生成されることを確認（ビルド通過のみが目的、ウィンドウは開かない）
- （Phase 3）`sandbox_graphics`を実際に起動し、ウィンドウに三角形が描画されることを目視確認

## 実施結果（骨格設計フェーズ・完了）
- `engine/include/sq/graphics/`に11個のヘッダ（window, vulkan_instance, debug_messenger, physical_device, device, swapchain, render_pass, graphics_pipeline, command_buffers, sync_objects, renderer）と対応する`.cpp`を作成。すべて宣言＋TODOコメントのスケルトン
- `shaders/triangle.vert`, `shaders/triangle.frag`をTODOコメントのみの空ファイルとして作成
- `sandbox_graphics/`（`main.cpp`, `CMakeLists.txt`）を新規作成
- `vcpkg.json`に`glfw3`, `glm`, `vulkan`を追加し、`cmake --preset=default`で解決確認（Vulkan 1.4.350が検出された）
- `engine/CMakeLists.txt`にgraphics系ソース一式とVulkan/GLFW/GLMのリンクを追加
- ルート`CMakeLists.txt`に`sandbox_graphics`サブディレクトリを追加
- `cmake --build build --config Debug`で全ターゲット（`sq_engine_lib`, `sandbox`, `sandbox_graphics`, `ecs_tests`）のビルドが成功
- `ctest`で既存ECSテスト（7ケース、日本語TEST_CASE名含む）が引き続き全パスすることを確認

## Phase 3: 実装フェーズ（完了）
骨格に沿って、ユーザー自身が各`.cpp`のTODOを実コードに置き換え、三角形描画まで到達した。

### 実装した内容
- **`vulkan_instance.cpp`**: `VkApplicationInfo`/`VkInstanceCreateInfo`を設定し`vkCreateInstance`。検証レイヤー有効化は`NDEBUG`マクロで判定（`#if !defined(NDEBUG)`）し、`supports_validation_layer()`で`VK_LAYER_KHRONOS_validation`の対応を`vkEnumerateInstanceLayerProperties`で確認
- **`window.cpp`**: `GLFW_INCLUDE_VULKAN`を定義した上で`<GLFW/glfw3.h>`をinclude（`glfw3native.h`は不要、ネイティブハンドルが必要な場合のみ使う）。`glfwInit`/`glfwCreateWindow`/`glfwGetRequiredInstanceExtensions`を実装
- **`debug_messenger.cpp`**: `vkGetInstanceProcAddr`経由で`vkCreateDebugUtilsMessengerEXT`/`vkDestroyDebugUtilsMessengerEXT`を取得して呼び出し。コールバックはseverityに応じて`spdlog`へ出力
- **`physical_device.cpp` / `device.cpp`**: 骨格のみで未着手（`Renderer`からは呼ばれているが、実体は今後の課題）
- **`swapchain.cpp`**: `vkGetPhysicalDeviceSurfaceFormatsKHR`/`PresentModesKHR`/`CapabilitiesKHR`は`physical_device`、`vkCreateSwapchainKHR`等のリソース生成・破棄は`device`に渡す区別を実装。実装過程で見つかった注意点：
  - `oldSwapchain`の扱い・`image_views_`/`images_`の`clear()`漏れ・`image_format_`のフォールバック漏れが課題として残っている（`recreate()`時のバグの可能性、未修正）
  - 画像数は`caps.minImageCount`をそのまま採用（`+1`しない方針に変更）。`minImageCount`が1を返す環境ではダブルバッファリングできない可能性があるため、`std::max(caps.minImageCount, 2u)`への変更を将来的に検討
- **`graphics_pipeline.cpp`**: シェーダーモジュールのロード、頂点入力なし（ハードコードされた三角形）、`VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`、ビューポート/ラスタライズ/マルチサンプル/カラーブレンド状態を設定し`vkCreateGraphicsPipelines`
- **`command_buffers.cpp`**: コマンドプール生成（`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`）、`record()`で`vkResetCommandBuffer`→`vkBeginCommandBuffer`→ユーザー記録コールバック→`vkEndCommandBuffer`
- **`sync_objects.cpp`**: `image_available`/`render_finished`セマフォと`in_flight_fence`をフレーム数分生成
- **`renderer.cpp`**: コンストラクタで骨格通りの初期化順序（Window→Instance→DebugMessenger→Surface→PhysicalDevice→Device→Swapchain→RenderPass→Pipeline→Framebuffers→CommandBuffers→Sync）を実装。`draw_frame()`で取得→記録→送信→提示の1フレームループを実装
- **シェーダー**: `triangle.vert`/`triangle.frag`をGLSLで記述（頂点バッファなし、`gl_VertexIndex`から座標・色をハードコード出力）。`sandbox_graphics/CMakeLists.txt`に`glslc`を使ったカスタムビルドステップを追加し、`.vert`/`.frag`→`.spv`へコンパイルして実行ファイル隣の`shaders/`にコピーするようにした

### 実装中に発生し、解決したバグ
1. **MSVCの日本語コメント文字化け**: 既存の`/utf-8`コンパイルオプションの対象範囲内のため、コメントは問題なく動作
2. **`CommandBuffers::record()`が`vkResetCommandPool`を呼んでいた**: プール全体をリセットすると他のインデックスのコマンドバッファ（pending状態の可能性あり）も巻き込んでしまい、`must not be in the pending state`のバリデーションエラーが発生。`vkResetCommandPool`の呼び出しを削除し、`vkResetCommandBuffer`（単体リセット）のみに修正
3. **`draw_frame()`で`vkWaitForFences`/`vkResetFences`が未実装だった**: GPUがまだ使用中のコマンドバッファ/フェンスを再利用しようとして同様のバリデーションエラーが発生。フレーム冒頭で前回フレームの完了待ち→リセットを行うよう修正
4. **`vkAcquireNextImageKHR`にフェンスを渡していた**: `in_flight_fence`を「画像取得時」と「`vkQueueSubmit`時」の二重にシグナルしようとして不正な状態になっていた。`vkAcquireNextImageKHR`のフェンス引数は`VK_NULL_HANDLE`に変更
5. **`pWaitDstStageMask`が毎フレーム`new`でヒープリークしていた**: ローカル変数のアドレスを渡す形に修正
6. **`present_info`に`swapchainCount`/`pSwapchains`が未設定だった**: 必須フィールド漏れにより未定義動作の原因になっていたため追加
7. **`render_finished`セマフォの再利用バグ（`pSignalSemaphores ... is being signaled ... but it may still be in use`）**: `render_finished`セマフォを`current_frame_`（フレームインフライト数=2）でインデックスしていたが、スワップチェーン画像枚数（3枚）と数が合わず、プレゼンテーションでまだ参照されている可能性のあるセマフォを再シグナルしてしまっていた。`image_available`/`in_flight_fence`はフレームインフライト数のまま`current_frame_`でインデックスし、`render_finished`だけはスワップチェーン画像枚数分用意して`image_index`でインデックスするよう設計変更が必要（`SyncObjects`のコンストラクタに`swapchain_image_count`引数を追加する案を提示）

### 結果
上記の修正を経て、`sandbox_graphics`がウィンドウを開き、検証レイヤーのエラーなく三角形を描画できる状態まで到達した。

## 次のフェーズ（未実施）
1. `swapchain.cpp`の`oldSwapchain`/`image_views_`クリア漏れ/`image_format_`フォールバック漏れの修正（ウィンドウリサイズ時の`recreate_swapchain()`で問題化する可能性）
2. `render_finished`セマフォをスワップチェーン画像枚数分に分離する設計変更の適用
3. ECSのPosition/Meshコンポーネントを描画データに繋げる（インスタンス描画への発展）
4. ウィンドウリサイズ時のスワップチェーン再作成の動作確認
