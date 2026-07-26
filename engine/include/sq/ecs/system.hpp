#pragma once

#include "sq/ecs/registry.hpp"

namespace sq::ecs {

// 入力に依存しない更新処理の基底クラス（phase10プラン A-1）。
// SystemScheduler が毎更新tickで update() を呼ぶ。dt は前回updateからの経過秒。
//
// registry を const& で受けるのは本エンジンの慣習に合わせるため
// （View::each() は const registry からコンポーネントの非const参照を渡す。Renderer::draw_frame も同様）。
// エンティティの生成・破棄を伴うシステムが必要になったら非constへ拡張する（将来課題）。
class System {
public:
    virtual ~System() = default;

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    virtual void update(const Registry& registry, float dt) = 0;

protected:
    System() = default;  // 抽象基底。派生クラスからのみ構築する
};

}  // namespace sq::ecs
