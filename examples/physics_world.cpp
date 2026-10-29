#include <shark/physics.hpp>

#include <iostream>
#include <utility>

int main() {
    auto created = shark::PhysicsWorld::create();
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto world = std::move(created).value();
    for (int tick = 0; tick < 3; ++tick) {
        const auto stepped = world->step();
        if (!stepped) {
            std::cerr << stepped.error().message << '\n';
            return 1;
        }
    }
    const auto stats = world->stats();
    if (!stats || stats.value().completed_ticks != 3 || stats.value().bodies != 0 ||
        stats.value().closed || stats.value().faulted)
        return 1;
    const auto closed = world->close();
    if (!closed) {
        std::cerr << closed.error().message << '\n';
        return 1;
    }
    return 0;
}
