#pragma once

#include <cstdint>
#include <functional>

namespace sq::ecs {

class Entity {
public:
    using Id = std::uint32_t;
    using Generation = std::uint32_t;

    Entity() = default;
    Entity(const Id id, const Generation generation) : id_(id), generation_(generation) {}

    [[nodiscard]] Id id() const { return id_; }
    [[nodiscard]] Generation generation() const { return generation_; }
    [[nodiscard]] bool is_null() const { return id_ == kNullId; }

    friend bool operator==(const Entity& a, const Entity& b) {
        return a.id_ == b.id_ && a.generation_ == b.generation_;
    }
    friend bool operator!=(const Entity& a, const Entity& b) { return !(a == b); }

    static constexpr Id kNullId = static_cast<Id>(-1);

private:
    Id id_ = kNullId;
    Generation generation_ = 0;
};

}  // namespace sq::ecs

template <>
struct std::hash<sq::ecs::Entity> {
    std::size_t operator()(const sq::ecs::Entity& e) const noexcept {
        return std::hash<std::uint64_t>{}(
            (static_cast<std::uint64_t>(e.id()) << 32) | e.generation());
    }
};
