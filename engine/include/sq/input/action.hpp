#pragma once

#include <cstddef>

namespace sq::input {

// エンジンが提供する抽象操作（phase10プラン B-1）。
// 物理キー/ボタンとの対応は InputMap が持つため、ゲームロジック側はこの enum だけを見ればよい。
//
// 末尾の Count は配列サイズ用の番兵。新しい Action は必ず Count の**前**に追加すること。
enum class Action {
    MoveForward,
    MoveBackward,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    CursorEnable,        // 押下中のみカーソル表示（既定: 左ALT）
    ToggleFullscreen,
    Quit,
    SwitchCamera,
    Count,
};

inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

// Action を配列添字へ変換する小ヘルパ。
[[nodiscard]] inline constexpr std::size_t action_index(const Action action) {
    return static_cast<std::size_t>(action);
}

}  // namespace sq::input
