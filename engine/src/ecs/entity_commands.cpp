#include "sq/ecs/entity_commands.hpp"

#include <algorithm>
#include <utility>
#include <tuple>

namespace sq::ecs {

void EntityCommands::destroy(Entity entity) {
    // destroy_ に積む（この時点では何も消さない）。
    destroy_.emplace_back(entity);
}

void EntityCommands::defer(std::function<void(Registry&)> fn) {
    // deferred_ に積む。
    deferred_.emplace_back(std::move(fn));
}

void EntityCommands::flush(Registry& registry) {
    // 次の順で実行して空にする。
    // 1. deferred_ を積んだ順に呼ぶ
    std::vector<std::function<void(Registry&)>> deferred;
    deferred_.swap(deferred);
    for (auto& fn : deferred) {
        fn(registry);
    }
    //    ★ 呼んでいる最中に deferred_ が伸びる（defer の中で defer される）と、範囲 for のイテレータが壊れる。
    //      先に std::vector をローカルへ swap（またはムーブ）してから回すのが安全。

    // 2. destroy_ を重複除去する
    // idでソート
    std::ranges::stable_sort(destroy_, [](const Entity& a, const Entity& b) {
        return std::tuple{a.id(), a.generation()} < std::tuple{b.id(), b.generation()};
    });

    // 同じエンティティ(idも世代も同じ)を除く
    auto [first, last] = std::ranges::unique(destroy_);
    destroy_.erase(first, last);

    // 3. registry.is_alive(e) のものだけ registry.destroy(e) → destroy_ を clear
    for (auto& entity : destroy_) {
        if (registry.is_alive(entity)) {
            registry.destroy(entity);
        }
    }
    destroy_.clear();
    //    ★ 死んだハンドルは**黙って飛ばす**（D-10）。Registry::destroy は死んだハンドルで例外を投げる（record_of）。
    //      destroy_hierarchy(親) と destroy(子) が同じ tick に積まれるのは正常な使い方。
}

bool EntityCommands::empty() const {
    return destroy_.empty() && deferred_.empty();
}

}  // namespace sq::ecs
