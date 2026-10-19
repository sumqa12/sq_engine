#pragma once

#include <functional>
#include <vector>

#include "sq/ecs/entity.hpp"
#include "sq/ecs/registry.hpp"

namespace sq::ecs {

// エンティティの構造変更を溜めておき、同期点でまとめて実行する（phase17 ④-2 / D-10）。
//
// なぜ要るのか:
//   - View::each() の中で destroy / add / remove すると、反復中のアーキタイプが swap-and-pop で並び替わり、
//     末尾の要素を飛ばすか、消えた要素を読む（④-1）
//   - System::update は const Registry& しか受け取らない（構造を変えられない）
//   → System は「読むのは const Registry&、変えるのは EntityCommands&」という形にし、
//     実際の変更は SystemScheduler::update の末尾（全 System の後）の1点でまとめて行う。
//   Unity DOTS の EntityCommandBuffer、Bevy の Commands と同じ考え方。
//
// ★ 子孫ごと消すのはここではなく scene::destroy_hierarchy（hierarchy.hpp）。
//   Parent は scene の概念で、ecs は階層を知らない層のまま保つ（ecs → scene の依存を作らない。D-10）。
// ★ System の外（main の起動時や、ループ中のキー処理）からは、これまでどおり Registry を直接変えてよい。
//   **反復中でなければ**問題は無い。EntityCommands は「System の中から変えたい」ときの仕組み。
class EntityCommands {
public:
    // そのエンティティだけを消す（同期点で）。
    // ★ 子は残る。親だけ消すと、子は死んだ親を Parent で指したまま TransformSystem で単位行列扱いになり、
    //   **原点へ飛ぶ**（④-1）。子孫ごと消すなら scene::destroy_hierarchy を使うこと。
    // ★ 同じエンティティを2回積んでも、既に死んだエンティティを積んでもよい（flush が黙って飛ばす）。
    void destroy(Entity entity);

    // 同期点で fn(registry) を実行する。生成・コンポーネントの付け外しはこれで書く。
    //   ★ fn の中では registry を自由に変えてよい（もう反復中ではない）。
    //   ★ fn が捕捉した参照の寿命に注意。flush は同じ tick の中なので、
    //     tick を跨がなければローカル変数の参照を捕捉しても安全。
    void defer(std::function<void(Registry&)> fn);

    // 溜めたコマンドを実行して空にする（SystemScheduler::update の末尾から呼ばれる）。
    //   1. defer を積んだ順に実行する
    //   2. destroy_ を重複除去し、is_alive のものだけ registry.destroy する
    //   ★ 破棄を最後にまとめる理由: defer が「消す予定のエンティティ」に add しても落ちないように。
    //     逆順だと、消したエンティティに add して例外になる。
    //   ★ defer の中で、さらに commands に積まれることは想定しない（積まれたら次の flush で実行される）。
    void flush(Registry& registry);

    // 積まれたコマンドが1つも無いか。
    [[nodiscard]] bool empty() const;

private:
    std::vector<Entity> destroy_;
    std::vector<std::function<void(Registry&)>> deferred_;
};

}  // namespace sq::ecs
