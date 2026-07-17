# C++ ゲームエンジン（学習目的）— フルスクリーン対応（GLFW切替 + VK_EXT_full_screen_exclusive）

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

実装方針（ユーザー実装）:
- `set_fullscreen(true)`: `glfwGetWindowPos`/`glfwGetWindowSize` で現状を退避 → ウィンドウ中心が乗っているモニタを特定（単純にはglfwGetPrimaryMonitor()でも可）→ `glfwGetVideoMode` → `glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate)`
- `set_fullscreen(false)`: `glfwSetWindowMonitor(window_, nullptr, 退避した位置とサイズ..., 0)`
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

draw_frame のエラーハンドリング追加（**acquireとpresentの両方**）:
- `VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT` を `VK_ERROR_OUT_OF_DATE_KHR` と同様に扱い、recreate_swapchainする
  - 排他モードはスタートメニュー表示・Alt+Tab等で勝手に解除される（記事より）。recreate後に再acquireを試み、失敗したら排他なしで続行
- `recreate_swapchain()`: swapchain再作成後、フルスクリーン中なら `swapchain_->acquire_full_screen_exclusive()` を呼ぶ

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

## 注意点・ハマりどころ（記事より）

- **検証レイヤーは排他モードの失敗理由を出してくれない**（デバッグしづらい領域）
- ウィンドウサイズ・解像度・HMONITORの一貫性が必須（フルスクリーン化前にacquireすると失敗する）
- スタートメニューやウィンドウ切替で排他モードは自動解除される → MODE_LOSTハンドリング必須
- 先日のドライバ問題の教訓: この領域はドライバ相性が出やすい。おかしくなったらまずvkcube等で環境切り分けを
