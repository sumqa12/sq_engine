#include "sq/ecs/archetype.hpp"

#include <algorithm>
#include <ranges>

namespace sq::ecs {

// コンポーネントのタイプ ID のリストから、ソートされ、重複が除去された署名を作成します。
Signature make_signature(std::vector<ComponentTypeId> ids) {
    std::ranges::sort(ids);
    ids.erase(std::ranges::unique(ids).begin(), ids.end());
    return ids;
}

//  Archetype は、コンポーネントのタイプ ID のソートされたリストを署名として保持します。
Archetype::Archetype(Signature signature) : signature_(std::move(signature)) {}

// `id` がこのアーキタイプの署名に含まれているかどうかを返します。
bool Archetype::has_component(const ComponentTypeId id) const {
    return std::ranges::binary_search(signature_, id);
}

// `id` に対応するコンポーネントストレージを登録します。すでに登録されている場合は、上書きされます。
void Archetype::register_storage(ComponentTypeId id, std::unique_ptr<IComponentStorage> storage) {
    storages_.emplace(id, std::move(storage));
}

// `entity` を新しい行として追加します。
// コンポーネントのデータは、シグネチャに含まれる各コンポーネント型について、storage<T>().emplace(...) を使用して個別に設定する必要があります。
std::size_t Archetype::push_entity(const Entity entity) {
    entities_.push_back(entity);
    return entities_.size() - 1;
}

// `index` の行を削除します（entities_ およびすべてのストレージを横断してスワップ・アンド・ポップを行います）。
// スワップ後に `index` を占めるようになったエンティティを返します。
// 削除された行が最後の行だった場合は、null の Entity を返します（これにより、呼び出し元はインデックスマップを更新できます）。
// エンティティが完全に破棄され、そのコンポーネントデータが破棄される場合に使用されます。
Entity Archetype::remove_entity_at(const std::size_t index) {
    assert(index < entities_.size());
    for (const auto& storage : storages_ | std::views::values) {
        storage->swap_remove(index);
    }

    if (const std::size_t last = entities_.size() - 1; index != last) {
        entities_[index] = entities_[last];
    }
    entities_.pop_back();

    if (index < entities_.size()) {
        return entities_[index];
    }
    return Entity{};
}

// コンポーネントのストレージには一切触れることなく、entities_ に対してのみスワップ・アンド・ポップを行います。
// これは、アーキタイプ遷移（コンポーネントの追加／削除）の際に使用され、その際、コンポーネントのデータは事前にストレージ単位で再配置済みとなっています。
// スワップ後に `index` を占めるようになったエンティティを返します。
// 削除された行が最後尾だった場合は、null の Entity を返します。
Entity Archetype::move_out_entity_at(const std::size_t index) {
    assert(index < entities_.size());
    if (const std::size_t last = entities_.size() - 1; index != last) {
        entities_[index] = entities_[last];
    }
    entities_.pop_back();

    if (index < entities_.size()) {
        return entities_[index];
    }
    return Entity{};
}

// `id` に対応するコンポーネントストレージへのポインタを返します。
// そのようなストレージが存在しない場合は nullptr を返します。
IComponentStorage* Archetype::storage_ptr(const ComponentTypeId id) {
    const auto it = storages_.find(id);
    return it != storages_.end() ? it->second.get() : nullptr;
}

}  // namespace sq::ecs
