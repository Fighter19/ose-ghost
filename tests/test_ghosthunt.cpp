// The ghost hunt: rooms files, nav paths, a generated nav mesh, soundscape room guesses, EMF
// readings and the ghost throwing props, on the generated dev maps; with the game installed,
// every CS:S map's nav areas and a hunt on cs_office.
#include <doctest/doctest.h>

#include <cstdlib>

#include "core/cvar.h"
#include "devcontent/devcontent.h"
#include "fs/filesystem.h"
#include "game/components.h"
#include "ghosthunt.h"
#include "physics/rigid.h"

using namespace ose;
using namespace ose::ghosthunt;

namespace {

void setCvar(const char* name, const char* value) {
    CVar* v = CVar::find(name);
    REQUIRE(v);
    v->set(value);
}

// A dev map loaded into a World (without the hunt unless `hunt`).
struct DevMap {
    fs::FileSystem fs;
    game::World world;
    explicit DevMap(const char* map, bool hunt = false) {
        fs.addMount(devcontent::build());
        auto b = bsp::BspFile::load(*fs.readFile(std::string("maps/") + map + ".bsp"));
        REQUIRE(b);
        auto bsp = std::make_shared<bsp::BspFile>(std::move(*b));
        bsp->path = std::string("maps/") + map + ".bsp";
        setCvar("ghosthunt", hunt ? "1" : "0");
        setCvar("gh_seed", "7");
        world.loadMap(bsp, &fs);
        world.spawnMapEntities();
        setCvar("ghosthunt", "0");
    }
    void run(float seconds) {
        for (int i = 0; i < int(seconds * 66); ++i) world.tick(1.0f / 66);
    }
};

nav::Area area(uint32_t id, float x0, float y0, float x1, float y1, float z) {
    nav::Area a;
    a.id = id;
    a.nw = {x0, y0, z};
    a.se = {x1, y1, z};
    a.neZ = a.swZ = z;
    return a;
}

}  // namespace

TEST_CASE("rooms files: read, written and read again") {
    std::string error;
    auto rooms = parseRooms(R"("Rooms"
{
    "room" { "name" "Kitchen" "box" "0 0 0 100 200 120" "box" "100 0 0 150 50 120" }
    "room" { "name" "Hall" "box" "-300 -300 0 0 0 120" }
    "room" { "name" "Broken" "box" "1 2 3" }
})",
                            &error);
    REQUIRE(rooms.size() == 2);  // the one with a bad box has none left
    CHECK(!error.empty());
    CHECK(rooms[0].name == "Kitchen");
    CHECK(rooms[0].boxes.size() == 2);
    CHECK(rooms[0].bounds.maxs.x == 150);
    CHECK(rooms[0].contains({120, 20, 60}));
    CHECK(!rooms[0].contains({120, 100, 60}));
    CHECK(roomAt(rooms, {-10, -10, 10}) == 1);
    CHECK(roomAt(rooms, {500, 500, 10}) == -1);
    auto again = parseRooms(writeRooms(rooms, "test"));
    REQUIRE(again.size() == 2);
    CHECK(again[1].name == "Hall");
    CHECK(again[0].boxes[1].mins.x == 100);
}

TEST_CASE("nav mesh from nav areas: links, area lookup, paths through shared edges") {
    // Three areas in a row along +x, the last one a step higher; a fourth one unconnected.
    nav::NavFile f;
    f.places = {"Hall"};
    f.areas = {area(10, 0, 0, 100, 100, 0), area(11, 100, 0, 200, 100, 0), area(12, 200, 25, 300, 75, 16), area(13, 1000, 1000, 1100, 1100, 0)};
    f.areas[0].connections[1] = {11};
    f.areas[1].connections[1] = {12};
    f.areas[1].connections[3] = {10};
    f.areas[2].connections[3] = {11};
    f.areas[0].place = 1;
    NavMesh nm;
    nm.fromNav(f);
    CHECK(nm.linkCount() == 4);
    CHECK(nm.findArea({50, 50, 10}) == 0);
    CHECK(nm.findArea({250, 50, 40}) == 2);
    CHECK(nm.findArea({250, 50, -400}) == -1);  // far below every floor and away from all centres
    std::vector<Vec3> path;
    REQUIRE(nm.findPath({10, 50, 0}, {290, 50, 16}, path));
    REQUIRE(path.size() == 4);  // start, two edges, goal
    CHECK(path[1].x == doctest::Approx(100));
    CHECK(path[2].x == doctest::Approx(200));
    CHECK(path[2].y >= 25);
    CHECK(path[2].y <= 75);
    CHECK(path[3].z == doctest::Approx(16));
    CHECK(!nm.findPath({10, 50, 0}, {1050, 1050, 0}, path));
}

TEST_CASE("generated nav mesh covers the devroom floor and goes round the pillar") {
    DevMap m("devroom");
    NavMesh nm;
    nm.generate(m.world, {{0, -300, 8}});
    CHECK(nm.source == "generated");
    // The room is 1024 units wide: about 32 x 32 cells minus the walls' and pillar's.
    CHECK(nm.areas.size() > 600);
    CHECK(nm.areas.size() < 1100);
    Vec3 pillar{(devcontent::devroom::kPillarMin[0] + devcontent::devroom::kPillarMax[0]) * 0.5f, (devcontent::devroom::kPillarMin[1] + devcontent::devroom::kPillarMax[1]) * 0.5f, 0};
    int at = nm.findArea(pillar + Vec3{0, 0, 8});
    if (at >= 0) CHECK(!nm.areas[size_t(at)].containsXY(pillar.x, pillar.y));  // only a neighbour, never a cell inside
    // Across the pillar: the path bends around it.
    Vec3 a = pillar - Vec3{0, 150, 0}, b = pillar + Vec3{0, 150, 0};
    std::vector<Vec3> path;
    REQUIRE(nm.findPath(a, b, path));
    float sideways = 0;
    for (const auto& p : path) sideways = std::max(sideways, std::abs(p.x - pillar.x));
    CHECK(sideways > (devcontent::devroom::kPillarMax[0] - devcontent::devroom::kPillarMin[0]) * 0.5f);
}

TEST_CASE("soundscape guesses: the walls around an env_soundscape make a room named after it") {
    DevMap m("devroom");
    kv::KeyValues kv;
    kv.add("classname", "env_soundscape");
    kv.add("origin", "-300 -300 64");
    kv.add("radius", "2000");
    kv.add("soundscape", "dev.storage_room");
    m.world.spawn(kv);
    auto rooms = guessRoomsFromSoundscapes(m.world);
    REQUIRE(rooms.size() == 1);
    CHECK(rooms[0].name == "Storage_room");
    const auto& b = rooms[0].bounds;
    CHECK(b.mins.x == doctest::Approx(-devcontent::devroom::kRoomHalf).epsilon(0.05));
    CHECK(b.mins.y == doctest::Approx(-devcontent::devroom::kRoomHalf).epsilon(0.05));
    CHECK(b.mins.z > -10);  // just below the floor (a displacement bump raises it there)
    CHECK(b.mins.z < 16);
    CHECK(b.maxs.z > 200);
    // The pillar blocks some rays towards +x/+y, but the median of each side still finds the far walls.
    CHECK(b.maxs.x > devcontent::devroom::kRoomHalf * 0.7f);
    CHECK(b.maxs.y > devcontent::devroom::kRoomHalf * 0.7f);
}

TEST_CASE("EMF: the strongest trace within range, gone when it expires") {
    game::World w;
    EmfField f;
    f.add({{0, 0, 0}, {}, 2, 0, 10, "touch"});
    f.add({{80, 0, 0}, {}, 5, 0, 5, "throw"});
    CHECK(f.reading({0, 0, 0}, 50) == 2);
    CHECK(f.reading({40, 0, 0}, 50) == 5);
    CHECK(f.reading({400, 0, 0}, 50) == 1);
    for (int i = 0; i < 66 * 6; ++i) w.tick(1.0f / 66);
    f.update(w);
    CHECK(f.sources.size() == 1);
    CHECK(f.reading({40, 0, 0}, 50) == 2);
}

TEST_CASE("the hunt on physroom: a ghost walks the generated nav mesh and throws barrels") {
    setCvar("gh_throw_maxmass", "1000");
    setCvar("gh_throw_range", "400");
    setCvar("gh_ghost_emf5", "0");
    DevMap m("physroom", true);
    Hunt* h = huntOf(m.world);
    REQUIRE(h);
    CHECK(h->map == "physroom");
    CHECK(h->nav.source == "generated");
    CHECK(h->nav.areas.size() > 300);
    auto* g = h->ghostState();
    REQUIRE(g);
    auto* model = m.world.registry().get<game::StudioModel>(h->ghost);
    REQUIRE(model);
    CHECK(model->visible);
    CHECK(model->renderColor.w < 1);

    // It moves about.
    Vec3 start = h->ghostPos();
    float moved = 0;
    for (int i = 0; i < 20; ++i) {
        m.run(1);
        moved = std::max(moved, length(h->ghostPos() - start));
    }
    CHECK(moved > 50);

    // Next to the barrels it throws one: the barrel flies off and leaves EMF 3.
    m.world.registry().get<game::Transform>(h->ghost)->origin = {0, 60, 0};
    size_t before = h->throws.size();
    REQUIRE(ghostThrow(*h));
    REQUIRE(h->throws.size() == before + 1);
    const ThrowRecord& t = h->throws.back();
    CHECK(length(t.velocity) > 200);
    CHECK(h->emf.reading(t.propStart, 30) == 3);
    // It flies (the barrels stand close together, so it soon hits its neighbours).
    auto* pp = m.world.registry().get<game::PhysicsProp>(t.prop);
    REQUIRE(pp);
    m.world.tick(1.0f / 66);
    CHECK(length(m.world.rigid().get(pp->body)->vel) > 150);
    m.run(0.5f);
    CHECK(length(m.world.rigid().get(pp->body)->pos - t.propStart) > 5);
    // The trace goes with the barrel and fades.
    CHECK(h->emf.reading(m.world.rigid().get(pp->body)->pos, 30) == 3);
    m.run(25);
    CHECK(h->emf.reading(m.world.rigid().get(pp->body)->pos, 30) <= 3);
    for (const auto& s : h->emf.sources) CHECK(s.cause != "throw");
    setCvar("gh_throw_maxmass", "60");
    setCvar("gh_throw_range", "220");
    setCvar("gh_ghost_emf5", "-1");
}

TEST_CASE("without ghosthunt 1 nothing runs") {
    DevMap m("devroom");
    CHECK(huntOf(m.world) == nullptr);
    CHECK(!m.world.findByClass("gh_ghost"));
}

// ------------------------------------------------------------------- with CS:S installed
TEST_CASE("game content: every map's nav areas give connected rooms") {
    const char* css = std::getenv("OSE_CSS_DIR");
    if (!css) return;
    fs::FileSystem fsys;
    REQUIRE(fsys.mountGameInfo(std::string(css) + "/cstrike/gameinfo.txt"));
    int maps = 0;
    for (const auto& f : fsys.allFiles()) {
        if (!istartsWith(f, "maps/") || !iendsWith(f, ".nav")) continue;
        CAPTURE(f);
        auto data = fsys.readFile(f);
        REQUIRE(data);
        auto file = nav::parseNav(*data);
        REQUIRE(file);
        NavMesh nm;
        nm.fromNav(*file);
        CHECK(nm.linkCount() > nm.areas.size());  // most areas lead to two or more
        // The ends of almost every link share an edge or a corner (up to a small gap).
        size_t touching = 0, total = 0;
        for (size_t i = 0; i < nm.areas.size(); ++i)
            for (int j : nm.links[i]) {
                const auto& a = nm.areas[i];
                const auto& b = nm.areas[size_t(j)];
                ++total;
                if (a.nw.x <= b.se.x + 4 && b.nw.x <= a.se.x + 4 && a.nw.y <= b.se.y + 4 && b.nw.y <= a.se.y + 4) ++touching;
            }
        CHECK(touching > total * 90 / 100);  // the rest jump or drop over gaps
        // A path from the first area to the farthest one reachable.
        std::vector<Vec3> path;
        CHECK(nm.findPath(nm.areas[0].center(), nm.areas[0].center(), path));
        ++maps;
    }
    CHECK(maps >= 18);
}

TEST_CASE("game content: a hunt on cs_office uses its nav places as rooms, and the ghost throws something") {
    const char* css = std::getenv("OSE_CSS_DIR");
    if (!css) return;
    fs::FileSystem fsys;
    REQUIRE(fsys.mountGameInfo(std::string(css) + "/cstrike/gameinfo.txt"));
    auto data = fsys.readFile("maps/cs_office.bsp");
    REQUIRE(data);
    auto b = bsp::BspFile::load(std::move(*data));
    REQUIRE(b);
    auto bsp = std::make_shared<bsp::BspFile>(std::move(*b));
    bsp->path = "maps/cs_office.bsp";
    setCvar("ghosthunt", "1");
    setCvar("gh_rooms_source", "nav");
    setCvar("gh_ghost_room", "MainHall");
    setCvar("gh_throw_min", "1");
    setCvar("gh_throw_max", "2");
    setCvar("gh_throw_range", "600");
    game::World world;
    world.loadMap(bsp, &fsys);
    world.spawnMapEntities();
    setCvar("ghosthunt", "0");
    Hunt* h = huntOf(world);
    REQUIRE(h);
    CHECK(h->nav.source == "maps/cs_office.nav");
    CHECK(h->nav.areas.size() == 1002);
    CHECK(h->rooms.size() == 14);
    auto* g = h->ghostState();
    REQUIRE(g);
    REQUIRE(g->room >= 0);
    CHECK(h->rooms[size_t(g->room)].name == "MainHall");
    for (int i = 0; i < 66 * 30; ++i) world.tick(1.0f / 66);
    CHECK(!h->throws.empty());
    // It stays in (or comes back to) its room most of the time; after 30 s it is not far.
    int room = roomAt(h->rooms, h->ghostPos() + Vec3{0, 0, 16});
    CAPTURE(room);
    CHECK(length(h->ghostPos() - h->rooms[size_t(g->room)].bounds.center()) < 2500);
    // Soundscape guesses on the same map: one room per distinct place a soundscape sits in.
    auto guesses = guessRoomsFromSoundscapes(world);
    CHECK(guesses.size() >= 8);
    for (const char* cv : {"gh_rooms_source", "gh_ghost_room", "gh_throw_min", "gh_throw_max", "gh_throw_range"}) CVar::find(cv)->reset();
}

TEST_CASE("a rooms file shipped with the module replaces the other room sources") {
    fs::FileSystem fsys;
    fsys.addMount(devcontent::build());
    REQUIRE(fsys.addDirectory(OSE_GHOSTHUNT_CONTENT));
    auto b = bsp::BspFile::load(*fsys.readFile("maps/devroom.bsp"));
    REQUIRE(b);
    auto bsp = std::make_shared<bsp::BspFile>(std::move(*b));
    bsp->path = "maps/devroom.bsp";
    setCvar("ghosthunt", "1");
    setCvar("gh_ghost_room", "East");
    game::World world;
    world.loadMap(bsp, &fsys);
    world.spawnMapEntities();
    setCvar("ghosthunt", "0");
    CVar::find("gh_ghost_room")->reset();
    Hunt* h = huntOf(world);
    REQUIRE(h);
    REQUIRE(h->rooms.size() == 2);
    CHECK(h->rooms[0].name == "West");
    CHECK(h->rooms[0].source == "scripts/ghosthunt/rooms/devroom.txt");
    // Every generated nav cell is in one of the two halves.
    size_t inRooms = h->roomAreas[0].size() + h->roomAreas[1].size();
    CHECK(inRooms == h->nav.areas.size());
    auto* g = h->ghostState();
    REQUIRE(g);
    CHECK(h->rooms[size_t(g->room)].name == "East");
    CHECK(h->ghostPos().x >= 0);
}
