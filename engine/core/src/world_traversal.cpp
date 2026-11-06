#include "shark/world_traversal.hpp"

#include "physics_runtime_access.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

namespace shark {
namespace {
struct Failure {
    Error error;
};
[[noreturn]] void fail(const char* message) {
    throw Failure{Error{ErrorCode::conflict, message, {}}};
}
template <class T> T checked(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void checked(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <class T, class F> Result<T> boundary(const char* operation, F&& function) {
    try {
        if constexpr (std::is_void_v<T>) {
            function();
            return {};
        } else
            return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed, "World traversal operation failed", {operation}};
    }
}
struct PinGuard {
    WorldCellResidency& owner;
    WorldCellPin pin;
    bool retained{false};
    ~PinGuard() {
        if (!retained && !owner.unpin(pin))
            std::terminate();
    }
};
std::uint64_t fresh_instance() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max())
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    fail("World traversal identity exhausted");
}
} // namespace

struct WorldTraversal::Impl {
    struct Active {
        WorldTraversalCell info;
        WorldCellPin pin;
        WorldNavigationTile tile;
    };
    WorldCellResidency& residency;
    PhysicsWorld& physics;
    Scene& scene;
    WorldLocalFrame frame;
    const std::thread::id owner{std::this_thread::get_id()};
    const std::uint64_t instance{fresh_instance()};
    std::array<std::optional<Active>, 8> active{};
    std::unique_ptr<WorldNavigation> navigation;
    bool closed{false};

    Impl(WorldCellResidency& r, PhysicsWorld& p, Scene& s, WorldLocalFrame f,
         std::unique_ptr<WorldNavigation> nav)
        : residency(r), physics(p), scene(s), frame(f), navigation(std::move(nav)) {}
    void check_owner() const {
        if (owner != std::this_thread::get_id())
            fail("World traversal requires its creating thread");
    }
    void check_ready() const {
        check_owner();
        if (closed)
            fail("World traversal owner is closed");
        static_cast<void>(checked(scene_detail::PhysicsRuntimeAccess::lease(physics, scene)));
        for (const auto& entry : active)
            if (entry) {
                const auto state = checked(physics.body_state(scene, entry->info.collision));
                if (state.geometry != PhysicsBodyGeometry::static_mesh)
                    fail("World cell lost its checked collision attachment");
            }
    }
    std::unique_ptr<WorldNavigation> candidate(const WorldNavigationTile* add,
                                               const WorldCellKey* remove) const {
        std::vector<WorldNavigationTile> tiles;
        tiles.reserve(8);
        for (const auto& entry : active)
            if (entry && (!remove || entry->info.key != *remove))
                tiles.push_back(entry->tile);
        if (add)
            tiles.push_back(*add);
        return checked(WorldNavigation::create(frame, tiles));
    }
};

WorldTraversal::WorldTraversal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WorldTraversal::~WorldTraversal() {
    if (!close())
        std::terminate();
}
std::uint64_t WorldTraversal::instance() const noexcept {
    return impl_->instance;
}
WorldLocalFrame WorldTraversal::frame() const {
    return impl_->frame;
}

Result<void> WorldTraversal::check_context(const Scene& scene, const PhysicsWorld& physics) const {
    return boundary<void>("world.traversal.context", [&] {
        impl_->check_owner();
        if (&scene != &impl_->scene || &physics != &impl_->physics)
            fail("Traversal requires its exact Scene and PhysicsWorld owners");
        impl_->check_ready();
    });
}

Result<bool> WorldTraversal::allows_motion(WorldPosition from, WorldPosition to,
                                           double horizontal_radius) const {
    return boundary<bool>("world.traversal.motion", [&] {
        impl_->check_ready();
        if (!std::isfinite(horizontal_radius) || horizontal_radius < 0 ||
            horizontal_radius > physics_maximum_dimension)
            fail("Traversal radius is outside the bounded profile");
        // Validate both worlds/bounds before deriving any cell inventory.
        static_cast<void>(checked(
            world_position_relative_to(from, impl_->frame.origin, physics_local_position_limit)));
        static_cast<void>(checked(
            world_position_relative_to(to, impl_->frame.origin, physics_local_position_limit)));
        const auto a0 =
            checked(translate_world_position(from, {-horizontal_radius, 0, -horizontal_radius}));
        const auto a1 =
            checked(translate_world_position(from, {horizontal_radius, 0, horizontal_radius}));
        const auto b0 =
            checked(translate_world_position(to, {-horizontal_radius, 0, -horizontal_radius}));
        const auto b1 =
            checked(translate_world_position(to, {horizontal_radius, 0, horizontal_radius}));
        const auto min_x = std::min(a0.cell.x, b0.cell.x), max_x = std::max(a1.cell.x, b1.cell.x);
        const auto min_z = std::min(a0.cell.z, b0.cell.z), max_z = std::max(a1.cell.z, b1.cell.z);
        const auto min_y = std::min(from.cell.y, to.cell.y),
                   max_y = std::max(from.cell.y, to.cell.y);
        if (max_x - min_x >= 8 || max_y - min_y >= 8 || max_z - min_z >= 8 ||
            (max_x - min_x + 1) * (max_y - min_y + 1) * (max_z - min_z + 1) > 8)
            return false;
        for (auto x = min_x; x <= max_x; ++x)
            for (auto y = min_y; y <= max_y; ++y)
                for (auto z = min_z; z <= max_z; ++z) {
                    const WorldCellKey key{from.world, {x, y, z}};
                    if (std::none_of(
                            impl_->active.begin(), impl_->active.end(),
                            [&](const auto& entry) { return entry && entry->info.key == key; }))
                        return false;
                }
        return true;
    });
}

Result<std::unique_ptr<WorldTraversal>> WorldTraversal::create(WorldCellResidency& residency,
                                                               PhysicsWorld& physics, Scene& scene,
                                                               WorldLocalFrame frame) {
    return boundary<std::unique_ptr<WorldTraversal>>("world.traversal.create", [&] {
        static_cast<void>(checked(scene_detail::PhysicsRuntimeAccess::lease(physics, scene)));
        SceneQueryOptions options;
        options.fields = {false, false, false};
        options.page_size = 1;
        options.scan_budget = 1;
        const auto page = checked(scene.query_entities(options));
        const auto* scope = std::get_if<RegionSceneScope>(&page.scope());
        if (!scope || scope->world != frame.origin.world)
            fail("World traversal requires a matching leased region Scene");
        auto nav = checked(WorldNavigation::create(frame, {}));
        return std::unique_ptr<WorldTraversal>{new WorldTraversal{
            std::make_unique<Impl>(residency, physics, scene, frame, std::move(nav))}};
    });
}

Result<WorldTraversalCell> WorldTraversal::activate(WorldCellKey key,
                                                    std::uint64_t expected_generation,
                                                    WorldEntityId proxy,
                                                    PhysicsStaticMeshSettings settings) {
    return boundary<WorldTraversalCell>("world.traversal.activate", [&] {
        impl_->check_ready();
        if (key.world != impl_->frame.origin.world)
            fail("World cell belongs to a different world");
        for (const auto& entry : impl_->active)
            if (entry && entry->info.key == key)
                fail("World cell is already traversal-ready");
        auto free = std::find_if(impl_->active.begin(), impl_->active.end(),
                                 [](const auto& entry) { return !entry; });
        if (free == impl_->active.end())
            fail("World traversal cell capacity exhausted");
        PinGuard pin{impl_->residency, checked(impl_->residency.pin(key, expected_generation))};
        const auto* content = checked(impl_->residency.resident(key));
        if (!content->geometry())
            fail("World cell has no prepared collision/navigation content");
        WorldNavigationTile tile{key, expected_generation, *content->geometry()};
        auto navigation = impl_->candidate(&tile, nullptr);
        PhysicsStaticMesh mesh;
        mesh.triangles = tile.geometry.collision_triangles;
        mesh.vertices.reserve(tile.geometry.vertices.size());
        for (const auto& vertex : tile.geometry.vertices) {
            const auto global = checked(world_position_from_local(
                content->origin(), {vertex.x, vertex.y, vertex.z}, world_cell_size_meters));
            const auto local = checked(world_position_relative_to(global, impl_->frame.origin,
                                                                  physics_local_position_limit));
            mesh.vertices.emplace_back(local[0], local[1], local[2]);
        }
        const auto body = checked(impl_->physics.create_static_mesh_body(
            impl_->scene, proxy, Name{"World cell collision"}, mesh, settings));
        const WorldTraversalCell result{key, expected_generation, body, proxy};
        // Everything after native creation is a nonallocating publication.
        free->emplace(Impl::Active{result, pin.pin, std::move(tile)});
        impl_->navigation = std::move(navigation);
        pin.retained = true;
        return result;
    });
}

Result<void> WorldTraversal::deactivate(WorldCellKey key, std::uint64_t expected_generation) {
    return boundary<void>("world.traversal.deactivate", [&] {
        impl_->check_ready();
        auto found =
            std::find_if(impl_->active.begin(), impl_->active.end(),
                         [&](const auto& entry) { return entry && entry->info.key == key; });
        if (found == impl_->active.end() ||
            (*found)->info.resident_generation != expected_generation)
            fail("World cell traversal generation is stale or absent");
        auto navigation = impl_->candidate(nullptr, &key);
        checked(impl_->physics.remove_body(impl_->scene, (*found)->info.collision));
        checked(impl_->residency.unpin((*found)->pin));
        found->reset();
        impl_->navigation = std::move(navigation);
    });
}

Result<std::vector<WorldTraversalCell>> WorldTraversal::cells() const {
    return boundary<std::vector<WorldTraversalCell>>("world.traversal.cells", [&] {
        impl_->check_ready();
        std::vector<WorldTraversalCell> result;
        result.reserve(8);
        for (const auto& entry : impl_->active)
            if (entry)
                result.push_back(entry->info);
        return result;
    });
}
Result<WorldNavigationPath> WorldTraversal::path(WorldPosition from, WorldPosition to,
                                                 std::size_t maximum_expansions,
                                                 std::size_t maximum_waypoints) const {
    return boundary<WorldNavigationPath>("world.traversal.path", [&] {
        impl_->check_ready();
        return checked(impl_->navigation->path(from, to, maximum_expansions, maximum_waypoints));
    });
}
Result<WorldNavigationCursor> WorldTraversal::begin(const WorldNavigationPath& path) const {
    return boundary<WorldNavigationCursor>("world.traversal.begin", [&] {
        impl_->check_ready();
        return checked(impl_->navigation->begin(path));
    });
}
Result<WorldNavigationCursor> WorldTraversal::advance(const WorldNavigationPath& path,
                                                      const WorldNavigationCursor& cursor,
                                                      double maximum_distance) const {
    return boundary<WorldNavigationCursor>("world.traversal.advance", [&] {
        impl_->check_ready();
        if (std::isfinite(maximum_distance) && maximum_distance >= 0 &&
            cursor.next_waypoint < path.waypoints().size()) {
            const auto delta =
                checked(world_position_relative_to(path.waypoints()[cursor.next_waypoint],
                                                   cursor.position, world_local_coordinate_limit));
            const auto distance =
                std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
            // One native steering step targets only its current straight
            // segment. A chord across later portals could leave the corridor.
            maximum_distance = std::min(maximum_distance, distance);
        }
        return checked(impl_->navigation->advance(path, cursor, maximum_distance));
    });
}
Result<void> WorldTraversal::close() {
    return boundary<void>("world.traversal.close", [&] {
        impl_->check_owner();
        if (impl_->closed)
            return;
        const auto native = checked(impl_->physics.stats());
        // On an already closed native world no native owner remains. Proxy
        // Scene state from a fault must be discarded/reconciled by the host.
        for (auto& entry : impl_->active)
            if (entry) {
                if (!native.closed)
                    checked(impl_->physics.remove_body(impl_->scene, entry->info.collision));
                checked(impl_->residency.unpin(entry->pin));
                entry.reset();
            }
        impl_->navigation.reset();
        impl_->closed = true;
    });
}
} // namespace shark
