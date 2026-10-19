#pragma once

#include "sq/ecs/entity_commands.hpp"
#include "sq/ecs/registry.hpp"

namespace sq::ecs {

// 入力に依存しない更新処理の基底クラス（phase10プラン A-1）。
// SystemScheduler が毎更新tickで update() を呼ぶ。dt は前回updateからの経過秒。
//
// registry を const& で受けるのは本エンジンの慣習に合わせるため
// （View::each() は const registry からコンポーネントの非const参照を渡す。Renderer::draw_frame も同様）。
//
// phase17 ④-3 / D-10: エンティティの生成・破棄は commands に積む（registry は const のまま）。
//   実際の変更は SystemScheduler::update の末尾でまとめて行われる。
//   ★ registry を非 const にしないのは、「System の中で直接 destroy できてしまう」状態を**型で**防ぐため。
//     each() の中で destroy すると反復が壊れる罠を、コンパイルエラーに変える。
// ★ コンポーネントの**値**の書き換えは、これまでどおり each() の中で直接行ってよい（構造変更ではない）。
class System {
public:
    virtual ~System() = default;

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    virtual void update(const Registry& registry, EntityCommands& commands, float dt) = 0;

protected:
    System() = default;  // 抽象基底。派生クラスからのみ構築する
};

}  // namespace sq::ecs
