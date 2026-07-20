# C++ ゲームエンジン（学習目的）— フルスクリーン対応（GLFW切替 + VK_EXT_full_screen_exclusive）【完了 2026-07-20】

**実装完了**: F11トグル（ボーダーレス方式）、FSE排他モードの取得/解放、MODE_LOST→release+recreateによるAlt+Tab復帰、フォーカス連動の排他再取得まで実機で動作確認済み。

## Context
次フェーズ（深度バッファ+3D化）の前に、フルスクリーン表示を実装する。参考: [Zenn: Vulkanで排他的フルスクリーン](https://zenn.dev/techmadot/articles/vk-exclusive-fullscreen)

2層構造で実装する:
1. **レイヤー1（必須）**: GLFWでウィンドウをフルスクリーン化。スワップチェーンは既存のリサイズ処理（resizedフラグ→recreate）が面倒を見る
2. **レイヤー2（本題）**: `VK_EXT_full_screen_exclusive`（Windows専用）でDWMを完全にバイパスする排他所有権を明示的に取得（`APPLICATION_CONTROLLED`）

## 拡張の有効化

### インスタンス（vulkan_instance.cpp）
GLFW必須拡張に加えて以下を追加（`std::vector<const char*>`にマージ）:
- `VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME`
- `VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME`

### デバイス（device.cpp）
- `VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME` を有効化
- 対応チェック: `PhysicalDeviceSelector::is_suitable` と同様に `vkEnumerateDeviceExtensionProperties` で確認し、非対応なら排他モードを諦めてレイヤー1のみで動く（`bool fullscreen_exclusive_supported_` を保持）

## Window の変更（window.hpp / window.cpp）

```cpp
// フルスクリーン切替。enabled=trueでウィンドウが今いるモニタ全面へ、falseで元のウィンドウ位置・サイズに復元。
void set_fullscreen(bool enabled);
[[nodiscard]] bool is_fullscreen() const;

// キー入力の状態を返す（glfwGetKey）。main側でのF11トグル用。
[[nodiscard]] bool is_key_pressed(int key) const;

// Win32ネイティブハンドル（HWND）。swapchainのHMONITOR取得に使う。
// windows.hのヘッダ汚染を避けるためvoid*で返す。
[[nodiscard]] void* win32_window() const;

private:
    bool fullscreen_ = false;
    int windowed_x_ = 0, windowed_y_ = 0;        // 復元用のウィンドウ位置
    int windowed_width_ = 0, windowed_height_ = 0;  // 復元用のサイズ
```

実装方針（ユーザー実装）。**決定事項: ビデオモード切替はせず、ボーダーレス方式にする**（切替が速い・Alt+Tabに強い・FSE拡張と本来セットの構成。記事のWS_POPUP化と同等）:
- `set_fullscreen(true)`: `glfwGetWindowPos`/`glfwGetWindowSize` で現状を退避 → `glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_FALSE)` → モニタの位置・ビデオモードを取得し、**monitor引数はnullptrのまま** `glfwSetWindowMonitor(window_, nullptr, mx, my, mode->width, mode->height, 0)` でモニタ全面に配置
- `set_fullscreen(false)`: `GLFW_DECORATED` を戻し、`glfwSetWindowMonitor(window_, nullptr, 退避した位置とサイズ..., 0)`
- ※monitor引数に実モニタを渡すとビデオモード切替（本物の排他的フルスクリーン）になる。この経路は採用しない
- どちらもフレームバッファサイズ変更→既存コールバックで `resized_` が立つ → 次のpresent後に自然にrecreateされる
- `win32_window()`: `#define GLFW_EXPOSE_NATIVE_WIN32` + `#include <GLFW/glfw3native.h>`（**window.cppのみ**でinclude）→ `glfwGetWin32Window(window_)`
- 記事はWin32スタイル（WS_POPUP化+Maximize）を直接操作しているが、GLFWでは `glfwSetWindowMonitor` が同等のことをやってくれるのでこちらを推奨

## Swapchain の変更（swapchain.hpp / swapchain.cpp）

```cpp
// use_fullscreen_exclusive: trueならVK_EXT_full_screen_exclusiveのpNextチェーンを付けて作成する
void create(std::uint32_t width, std::uint32_t height);  // 既存
void set_fullscreen_exclusive(bool enabled, void* hwnd); // recreate前に呼ぶ設定用（メンバに保持）

// 排他モードの取得/解放（vkGetDeviceProcAddrで関数ポインタを取得して呼ぶ）
VkResult acquire_full_screen_exclusive();
VkResult release_full_screen_exclusive();

private:
    bool fullscreen_exclusive_ = false;   // 次のcreateでpNextチェーンを付けるか
    void* hwnd_ = nullptr;                // HMONITOR導出用
    bool exclusive_acquired_ = false;     // 取得済みフラグ（二重acquire/release防止）
```

**サーフェス単位の対応チェック（必須・実装中に判明）**: 拡張が列挙されることと「このサーフェス+モニタで使えること」は別。`APPLICATION_CONTROLLED` を使う前に `vkGetPhysicalDeviceSurfaceCapabilities2KHR`（`vkGetInstanceProcAddr` で取得）で問い合わせ、`VkSurfaceCapabilitiesFullScreenExclusiveEXT::fullScreenExclusiveSupported == VK_TRUE` のときだけFSEチェーンを付ける。入力側は `VkPhysicalDeviceSurfaceInfo2KHR` → FSE Info → Win32 Info のチェーン、出力側は `VkSurfaceCapabilities2KHR` → `VkSurfaceCapabilitiesFullScreenExclusiveEXT`。このチェックを飛ばすと、非対応環境（Intel iGPU等）で `vkCreateSwapchainKHR` がドライバ内部（igvk64.dll）でアクセス違反を起こすことを確認済み。

`create()` 内のpNextチェーン（ユーザー実装、`#ifdef _WIN32` ガード推奨）:

```
VkSwapchainCreateInfoKHR
  └─ pNext → VkSurfaceFullScreenExclusiveInfoEXT
       .fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT
       └─ pNext → VkSurfaceFullScreenExclusiveWin32InfoEXT
            .hmonitor = MonitorFromWindow((HWND)hwnd_, MONITOR_DEFAULTTONEAREST)
```

- swapchain.cppで `#include <windows.h>`（`MonitorFromWindow`用）と `#include <vulkan/vulkan_win32.h>` が必要
- `vkAcquireFullScreenExclusiveModeEXT` / `vkReleaseFullScreenExclusiveModeEXT` はデバイス拡張関数なので `vkGetDeviceProcAddr` で取得
- **注意**: acquireは「スワップチェーン作成後、かつウィンドウが実際にフルスクリーン表示になった後」に呼ぶ。失敗時は `VK_ERROR_INITIALIZATION_FAILED` が返る → ログを出して排他なし（ボーダーレス）で続行する（throwしない）
- destroy/recreate前にacquire済みならreleaseする

## Renderer の変更（renderer.hpp / renderer.cpp）

```cpp
// フルスクリーン切替の公開API。Window切替 + 次フレームのrecreateで排他モードを取得する。
void set_fullscreen(bool enabled);
[[nodiscard]] bool is_fullscreen() const;
```

draw_frame のエラーハンドリング（**acquireとpresentの両方**。実機検証済みの最終形）:
- acquire SUCCESS / SUBOPTIMAL → 続行
- acquire OUT_OF_DATE → recreate → return
- acquire **MODE_LOST → release → recreate → return**
  - 仕様の文面上はrelease後に同じスワップチェーンを使い続けられそうに読めるが、**実ドライバは一度lostしたスワップチェーンのacquireがMODE_LOSTを返し続ける**（実機で確認）。OUT_OF_DATE同様に1回recreateするのが実務上の正解（DXVK等も同じ扱い）
  - 毎フレーム再作成ループにはならない: recreate後は排他未取得で始まり、排他なしのスワップチェーンはMODE_LOSTを返さないため（MODE_LOSTは「acquire済みの排他を奪われた」ときだけ）
- present OUT_OF_DATE / SUBOPTIMAL / resizedフラグ → recreate。present MODE_LOST → release → recreate
- **`vkResetFences` は acquire 成功後（submit確定後）に移動すること**。MODE_LOST等のreturnパスはSyncObjectsを再作成しないため、冒頭でリセットすると次フレームの `vkWaitForFences` がデッドロックする
- acquireがエラーを返したフレームは画像が無いので、record/submit/presentへ**進んではいけない**（違反すると「wait semaphore has no way to be signaled」「image not acquired」等の検証エラー群になる）
- 排他の再取得: 「フルスクリーン中 && `created_with_fse_` && 未acquire && フォーカスあり」のとき acquire を試行（`exclusive_acquired_` フラグで二重acquire防止。`created_with_fse_` = 現在のスワップチェーンがFSEチェーン付きで作成されたかのフラグ。acquireはFSE付きスワップチェーンにしか呼べない）
- `Swapchain::recreate()` 冒頭に `if (exclusive_acquired_) release_full_screen_exclusive();` ガードを置く
- `recreate_swapchain()`: swapchain再作成後、フルスクリーン中かつフォーカスありなら `acquire_full_screen_exclusive()` を呼ぶ

### フォーカス連動の描画ポーズ
- Window: `glfwSetWindowFocusCallback` で `focused_` フラグを管理、`is_focused()` を公開（Rendererに委譲メソッド）
- main.cpp: `is_fullscreen() && !is_focused()` の間は draw_frame をスキップし、`sleep_for(10ms)` 程度でCPUを休ませる（busyループ防止）。フォーカス復帰で描画再開＋排他再取得
- 最小化（extent 0）は既存の `wait_while_minimized()` が担当。フォーカス喪失ポーズとは別ケース

## sandbox_graphics/main.cpp

- ループ内でF11のエッジ検出（前フレームの押下状態を保持し、押した瞬間だけ）→ `renderer.set_fullscreen(!renderer.is_fullscreen())`

## 実装順序（推奨）

1. **レイヤー1のみ先に完成させる**: Window::set_fullscreen + main.cppのF11トグル。既存のリサイズ処理だけでフルスクリーン⇔ウィンドウが安定して切り替わることを確認
2. インスタンス/デバイス拡張の有効化（検証レイヤーにエラーが出ないこと）
3. SwapchainのpNextチェーン + acquire/release
4. RendererのMODE_LOSTハンドリング

## 検証方法

- F11でフルスクリーン⇔ウィンドウが往復できる（複数回繰り返しても検証エラーなし）
- フルスクリーン中にAlt+Tab / Winキー → MODE_LOSTから自動復帰する（クラッシュ・凍結しない）
- 排他モード取得の成否をログで確認（`vkAcquireFullScreenExclusiveModeEXT`の戻り値）
- 非対応環境（Intel機等）でもレイヤー1のみで動作すること
- ウィンドウ⇔フルスクリーンの切替を挟んでも三角形の回転・リサイズ処理が正常なこと

## 実装中に得た教訓（2026-07-17〜18）

- sType設定漏れは検証レイヤーに「unexpected VkStructureType **VK_STRUCTURE_TYPE_APPLICATION_INFO**」（enum値0）として現れる
- pNextチェーンの構造体をifブロック内で宣言すると、vkCreateSwapchainKHR呼び出し時にはダングリングポインタ（Phase 3のpWaitDstStageMaskと同型の罠）
- `HMONITOR` は不透明ハンドル。デバッガで「メモリが読めない」と出ても正常
- Intel Iris Xe機の `vkCreateSwapchainKHR` クラッシュ（igvk64.dll内AV）は**古いドライバが原因**だった（FSEチェーン無しでも再現）。Radeon機の2秒acquire停止も同様にドライバ更新で解決。**present/swapchain系の不可解な不具合は、コードを疑う前に (1)vkcubeで再現確認 (2)GPUドライバ更新** が鉄則

## 注意点・ハマりどころ（記事より）

- **検証レイヤーは排他モードの失敗理由を出してくれない**（デバッグしづらい領域）
- ウィンドウサイズ・解像度・HMONITORの一貫性が必須（フルスクリーン化前にacquireすると失敗する）
- スタートメニューやウィンドウ切替で排他モードは自動解除される → MODE_LOSTハンドリング必須
- 先日のドライバ問題の教訓: この領域はドライバ相性が出やすい。おかしくなったらまずvkcube等で環境切り分けを
