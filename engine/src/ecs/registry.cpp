#include "sq/ecs/registry.hpp"

#include <stdexcept>

namespace sq::ecs {

// Registry は、空の署名を持つアーキタイプを作成し、すべての新しいエンティティは最初にこのアーキタイプに配置されます。
Registry::Registry() {
    auto empty = std::make_unique<Archetype>(Signature{});
    empty_archetype_ = empty.get();
    archetypes_.emplace(Signature{}, std::move(empty));
}

// Registry::create() は、Entity::Id を再利用するために、破棄されたエンティティの ID を free_ids_ に保持します。
Entity Registry::create() {
    Entity::Id id;
    Entity::Generation generation;

    if (!free_ids_.empty()) {
        id = free_ids_.back();
        free_ids_.pop_back();
        generation = records_[id].generation;
    } else {
        id = static_cast<Entity::Id>(records_.size());
        generation = 0;
        records_.emplace_back();
    }

    const Entity entity(id, generation);
    const std::size_t index = empty_archetype_->push_entity(entity);
    records_[id] = EntityRecord{empty_archetype_, index, generation, true};
    return entity;
}

// Registry::destroy() は、アーキタイプからエンティティを削除し、EntityRecord を更新して、Entity::Generation をインクリメントし、Entity::Id を free_ids_ に戻します。
void Registry::destroy(const Entity entity) {
    EntityRecord& record = record_of(entity);
    if (const Entity displaced = record.archetype->remove_entity_at(record.index); !displaced.is_null()) {
        records_[displaced.id()].index = record.index;
    }

    record.alive = false;
    record.archetype = nullptr;
    record.generation += 1;
    free_ids_.push_back(entity.id());
}

// Registry::is_alive() は、EntityRecord が有効であり、Entity::Generation が一致するかどうかを確認します。
bool Registry::is_alive(const Entity entity) const {
    if (entity.id() >= records_.size()) {
        return false;
    }
    const EntityRecord& record = records_[entity.id()];
    return record.alive && record.generation == entity.generation();
}

// Registry::record_of() は、EntityRecord を返します。Entity が無効または古い場合は、std::runtime_error をスローします。
Registry::EntityRecord& Registry::record_of(const Entity entity) {
    if (!is_alive(entity)) {
        throw std::runtime_error("sq::ecs::Registry: stale or invalid Entity handle");
    }
    return records_[entity.id()];
}

// Registry::record_of() const は、EntityRecord を返します。Entity が無効または古い場合は、std::runtime_error をスローします。
const Registry::EntityRecord& Registry::record_of(const Entity entity) const {
    if (!is_alive(entity)) {
        throw std::runtime_error("sq::ecs::Registry: stale or invalid Entity handle");
    }
    return records_[entity.id()];
}

// Registry::finish_transition() は、アーキタイプ間でエンティティが移動された後、移動されたエンティティの管理情報を更新し、EntityRecord を新しい保存先を指すように更新します。
void Registry::finish_transition(const Entity entity, Archetype& old_arch, const std::size_t old_index,
                                  Archetype& new_arch, const std::size_t new_index) {
    if (const Entity displaced = old_arch.move_out_entity_at(old_index); !displaced.is_null()) {
        records_[displaced.id()].index = old_index;
    }

    EntityRecord& record = records_[entity.id()];
    record.archetype = &new_arch;
    record.index = new_index;
}

// Registry::get_or_create_subset() は、指定されたシグネチャを持つアーキタイプを返します。
// 存在しない場合は、新しいアーキタイプを作成し、source アーキタイプからコンポーネントストレージをコピーします。
Archetype& Registry::get_or_create_subset(const Signature& signature, Archetype& source) {
    if (const auto it = archetypes_.find(signature); it != archetypes_.end()) {
        return *it->second;
    }
    auto archetype = std::make_unique<Archetype>(signature);
    for (const ComponentTypeId& id : signature) {
        archetype->register_storage(id, source.storage_ptr(id)->create_empty());
    }
    Archetype* ptr = archetype.get();
    archetypes_.emplace(signature, std::move(archetype));
    return *ptr;
}

}  // namespace sq::ecs
