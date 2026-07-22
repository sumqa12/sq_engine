# C++ ゲームエンジン（学習目的）— Phase 9: 入力システム（InputManager + 操作対象コンポーネント + FreeFlyカメラ + マウス視線）

## Context
Phase 8でテクスチャ＋アルファブレンディングまで到達した（[phase8-texture-mapping.md](phase8-texture-mapping.md)参照）。次はカメラを動かすための**入力処理**を実装する。

現状: キー取得は `Window::is_key_pressed`（`glfwGetKey` ポーリング）と `sandbox_graphics/main.cpp` のF11手動エッジ検出のみ。カメラは `main.cpp` の周回ロジックで自動的に動くだけで、ユーザーが操作できない。

目標:
1. **コールバック方式の入力状態管理クラス**（押下/解放をコールバックで保存 → ループ＝いずれ入力システムが問い合わせ）
2. **エンティティごとの操作方法**を表すコンポーネント
3. **操作対象フラグ**のコンポーネント
4. **キーボード＋マウスでカメラをFreeFly操作**（WASD移動＋右ドラッグ視線）

**確定事項**: 操作スキーム = **FreeFly**、マウス入力 = **含める（右ドラッグ視線）**。

例によってClaudeは宣言・骨格・TODOコメントまで、実装本体はユーザーが書く（CLAUDE.md）。

## 設計判断（重要）
- **InputManagerはGLFW非依存**にし、GLFWコールバック登録は`Window`が担って `(key/button/cursor/scroll)` を転送する。
  - 理由: `Window`は既に `glfwSetWindowUserPointer(window_, this)` を `framebuffer_size_callback`/`focus_callback` で使用中。
    入力クラスが自前で user pointer を奪うと既存コールバックが壊れる。`Window`が同じ user pointer=`Window*` 経由で受け、
    登録済みの `std::function` へ転送する（既存 focus/resize と同じ方式）。
- **InputManagerの所有はアプリ（sandbox main）**。`new_frame()`のタイミングと問い合わせをループ側に置く方が素直。
  `Renderer::set_*_callback(...)` で配線し、Rendererに入力ロジックを持たせない。
- **フレームモデル（全入力共通）**: `new_frame()` を毎フレーム先頭 → `Window::poll_events()` → 問い合わせ、の順。
  `new_frame()` は key/button の `previous_=current_` スナップショットと、cursor/scroll の累積デルタの0リセットを行う。
  これで `is_pressed`（押した瞬間）と `cursor_delta`（このフレームの移動量）が正しく取れる。
- **連続移動は `is_down`、トグルは `is_pressed`**。更新tick(30UPS)とポーリング(毎ループ)のカデンツ差のため、
  更新tick内の移動は `is_down`/`cursor_delta` ベースにする。
- **コンポーネントは2つに分割**: `ControlTarget`（タグ＝操作対象フラグ）と `Controller`（scheme＋速度＋FreeFlyのyaw/pitch状態）。
- **マウス視線は右ボタン押下中のみ**。押下中は `glfwSetInputMode(GLFW_CURSOR, GLFW_CURSOR_DISABLED)` でカーソルをキャプチャ。

## 本セッションでの実施結果（骨格作成・完了）
CLAUDE.mdのルール（コード生成は宣言まで）に従い以下を作成。**全ターゲットのビルド成功・ctest全パスを確認済み**。
入力は未配線（TODO）のため、ユーザー実装完了までカメラ操作は動かない（従来の周回描画のまま）。

- **新規**:
  - [input_manager.hpp](../../engine/include/sq/input/input_manager.hpp)（宣言）/ [input_manager.cpp](../../engine/src/input/input_manager.cpp)（TODOスタブ）
    — GLFW非依存。key/button/cursor/scroll の on_* とnew_frame、is_down/is_pressed/is_released、cursor_delta/scroll_delta
  - [input_system.hpp](../../engine/include/sq/input/input_system.hpp) / [input_system.cpp](../../engine/src/input/input_system.cpp)（TODO）
    — `update_camera_control(const Registry&, const InputManager&, float dt)`。FreeFlyの手順をTODOで明記
  - [controller.hpp](../../engine/include/sq/scene/controller.hpp)（ヘッダオンリー）— `ControlScheme` / `Controller`（yaw/pitch含む）/ `ControlTarget`
- **変更（宣言 + TODO）**:
  - [window.hpp](../../engine/include/sq/graphics/window.hpp) / [window.cpp](../../engine/src/graphics/window.cpp)
    — `KeyCallback`等4型＋`set_*_callback`＋`set_cursor_captured`、privateに4静的コールバックと転送先メンバ。
      コンストラクタの `glfwSet*Callback` 登録・転送本体・キャプチャ本体はTODO
  - [renderer.hpp](../../engine/include/sq/graphics/renderer.hpp) / [renderer.cpp](../../engine/src/graphics/renderer.cpp)
    — `set_*_callback`／`set_cursor_captured` を追加し `window_` へ委譲（実装済み。既存 delegate と同様）
  - [sandbox_graphics/main.cpp](../../sandbox_graphics/main.cpp) — input系includeとTODO（生成/配線、new_frame、F11のis_pressed置換、
    右ボタンでのカーソルキャプチャ、update_camera_control呼び出し、カメラへのControlTarget/Controller付与、周回整理）
  - [engine/CMakeLists.txt](../../engine/CMakeLists.txt) — `src/input/input_manager.cpp`・`src/input/input_system.cpp` を追加

## ユーザーが実装する本体（TODO・推奨順）
1. `input_manager.cpp`（on_*・new_frame・is_*・cursor_delta/scroll_delta。範囲チェック込み）
2. `window.cpp`（コンストラクタで `glfwSet*Callback` 登録、静的コールバックの転送、setterの保存、`set_cursor_captured`）
3. main で InputManager 生成・配線、`new_frame()`→`poll_events()`、F11を `is_pressed` へ、右ボタンでキャプチャ切替
4. `controller.hpp` は完成済み。`update_camera_control` の FreeFly 実装 + カメラに `ControlTarget`/`Controller` 付与
5. 速度/感度/pitchクランプ/ズーム微調整、周回ロジック整理、必要なら `Camera::far_plane` 拡大

## 検証方法
- ビルド（CLion同梱cmake）: `& "C:\Users\user\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe" --build build/debug --config Debug`
- 実行: 右ボタンドラッグで視線、WASD＋Q/Eで移動。（任意でホイールズーム）
- リサイズ・フルスクリーン切替(F11)後も入力継続。フォーカス喪失ポーズ（`is_focused()`）と両立し暴発しない。キャプチャ解除で通常カーソルに戻る。
- F11トグルが `InputManager` 経由で従来通り動く。
- `ctest -C Debug` 全パス（ECS非変更、確認済み）。

## 将来課題（このフェーズではやらない）
- Orbit等の追加スキーム、Transform/Position駆動の操作、操作対象の切替。
- 入力を正式なECSの System 抽象へ昇格（現状 free 関数）。
- キーコンフィグ層（GLFWキー → エンジン抽象アクション。InputManagerのGLFW非依存を活かす）。
