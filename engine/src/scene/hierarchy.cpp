#include "sq/scene/hierarchy.hpp"

#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <concepts>

#include "sq/ecs/entity_commands.hpp"
#include "sq/ecs/registry.hpp"

namespace sq::scene {

namespace {
    // ルートを含めて幅優先探索をする
    template <typename OnVisit>
    requires std::invocable<OnVisit, const ecs::Entity&>
    void for_each_descendant(const std::unordered_map<ecs::Entity, std::vector<ecs::Entity>>& graph, const ecs::Entity& root, const OnVisit& on_visit) {
        // 次に訪問するノードを管理するキュー
        std::queue<ecs::Entity> q;

        // 訪問済みノードを記録するセット
        std::unordered_set<ecs::Entity> visited;

        // ルートノードを初期化
        q.push(root);
        visited.insert(root);

        // キューが空になるまで探索を続ける
        while (!q.empty()) {
            // キューの先頭から現在のノードを取り出す
            ecs::Entity current = q.front();
            q.pop();

            // 訪問時の処理を実行
            on_visit(current);

            // マップ内に子ノード（隣接リスト）が存在するか確認
            if (auto it = graph.find(current); it != graph.end()) {
                // 現在のノードの子（隣接要素）をループで回す
                for (const ecs::Entity& child : it->second) {
                    // まだ訪問していない子ノードであればキューに追加
                    if (!visited.contains(child)) {
                        visited.insert(child);
                        q.push(child);
                    }
                }
            }
        }
    }
}


void destroy_hierarchy(const ecs::Registry& registry, ecs::EntityCommands& commands, ecs::Entity root) {
    if (!registry.is_alive(root)) { return; }

    // 子の表を作る
    std::unordered_map<ecs::Entity, std::vector<ecs::Entity>> children;
    registry.view<Parent>().each([&](ecs::Entity e, Parent& p) {
        children[p.entity].push_back(e);
    });

    // root から幅優先でたどり、1体ずつ commands.destroy(e)（root 自身も含める）
    for_each_descendant(children, root, [&](const ecs::Entity& e) {
        commands.destroy(e);
    });
}
}  // namespace sq::scene
