#pragma once

namespace sq::scene {

// 指定した秒数が経ったら、自分と子孫を消す印（phase17 ④-5）。
//
// LifetimeSystem が毎 tick remaining を減らし、0 以下になったら scene::destroy_hierarchy で破棄を積む。
// ★ ルートにだけ付ければよい（子孫は destroy_hierarchy がまとめて消す）。
//   子にも付けると、親と子が同じ tick に二重に積まれる（EntityCommands::flush が重複を除くので落ちはしない）。
struct Lifetime {
    float remaining = 0.0f;  // 残り秒数
};

}  // namespace sq::scene
