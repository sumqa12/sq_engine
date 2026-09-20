#pragma once

#include <algorithm>
#include <memory>
#include <ranges>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

#include "sq/ecs/archetype.hpp"
#include "sq/ecs/entity.hpp"
#include "sq/ecs/view.hpp"

namespace sq::ecs {

// すべてのエンティティ、アーキタイプ、およびコンポーネントデータを管理します。
// コンポーネントの追加・削除に伴い、エンティティはアーキタイプ間を遷移します（アーキタイプ／SoA設計）。
class Registry {
public:
    Registry();

    Entity create();
    void destroy(Entity entity);

    [[nodiscard]] bool is_alive(Entity entity) const;

    template <typename T>
    T& add(const Entity entity, T component) {
        const EntityRecord& record = record_of(entity);
        Archetype& old_arch = *record.archetype;
        const std::size_t old_index = record.index;

        Signature new_sig = old_arch.signature();
        new_sig.push_back(std::type_index(typeid(T)));
        new_sig = make_signature(std::move(new_sig));

        Archetype& new_arch = get_or_create_with_added<T>(new_sig, old_arch);
        const std::size_t new_index = new_arch.push_entity(entity);

        for (const ComponentTypeId& id : old_arch.signature()) {
            old_arch.storage_ptr(id)->move_element_to(old_index, *new_arch.storage_ptr(id));
        }
        T& stored = new_arch.storage<T>().emplace(std::move(component));

        finish_transition(entity, old_arch, old_index, new_arch, new_index);
        return stored;
    }

    template <typename T>
    void remove(const Entity entity) {
        const EntityRecord& record = record_of(entity);
        Archetype& old_arch = *record.archetype;
        const std::size_t old_index = record.index;
        const auto removed_id = std::type_index(typeid(T));

        Signature new_sig = old_arch.signature();
        new_sig.erase(std::ranges::remove(new_sig, removed_id).begin(), new_sig.end());

        Archetype& new_arch = get_or_create_subset(new_sig, old_arch);
        const std::size_t new_index = new_arch.push_entity(entity);

        for (const ComponentTypeId& id : new_sig) {
            old_arch.storage_ptr(id)->move_element_to(old_index, *new_arch.storage_ptr(id));
        }
        old_arch.storage_ptr(removed_id)->swap_remove(old_index);

        finish_transition(entity, old_arch, old_index, new_arch, new_index);
    }

    template <typename T>
    [[nodiscard]] bool has(const Entity entity) const {
        const EntityRecord& record = record_of(entity);
        return record.archetype->has_component(std::type_index(typeid(T)));
    }

    template <typename T>
    T& get(const Entity entity) const {
        EntityRecord record = record_of(entity);
        return record.archetype->storage<T>().get(record.index);
    }

    template <typename... Components>
    [[nodiscard]] View<Components...> view() const {
        std::vector<Archetype*> matches;
        for (auto& archetype : archetypes_ | std::views::values) {
            if ((archetype->has_component(std::type_index(typeid(Components))) && ...)) {
                matches.push_back(archetype.get());
            }
        }
        return View<Components...>(std::move(matches));
    }



    // -- デバッグ用 --------------------------------------------------------

    // entity が持つコンポーネント型の一覧を返す（所有しないビュー）。
    //
    // 実体はそのエンティティが属するアーキタイプの Signature そのものなので、コピーは起きない。
    //
    // ★ 返るのは「型の一覧」であって**値ではない**。コンポーネントは型消去して保持されており、
    //   値まで一様に取り出す手段は無い（取り出すには get<T>() のように型を指定するしかない）。
    //   デバッグで知りたいのはたいてい「何が付いているか／付け忘れていないか」なので、
    //   型の一覧で足りる。値が見たい型は get<T>() で個別に引くこと。
    //
    // ★ 無効／破棄済みのハンドルでは **throw せず空を返す**。get() や has() と違い、
    //   「壊れたハンドルを調べる」こと自体がこの関数の用途に含まれるため。
    //   生死そのものは is_alive() で判定すること。
    //
    // ★ 返った参照が有効なのは、そのエンティティが add/remove でアーキタイプを移るまで。
    //   移った後は別の Signature を指すべきなので、持ち回らずその場で使うこと。
    //
    // 出力例（std::type_index::name() は MSVC なら "struct sq::scene::Transform" のように読める）:
    //   for (const ComponentTypeId& id : registry.components_of(e)) {
    //       spdlog::info("  {}", id.name());
    //   }
    [[nodiscard]] const Signature& components_of(Entity entity) const;

    // 生存しているエンティティの総数。
    // ★ 破棄済みで ID が再利用待ちのスロットは数えない。
    // 指定コンポーネントを持つエンティティの総数
    template <typename... Components>
    [[nodiscard]] std::size_t entity_count() {
        if (sizeof...(Components) == 0) {
            //   create() は「free_ids_ から再利用する」か「records_ を1つ伸ばす」かのどちらかで、
            //   destroy() は必ず free_ids_ へ id を戻す。したがって
            //     records_ の総数 － 解放済みID数 ＝ 生存数
            //   が常に成り立つ。O(1) で求まる。
            //
            //   ★ この不変条件を疑う場面では、records_ を走査して alive を数える実装
            //     （O(n)）に差し替えてもよい。デバッグ用途なら十分速いし、
            //     両者の値が食い違えば、それ自体が create/destroy 側のバグを示す手がかりになる。
            return records_.size() - free_ids_.size();
        }

        std::size_t result = 0;
        for (auto& archetype : archetypes_ | std::views::values) {
            if ((archetype->has_component(std::type_index(typeid(Components))) && ...)) {
                result++;
            }
        }
        return result;
    }

private:
    struct EntityRecord {
        Archetype* archetype = nullptr;
        std::size_t index = 0;
        Entity::Generation generation = 0;
        bool alive = false;
    };

    [[nodiscard]] EntityRecord& record_of(Entity entity);
    [[nodiscard]] const EntityRecord& record_of(Entity entity) const;

    // 行がアーキタイプ間で移動された後、移動されたエンティティの管理情報を更新し、
    // `entity` のレコードを新しい保存先を指すように更新します。
    void finish_transition(Entity entity,
        Archetype& old_arch, std::size_t old_index,
        Archetype& new_arch, std::size_t new_index
    );

    Archetype& get_or_create_subset(const Signature& signature, Archetype& source);

    template <typename T>
    Archetype& get_or_create_with_added(const Signature& signature, Archetype& source) {
        if (const auto it = archetypes_.find(signature); it != archetypes_.end()) {
            return *it->second;
        }
        auto archetype = std::make_unique<Archetype>(signature);
        for (const ComponentTypeId& id : source.signature()) {
            archetype->register_storage(id, source.storage_ptr(id)->create_empty());
        }
        archetype->register_storage(std::type_index(typeid(T)), std::make_unique<ComponentStorage<T>>());
        Archetype* ptr = archetype.get();
        archetypes_.emplace(signature, std::move(archetype));
        return *ptr;
    }

    std::vector<EntityRecord> records_;
    std::vector<Entity::Id> free_ids_;
    std::unordered_map<Signature, std::unique_ptr<Archetype>, SignatureHash> archetypes_;
    Archetype* empty_archetype_ = nullptr;
};

}  // namespace sq::ecs
