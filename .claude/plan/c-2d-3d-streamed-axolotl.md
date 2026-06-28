# C++ ゲームエンジン（学習目的）— Phase 1: プロジェクト基盤 + 自作ECS実装

## Context
2D/3D両対応のゲームエンジンをC++で学習目的で自作する。描画APIはVulkanを採用予定だが、まず最初のフェーズではアーキテクチャの核となる**自作ECS（Entity-Component-System）**をArchetype/SoA方式で実装する。Vulkan描画は次フェーズ。現在ディレクトリは空のため、プロジェクト基盤も合わせて構築する。

## 技術選定
- 言語: C++20（concepts, ranges を活用しECSの型安全性を高める）
- ビルド: CMake + vcpkg（manifestモード, `vcpkg.json`）
- テスト: Catch2（vcpkgで導入）
- ログ: spdlog（vcpkgで導入、デバッグ出力に使う）
- 将来のVulkan/GLFW/GLMは今回は導入しない（次フェーズで追加）

## ディレクトリ構成
```
sq_engine/
├── CMakeLists.txt
├── vcpkg.json
├── engine/
│   ├── CMakeLists.txt
│   ├── include/sq/ecs/
│   │   ├── entity.hpp          # Entity = ID + generation (struct, 軽量)
│   │   ├── component_storage.hpp  # SoA配列 + denseバッファ
│   │   ├── archetype.hpp       # コンポーネント集合 → 専用ストレージ
│   │   ├── registry.hpp        # World/Registry: create/destroy/add/remove/view
│   │   └── view.hpp            # 複数コンポーネントの反復用ビュー
│   └── src/ecs/
│       ├── registry.cpp
│       └── archetype.cpp
├── tests/
│   ├── CMakeLists.txt
│   └── ecs_tests.cpp
└── sandbox/                    # 動作確認用の簡易main
    └── main.cpp
```

## ECS設計詳細（Archetype/SoA方式）

1. **Entity**: `uint32_t id` + `uint32_t generation` を1つの`uint64_t`にパックしたハンドル。世代カウンタで「削除済みID再利用」によるダングリング参照を検出。

2. **ComponentStorage<T>**: `std::vector<T>` による密配列（SoA）。Archetype単位で各コンポーネント型ごとに1つ保持。

3. **Archetype**: 同じコンポーネント集合を持つEntity群をまとめる。
   - `std::vector<Entity>` で所属Entity一覧
   - `unordered_map<ComponentTypeId, std::unique_ptr<IComponentStorage>>` で各コンポーネント配列を保持
   - Entity追加/削除時は配列末尾とswapしてO(1)で削除（swap-and-pop）

4. **Registry（World）**:
   - `create()` / `destroy(Entity)`
   - `add<T>(Entity, T)` / `remove<T>(Entity)` → 該当Entityを古いArchetypeから新しいArchetypeへ移動（コンポーネントデータをコピー/ムーブ）
   - `get<T>(Entity)` / `has<T>(Entity)`
   - Entity→Archetype＋index のマッピングを`vector<EntityRecord>`（denseなEntity ID配列インデックス）で管理

5. **View<Components...>**: 該当する全コンポーネントを持つArchetypeを横断してイテレートする軽量ビュー。`registry.view<Position, Velocity>()` のような形でforeachできるようにする。

## 実装ステップ
1. プロジェクト基盤: `CMakeLists.txt`, `vcpkg.json`, ディレクトリ作成、Catch2/spdlogのvcpkg統合確認
2. `entity.hpp`: Entityハンドル定義
3. `component_storage.hpp`: 型消去された`IComponentStorage`インターフェースと`ComponentStorage<T>`実装
4. `archetype.hpp/.cpp`: Archetype管理、Entity追加削除のswap-and-pop実装
5. `registry.hpp/.cpp`: create/destroy/add/remove/get/has、Archetype間のEntity移動ロジック
6. `view.hpp`: 複数コンポーネントを持つEntityの反復処理
7. `tests/ecs_tests.cpp`（Catch2）: 生成・破棄・コンポーネント追加削除・view反復・世代カウンタによるダングリング検出のテスト
8. `sandbox/main.cpp`: Position/Velocityコンポーネントを使った簡易移動シミュレーションで動作確認

## 検証方法
- `cmake --preset default && cmake --build build` でビルド成功を確認
- `ctest` でECSの単体テスト（生成/破棄/add/remove/view反復）が全てパスすることを確認
- `sandbox` 実行バイナリでN個のEntityに対しPosition+=Velocityのループを数フレーム実行し、結果をログ出力して目視確認

## 実施結果（完了）
- vcpkgをクローン・bootstrapし、`VCPKG_ROOT`をユーザー環境変数として永続化（`D:\CLionProjects\vcpkg`）
- CMake + Visual Studio 2026ジェネレータでビルド環境構築（`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json` に `builtin-baseline` 追加）
- ECSコア一式（entity/component_storage/archetype/registry/view）を実装
- Catch2テスト7件・全パス、sandboxでPosition+Velocityの移動シミュレーション動作確認済み
- 追加対応: MSVCで日本語TEST_CASE名が文字化けする問題を解消
  - `CMakeLists.txt` に `/utf-8` コンパイルオプションを追加（MSVC限定）
  - `tests/CMakeLists.txt` で `catch_discover_tests` ではなく `add_test(NAME ecs_tests COMMAND ecs_tests)` を使用（個別テスト発見時のJSONパース経路でのコードページ変換による文字化けを回避）

## 次のフェーズ（未実施）
Vulkan描画パイプラインの導入（GLFW/GLMをvcpkgに追加、Vulkanインスタンス・スワップチェーン作成、簡易三角形描画）
