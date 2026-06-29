#include "sq/graphics/renderer.hpp"

int main() {
    auto renderer = sq::graphics::Renderer(800, 600, "sq_engine sandbox_graphics");
    renderer.run();
    return 0;
}
