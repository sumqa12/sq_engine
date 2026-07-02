#pragma once

#include <vector>

#include "sq/ecs/archetype.hpp"
#include "sq/ecs/entity.hpp"

namespace sq::ecs {

// `Components...` をすべて持つすべてのエンティティに対する、読み書き可能な反復ビュー。
// Registry::view<Components...>(); によって生成される。構築コストが低く、単に、
// 要求されたコンポーネントのスーパーセットとなるシグネチャを持つアーキタイプのリストである。
template <typename... Components>
class View {
public:
    explicit View(std::vector<Archetype*> archetypes) : archetypes_(std::move(archetypes)) {}

    // 一致するエンティティごとに func(Entity, Components&...) を呼び出します。
    template <typename Func>
    void each(Func&& func) const {
        for (Archetype* archetype : archetypes_) {
            const std::size_t count = archetype->size();
            const std::vector<Entity>& entities = archetype->entities();
            for (std::size_t i = 0; i < count; ++i) {
                func(entities[i], archetype->storage<Components>().get(i)...);
            }
        }
    }

    // 最初に一致したエンティティを返す
    [[nodiscard]] ecs::Entity front() const {
        for (Archetype* archetype : archetypes_) {
            if (archetype->size() > 0) {
                return archetype->entities().front();
            }
        }
        return ecs::Entity{};  // 空のエンティティを返す
    }

private:
    std::vector<Archetype*> archetypes_;
};

}  // namespace sq::ecs
