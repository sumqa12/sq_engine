#pragma once

#include <cassert>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

#include "sq/ecs/component_storage.hpp"
#include "sq/ecs/entity.hpp"

namespace sq::ecs {

using ComponentTypeId = std::type_index;
using Signature = std::vector<ComponentTypeId>;

// コンポーネントのタイプ ID のリストから、ソートされ、重複が除去された署名を作成します。
Signature make_signature(std::vector<ComponentTypeId> ids);

// Signature のハッシュ関数。Signature は std::unordered_map のキーとして使用されます。
struct SignatureHash {
    std::size_t operator()(const Signature& signature) const noexcept {
        std::size_t seed = signature.size();
        for (const auto& id : signature) {
            seed ^= std::hash<ComponentTypeId>{}(id) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

// 同じコンポーネント型のセットを共有するすべてのエンティティを格納し、各
// コンポーネント型は、キャッシュに優しい反復処理のために、密な並列配列（SoA）として格納されます。
class Archetype {
public:
    explicit Archetype(Signature signature); // signature は、ソートされ、重複が除去されたコンポーネント型のリストでなければなりません。

    [[nodiscard]] const Signature& signature() const { return signature_; }
    [[nodiscard]] bool has_component(ComponentTypeId id) const;
    [[nodiscard]] std::size_t size() const { return entities_.size(); }
    [[nodiscard]] const std::vector<Entity>& entities() const { return entities_; }

    template <typename T>
    ComponentStorage<T>& storage() {
        const auto it = storages_.find(std::type_index(typeid(T)));
        assert(it != storages_.end());
        return static_cast<ComponentStorage<T>&>(*it->second);
    }

    void register_storage(ComponentTypeId id, std::unique_ptr<IComponentStorage> storage);

    // `entity` を新しい行として追加します。コンポーネントのデータは、シグネチャに含まれる各コンポーネント型について、
    // storage<T>().emplace(...) を使用して個別に設定する必要があります。
    std::size_t push_entity(Entity entity);

    // `index` の行を削除します（entities_ およびすべてのストレージを横断してスワップ・アンド・ポップを行います）。
    // スワップ後に `index` を占めるようになったエンティティを返します。削除された行が最後の行だった場合は、null の Entity を返します（これにより、呼び出し元はインデックスマップを更新できます）。
    // エンティティが完全に破棄され、そのコンポーネントデータが破棄される場合に使用されます。
    Entity remove_entity_at(std::size_t index);

    // コンポーネントのストレージには一切触れることなく、entities_ に対してのみスワップ・アンド・ポップを行います。
    // これは、アーキタイプ遷移（コンポーネントの追加／削除）の際に使用され、その際、コンポーネントのデータは事前にストレージ単位で再配置済みとなっています。
    // スワップ後に `index` を占めるようになったエンティティを返します。削除された行が最後尾だった場合は、null の Entity を返します。
    Entity move_out_entity_at(std::size_t index);

    IComponentStorage* storage_ptr(ComponentTypeId id);

private:
    Signature signature_;
    std::vector<Entity> entities_;
    std::unordered_map<ComponentTypeId, std::unique_ptr<IComponentStorage>> storages_;
};

}  // namespace sq::ecs
