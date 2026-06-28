#include <catch2/catch_test_macros.hpp>

#include "sq/ecs/registry.hpp"

namespace {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Velocity {
    float dx = 0.0f;
    float dy = 0.0f;
};

struct Tag {};

}  // namespace

using sq::ecs::Entity;
using sq::ecs::Registry;

TEST_CASE("「create」は、個性的で生き生きとした存在を生み出す", "[ecs]") {
    Registry registry;
    Entity a = registry.create();
    Entity b = registry.create();

    REQUIRE(a != b);
    REQUIRE(registry.is_alive(a));
    REQUIRE(registry.is_alive(b));
}

TEST_CASE("コンポーネントのラウンドトリップを追加／取得／確認する", "[ecs]") {
    Registry registry;
    Entity e = registry.create();

    REQUIRE_FALSE(registry.has<Position>(e));
    registry.add<Position>(e, Position{1.0f, 2.0f});
    REQUIRE(registry.has<Position>(e));

    auto& [x, y] = registry.get<Position>(e);
    REQUIRE(x == 1.0f);
    REQUIRE(y == 2.0f);
}

TEST_CASE("複数のコンポーネントを追加すると、エンティティがアーキタイプ間を移動する", "[ecs]") {
    Registry registry;
    Entity e = registry.create();

    registry.add<Position>(e, Position{1.0f, 1.0f});
    registry.add<Velocity>(e, Velocity{0.5f, -0.5f});

    REQUIRE(registry.has<Position>(e));
    REQUIRE(registry.has<Velocity>(e));

    auto pos = registry.get<Position>(e);
    auto vel= registry.get<Velocity>(e);
    REQUIRE(pos.x == 1.0f);
    REQUIRE(vel.dx == 0.5f);
}

TEST_CASE("remove はコンポーネントを 1 つ削除しますが、他のコンポーネントは残します", "[ecs]") {
    Registry registry;
    const Entity e = registry.create();
    registry.add<Position>(e, Position{3.0f, 4.0f});
    registry.add<Velocity>(e, Velocity{1.0f, 1.0f});

    registry.remove<Velocity>(e);

    REQUIRE(registry.has<Position>(e));
    REQUIRE_FALSE(registry.has<Velocity>(e));
    REQUIRE(registry.get<Position>(e).x == 3.0f);
}

TEST_CASE("ある実体を破壊しても、同じアーキタイプに属する他の実体には影響を与えない", "[ecs]") {
    Registry registry;
    const Entity a = registry.create();
    const Entity b = registry.create();
    const Entity c = registry.create();

    registry.add<Position>(a, Position{1.0f, 0.0f});
    registry.add<Position>(b, Position{2.0f, 0.0f});
    registry.add<Position>(c, Position{3.0f, 0.0f});

    registry.destroy(b);

    REQUIRE(registry.is_alive(a));
    REQUIRE_FALSE(registry.is_alive(b));
    REQUIRE(registry.is_alive(c));
    REQUIRE(registry.get<Position>(a).x == 1.0f);
    REQUIRE(registry.get<Position>(c).x == 3.0f);
}

TEST_CASE("生成処理により、破棄されたエンティティのハンドルが古いものとして検出される", "[ecs]") {
    Registry registry;
    Entity e = registry.create();
    registry.destroy(e);

    REQUIRE_FALSE(registry.is_alive(e));

    Entity reused = registry.create();
    REQUIRE(reused.id() == e.id());
    REQUIRE(reused.generation() != e.generation());
    REQUIRE_FALSE(registry.is_alive(e));
    REQUIRE(registry.is_alive(reused));
}

TEST_CASE("リクエストされたすべてのコンポーネントを持つエンティティのみを反復処理して表示する", "[ecs]") {
    Registry registry;
    Entity moving = registry.create();
    Entity still = registry.create();

    registry.add<Position>(moving, Position{0.0f, 0.0f});
    registry.add<Velocity>(moving, Velocity{1.0f, 2.0f});
    registry.add<Position>(still, Position{5.0f, 5.0f});
    registry.add<Tag>(still, Tag{});

    int visited = 0;
    registry.view<Position, Velocity>().each([&](Entity entity, Position& pos, const Velocity& vel) {
        REQUIRE(entity == moving);
        pos.x += vel.dx;
        pos.y += vel.dy;
        ++visited;
    });

    REQUIRE(visited == 1);
    REQUIRE(registry.get<Position>(moving).x == 1.0f);
    REQUIRE(registry.get<Position>(moving).y == 2.0f);
}
