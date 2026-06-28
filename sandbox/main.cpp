#include <spdlog/spdlog.h>

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

}  // namespace

int main() {
    sq::ecs::Registry registry;

    constexpr int kEntityCount = 5;
    for (int i = 0; i < kEntityCount; ++i) {
        const sq::ecs::Entity e = registry.create();
        registry.add<Position>(e, Position{static_cast<float>(i), 0.0f});
        registry.add<Velocity>(e, Velocity{1.0f, 0.5f});
    }

    constexpr int kFrames = 3;
    for (int frame = 0; frame < kFrames; ++frame) {
        registry.view<Position, Velocity>().each(
            [](const sq::ecs::Entity, Position& pos, const Velocity& vel) {
                pos.x += vel.dx;
                pos.y += vel.dy;
            });

        spdlog::info("-- frame {} --", frame);
        registry.view<Position>().each([](const sq::ecs::Entity entity, Position& pos) {
            spdlog::info("entity({}, gen {}): pos=({:.1f}, {:.1f})", entity.id(),
                         entity.generation(), pos.x, pos.y);
        });
    }

    return 0;
}
