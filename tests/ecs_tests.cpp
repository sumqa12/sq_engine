#include <array>
#include <catch2/catch_test_macros.hpp>

#include "sq/ecs/entity_commands.hpp"  // phase17 ④-7
#include "sq/ecs/registry.hpp"
#include "sq/scene/hierarchy.hpp"      // Parent / destroy_hierarchy（phase17 ④-7）

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

// ---- phase17 ④-7: EntityCommands / destroy_hierarchy ----

using sq::ecs::EntityCommands;

TEST_CASE("destroy を積んだだけでは消えず、flush で消える", "[commands]") {
    // 1. e を作る → commands.destroy(e)
    Registry registry;
    Entity e = registry.create();
    EntityCommands commands;
    commands.destroy(e);
    // 2. まだ is_alive(e) であること
    REQUIRE(registry.is_alive(e));

    // 3. commands.flush(registry) → is_alive(e) でないこと、commands.empty() であること
    commands.flush(registry);
    REQUIRE_FALSE(registry.is_alive(e));
    REQUIRE(commands.empty());
}

TEST_CASE("同じエンティティを2回 destroy に積んでも落ちない", "[commands]") {
    // 1. e を作る → commands.destroy(e) を2回
    Registry registry;
    Entity e = registry.create();
    EntityCommands commands;
    commands.destroy(e);
    commands.destroy(e);

    // 2. flush が例外を投げない（REQUIRE_NOTHROW）こと、e が消えていること
    REQUIRE_NOTHROW(commands.flush(registry));
    REQUIRE_FALSE(registry.is_alive(e));
}

TEST_CASE("既に死んだエンティティを積んでも落ちない", "[commands]") {
    // 1. e を作って registry.destroy(e)（直接） → commands.destroy(e)
    Registry registry;
    Entity e = registry.create();
    EntityCommands commands;
    registry.destroy(e);
    commands.destroy(e);

    // 2. flush が例外を投げないこと
    REQUIRE_NOTHROW(commands.flush(registry));

    // 3. ★ e の id を再利用した別のエンティティを作ってから積んだ場合も、その別のエンティティは**消えない**こと
    //    （世代番号が違うので is_alive(e) は false。id だけで判定していると誤って消す）
    Entity e2 = registry.create();

    REQUIRE(e.id() == e2.id());
    REQUIRE_FALSE(e.generation() == e2.generation());

    commands.destroy(e);
    commands.flush(registry);

    REQUIRE(registry.is_alive(e2));

}

TEST_CASE("3段の階層の根を destroy_hierarchy すると3つとも消え、別の木は残る", "[commands]") {
    // 1. root ← child ← grandchild（Parent で指す）と、別の木 other_root ← other_child を作る
    Registry registry;
    Entity root = registry.create();
    Entity child = registry.create();
    Entity grandchild = registry.create();
    Entity other_root = registry.create();
    Entity other_child = registry.create();
    registry.add<sq::scene::Parent>(child, sq::scene::Parent(root));
    registry.add<sq::scene::Parent>(grandchild, sq::scene::Parent(child));
    registry.add<sq::scene::Parent>(other_child, sq::scene::Parent(other_root));

    // 2. sq::scene::destroy_hierarchy(registry, commands, root) → flush
    EntityCommands commands;
    sq::scene::destroy_hierarchy(registry, commands, root);
    commands.flush(registry);

    // 3. root / child / grandchild が消え、other_root / other_child が残ること
    bool require_false =  registry.is_alive(root) || registry.is_alive(child) || registry.is_alive(grandchild);
    bool require = registry.is_alive(other_root) && registry.is_alive(other_child);
    REQUIRE_FALSE(require_false);
    REQUIRE(require);
}

TEST_CASE("defer の中で create したエンティティが flush の後に存在する", "[commands]") {
    // 1. Entity created; を外に用意し、commands.defer([&](Registry& r) { created = r.create(); r.add<Position>(created, {}); })
    Entity created;
    EntityCommands commands;
    commands.defer([&](Registry& r) {
        created = r.create();
        r.add<Position>(created, {});
    });

    // 2. flush の前は created が null であること
    REQUIRE(created.is_null());

    // 3. flush の後は is_alive(created) かつ has<Position>(created) であること
    Registry registry;
    commands.flush(registry);
    bool require = registry.is_alive(created) && registry.has<Position>(created);
    REQUIRE(require);
}

TEST_CASE("defer と destroy を同じ flush に積むと defer が先に走る", "[commands]") {
    // 1. e を作る → commands.destroy(e) → commands.defer([&](Registry& r) { r.add<Tag>(e, {}); })
    //    （積んだ順は destroy が先）
    Registry registry;
    Entity e = registry.create();
    EntityCommands commands;
    commands.destroy(e);

    commands.defer([&](Registry& r) {
        r.add<Tag>(e, {});
    });

    // 2. flush が例外を投げないこと（defer が先に走るので、まだ生きている e に add できる）
    REQUIRE_NOTHROW(commands.flush(registry));

    // 3. flush の後、e が消えていること
    REQUIRE_FALSE(registry.is_alive(e));
}

TEST_CASE("each の中から積んだ destroy が反復を壊さない", "[commands]") {
    // 1. Position を持つエンティティを 5 体作る
    Registry registry;
    std::array<Entity, 5> entities{};
    for (auto& entity : entities) {
        entity = registry.create();
        registry.add<Position>(entity, {});
    }

    // 2. registry.view<Position>().each(...) の中で、全員を commands.destroy に積みつつ、訪問数を数える
    EntityCommands commands;
    std::size_t cnt = 0;
    registry.view<Position>().each([&](const Entity& e, ...) {
        commands.destroy(e);
        cnt++;
    });

    // 3. 訪問数が 5 であること（反復中には消えないので、全員を1回ずつ訪問できる）
    REQUIRE(cnt == entities.size());

    // 4. flush の後、5 体とも消えていること
    commands.flush(registry);
    for (auto& entity : entities) {
        REQUIRE_FALSE(registry.is_alive(entity));
    }
}

TEST_CASE("古いハンドルと、id を再利用した生きているエンティティを同時に積むと、生きている方が消える", "[commands]") {
    // e を destroy → e2 = create（同じ id・別の世代）→ commands.destroy(e); commands.destroy(e2);
    Registry registry;
    Entity e = registry.create();
    registry.destroy(e);

    EntityCommands commands;
    Entity e2 = registry.create();
    commands.destroy(e);
    commands.destroy(e2);
    commands.flush(registry);
    REQUIRE_FALSE(registry.is_alive(e2));
}
