#include "sq/assets/model_spawner.hpp"

#include "sq/scene/hierarchy.hpp"
#include "sq/scene/material.hpp"
#include "sq/scene/mesh_handle.hpp"
#include "sq/scene/transform.hpp"

namespace sq::assets {

std::vector<ecs::Entity> spawn_model(ecs::Registry& registry,
                                     const LoadedModel& model,
                                     const ecs::Entity root_parent) {
    std::vector<ecs::Entity> entities;

    // 手順:
    //   1. model.nodes と**同じ数の Entity を先にまとめて作る**z
    //      ★ 親を Parent{ entity } で指すため、子を作る時点で親の Entity が必要になる。
    //        1ノードずつ「作って親を付ける」とノード順によっては親がまだ存在しない。
    uint32_t nodeCount = model.nodes.size();
    entities.reserve(nodeCount);
    for (int i = 0; i < nodeCount; i++) { entities.push_back(registry.create()); }   // ★ 先に全部作る

    for (int i = 0; i < nodeCount; i++) {
        //   2. 各ノードに Transform（TRS から3引数コンストラクタで）と WorldTransform を付ける
        //      ★ WorldTransform は**ここで**付ける。伝播システムの中で後付けしてはいけない（D-4）。
        ecs::Entity entity = entities[i];
        auto [parent, position, rotation, scale, primitives, transparent] = model.nodes[i];
        registry.add<scene::Transform>(entity, scene::Transform(position, rotation, scale));
        registry.add<scene::WorldTransform>(entity, scene::WorldTransform{});

        //   3. node.parent != kNoParent なら Parent{ entities[node.parent] } を付ける。
        //      ルートノードには root_parent が指定されていればそれを親にする
        //      （モデル全体をまとめて動かすための空エンティティを外から与えられるようにする）。
        if (parent != LoadedModel::Node::kNoParent) {
            registry.add<scene::Parent>(entity, scene::Parent{ entities[parent] });   // 子は実の親へ
        } else if (!root_parent.is_null()) {
            registry.add<scene::Parent>(entity, scene::Parent{ root_parent });        // ルートだけ root_parent へ
        }

        //   4. node.primitives の各要素に MeshHandle / Material を付ける。
        //      ★ primitives が2つ以上ある場合は**子エンティティに分ける**
        //        （1エンティティ = 1メッシュ = 1マテリアルの前提を保つため）。
        //        子は単位変換の Transform + WorldTransform + Parent{ そのノードの Entity } を持つ。
        //        primitives が1つだけならノード自身に付ければよい（余計なエンティティを作らない）。
        if (!primitives.empty()) {
            auto [fMeshId, fMaterialId] = primitives[0];
            registry.add<scene::MeshHandle>(entity, scene::MeshHandle{ fMeshId });
            registry.add<scene::Material>(entity, scene::Material{ fMaterialId, transparent[0] });

            for (int j = 1; j < primitives.size(); j++) {
                auto [meshId, materialId] = primitives[j];
                ecs::Entity child = registry.create();
                registry.add<scene::Transform>(child, scene::Transform{});
                registry.add<scene::WorldTransform>(child, scene::WorldTransform{});
                registry.add<scene::Parent>(child, scene::Parent{ entity });
                registry.add<scene::MeshHandle>(child, scene::MeshHandle{ meshId });
                registry.add<scene::Material>(child, scene::Material{ materialId, transparent[j] });
            }
        }
    }

    return entities;
}

}  // namespace sq::assets
