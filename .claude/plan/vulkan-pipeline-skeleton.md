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

## このフェーズで作らないもの（後続フェーズ）
- 実際のVulkan API呼び出し実装（インスタンス生成〜三角形描画までの実コード）
- シェーダーのコンパイル（glslangValidator/glslcの統合、SPIR-Vビルドステップ）
- ECSとの連携（Mesh/Transformコンポーネントの導入、Registryからの描画データ抽出）

## 検証方法
- `cmake --preset=default`でvcpkgの新規依存（glfw3, glm, vulkan）が解決され、構成が通ることを確認
- `cmake --build build`で新規ファイル群（宣言のみ、空実装）がコンパイルエラーなく通ることを確認
- `sandbox_graphics`実行ファイルが生成されることを確認（ビルド通過のみが目的、ウィンドウは開かない）

## 実施結果（完了）
- `engine/include/sq/graphics/`に11個のヘッダ（window, vulkan_instance, debug_messenger, physical_device, device, swapchain, render_pass, graphics_pipeline, command_buffers, sync_objects, renderer）と対応する`.cpp`を作成。すべて宣言＋TODOコメントのスケルトン
- `shaders/triangle.vert`, `shaders/triangle.frag`をTODOコメントのみの空ファイルとして作成
- `sandbox_graphics/`（`main.cpp`, `CMakeLists.txt`）を新規作成
- `vcpkg.json`に`glfw3`, `glm`, `vulkan`を追加し、`cmake --preset=default`で解決確認（Vulkan 1.4.350が検出された）
- `engine/CMakeLists.txt`にgraphics系ソース一式とVulkan/GLFW/GLMのリンクを追加
- ルート`CMakeLists.txt`に`sandbox_graphics`サブディレクトリを追加
- `cmake --build build --config Debug`で全ターゲット（`sq_engine_lib`, `sandbox`, `sandbox_graphics`, `ecs_tests`）のビルドが成功
- `ctest`で既存ECSテスト（7ケース、日本語TEST_CASE名含む）が引き続き全パスすることを確認

## 次のフェーズ（未実施）
1. `vulkan_instance.cpp`からインスタンス生成・検証レイヤーの実装に着手
2. シェーダーのGLSL記述とSPIR-Vコンパイル（glslc）パイプラインの統合
3. `renderer.cpp`のTODO順に沿って、三角形が画面に表示されるまで実装
4. ECSのPosition/Meshコンポーネントを描画データに繋げる
