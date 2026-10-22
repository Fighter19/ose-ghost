// The ghost hunting game as a game module: with `ghosthunt 1` (set before the map loads,
// e.g. --exec "ghosthunt 1"), every map gets a ghost, rooms and an EMF reader. Players join
// the "investigator" team, which has no weapons.
#include <fstream>
#include <unordered_map>

#include "core/cvar.h"
#include "core/log.h"
#include "game/inspect.h"
#include "game/teams.h"
#include "ghosthunt.h"

namespace ose::ghosthunt {

namespace {

CVar ghosthunt("ghosthunt", "0", CVar::None, "Play the ghost hunt on the next map: a ghost, rooms and an EMF reader (set before the map loads)");
CVar gh_rooms_source("gh_rooms_source", "auto", CVar::None,
                     "Where rooms come from: auto (rooms file, else the map's nav places, else soundscape guesses), file, nav or soundscape");
CVar gh_seed("gh_seed", "0", CVar::None, "Random seed for the ghost (0: a different hunt each time)");
CVar loadout_investigator("loadout_investigator", "", CVar::None, "Weapons investigators spawn with (none)");
CVar playermodel_investigator("playermodel_investigator", "models/player/ct_urban.mdl models/player/ct_gsg9.mdl models/player/ct_sas.mdl models/player/ct_gign.mdl",
                              CVar::None, "Models investigators are shown with");

std::unordered_map<World*, std::unique_ptr<Hunt>>& hunts() {
    static std::unordered_map<World*, std::unique_ptr<Hunt>> h;
    return h;
}
World* lastWorld = nullptr;

std::string mapName(World& w) {
    if (!w.bsp() || w.bsp()->path.empty()) return "unknown";
    return stripExtension(fileName(w.bsp()->path));
}

void startHunt(World& w) {
    if (!ghosthunt.getBool()) return;
    auto h = std::make_unique<Hunt>(w);
    h->map = mapName(w);
    h->rng.seed(gh_seed.getInt() ? uint32_t(gh_seed.getInt()) : std::random_device{}());
    h->loadNav();
    h->loadRooms(gh_rooms_source.string());
    spawnGhost(*h);
    lastWorld = &w;
    hunts()[&w] = std::move(h);
}

void endHunt(World& w) {
    hunts().erase(&w);
    if (lastWorld == &w) lastWorld = nullptr;
}

Hunt* requireHunt() {
    Hunt* h = currentHunt();
    if (!h) logWarn("no ghost hunt is running (set ghosthunt 1 before the map loads, e.g. --exec \"ghosthunt 1\")");
    return h;
}

void printRooms(Hunt& h) {
    auto* g = h.ghostState();
    for (size_t i = 0; i < h.rooms.size(); ++i) {
        const auto& r = h.rooms[i];
        logInfo("  {:2} {:20} {:3} boxes {:4} nav areas  ({:.0f} {:.0f} {:.0f}) - ({:.0f} {:.0f} {:.0f}){}", i, r.name, r.boxes.size(), h.roomAreas[i].size(),
                r.bounds.mins.x, r.bounds.mins.y, r.bounds.mins.z, r.bounds.maxs.x, r.bounds.maxs.y, r.bounds.maxs.z,
                g && g->room == int(i) ? "  <- ghost room" : "");
    }
}

void registerCommands() {
    registerCommand(
        "gh_status",
        [](const std::vector<std::string>&) {
            Hunt* h = requireHunt();
            if (!h) return;
            logInfo("ghost hunt on {}: nav {} ({} areas, {} links), {} rooms", h->map, h->nav.source, h->nav.areas.size(), h->nav.linkCount(), h->rooms.size());
            if (auto* g = h->ghostState()) {
                Vec3 p = h->ghostPos();
                logInfo("ghost at {:.0f} {:.0f} {:.0f}, {}, room {}, EMF 5 evidence: {}, last: {}", p.x, p.y, p.z, stateName(g->state),
                        g->room >= 0 ? h->rooms[size_t(g->room)].name : "none", g->emf5 ? "yes" : "no", g->lastEvent);
            }
            logInfo("{} EMF traces, {} recent throws", h->emf.sources.size(), h->throws.size());
        },
        "Ghost hunt: the map's nav mesh and rooms, where the ghost is and what it does");
    registerCommand(
        "gh_rooms",
        [](const std::vector<std::string>&) {
            if (Hunt* h = requireHunt()) printRooms(*h);
        },
        "Ghost hunt: list the rooms (name, boxes, nav areas, bounds)");
    registerCommand(
        "gh_rooms_reload",
        [](const std::vector<std::string>& a) {
            Hunt* h = requireHunt();
            if (!h) return;
            std::string keep;
            if (auto* g = h->ghostState(); g && g->room >= 0) keep = h->rooms[size_t(g->room)].name;
            h->loadRooms(a.size() > 1 ? a[1] : gh_rooms_source.string());
            if (auto* g = h->ghostState()) {
                g->room = -1;
                for (size_t i = 0; i < h->rooms.size(); ++i)
                    if (iequals(h->rooms[i].name, keep)) g->room = int(i);
                if (g->room < 0 && !h->rooms.empty()) g->room = 0;
            }
            printRooms(*h);
        },
        "Ghost hunt: load the rooms again: gh_rooms_reload [auto|file|nav|soundscape]");
    registerCommand(
        "gh_rooms_save",
        [](const std::vector<std::string>& a) {
            Hunt* h = requireHunt();
            if (!h) return;
            std::string what = a.size() > 1 ? toLower(a[1]) : "current";
            std::vector<Room> rooms;
            std::string comment;
            if (what == "current") rooms = h->rooms, comment = "Rooms of " + h->map;
            else if (what == "soundscape") rooms = guessRoomsFromSoundscapes(h->world), comment = "Rooms of " + h->map + " guessed from its soundscapes";
            else if (what == "nav") {
                Hunt tmp(h->world);
                tmp.map = h->map;
                tmp.nav = h->nav;
                tmp.loadRooms("nav");
                rooms = tmp.rooms;
                comment = "Rooms of " + h->map + " from its nav places";
            } else return logWarn("usage: gh_rooms_save [current|nav|soundscape] [file]");
            std::string file = a.size() > 2 ? a[2] : h->map + "_rooms.txt";
            std::ofstream out(file);
            out << writeRooms(rooms, comment);
            if (!out) return logWarn("cannot write {}", file);
            logInfo("wrote {} rooms to {}; to use them, put the file at games/ghosthunt/content/scripts/ghosthunt/rooms/{}.txt", rooms.size(),
                    file, h->map);
        },
        "Ghost hunt: write rooms to a file to edit: gh_rooms_save [current|nav|soundscape] [file] (default <map>_rooms.txt)");
    registerCommand(
        "gh_rooms_guess",
        [](const std::vector<std::string>&) {
            Hunt* h = requireHunt();
            if (!h) return;
            auto rooms = guessRoomsFromSoundscapes(h->world);
            for (const auto& r : rooms) {
                const auto& b = r.bounds;
                logInfo("  {:20} ({:.0f} {:.0f} {:.0f}) - ({:.0f} {:.0f} {:.0f})  from {}", r.name, b.mins.x, b.mins.y, b.mins.z, b.maxs.x, b.maxs.y, b.maxs.z,
                        r.source);
            }
            logInfo("{} rooms guessed from soundscapes; gh_rooms_reload soundscape uses them, gh_rooms_save soundscape writes them", rooms.size());
        },
        "Ghost hunt: print the rooms the map's soundscapes suggest");
    registerCommand(
        "gh_ghost_throw",
        [](const std::vector<std::string>&) {
            if (!cheatsEnabled()) return logWarn("gh_ghost_throw needs sv_cheats 1");
            if (Hunt* h = requireHunt(); h && !ghostThrow(*h)) logInfo("nothing to throw near the ghost");
        },
        "Ghost hunt: make the ghost throw a prop near it now (needs sv_cheats 1)");
    registerCommand(
        "gh_ghost_room",
        [](const std::vector<std::string>& a) {
            Hunt* h = requireHunt();
            auto* g = h ? h->ghostState() : nullptr;
            if (!g) return;
            if (a.size() < 2) return logInfo("the ghost haunts {}", g->room >= 0 ? h->rooms[size_t(g->room)].name : "no room");
            if (!cheatsEnabled()) return logWarn("changing the ghost's room needs sv_cheats 1");
            for (size_t i = 0; i < h->rooms.size(); ++i)
                if (iequals(h->rooms[i].name, a[1])) {
                    g->room = int(i);
                    g->stateUntil = 0;
                    g->path.clear();
                    g->pathIndex = 0;
                    g->state = Ghost::State::Idle;
                    return logInfo("the ghost now haunts {}", h->rooms[i].name);
                }
            logWarn("no room named {} (gh_rooms lists them)", a[1]);
        },
        "Ghost hunt: show the ghost's room, or move it to another: gh_ghost_room [name] (needs sv_cheats 1)");
}

void registerGhostHunt() {
    game::registerSpawnTeam({"investigator", "Investigators (ghost hunt)",
                             {"info_player_counterterrorist", "info_player_terrorist", "info_player_start", "info_player_deathmatch"}, 3,
                             [](World&) { return ghosthunt.getBool(); }});
    game::registerInspector<Ghost>("ghost", [](World& w, Entity, const Ghost& g, std::vector<std::string>& out) {
        Hunt* h = huntOf(w);
        out.push_back(std::string(stateName(g.state)) + ", room " + (h && g.room >= 0 ? h->rooms[size_t(g.room)].name : "none"));
        out.push_back("EMF 5 evidence: " + std::string(g.emf5 ? "yes" : "no"));
        out.push_back("path " + std::to_string(g.pathIndex) + "/" + std::to_string(g.path.size()));
        out.push_back("last: " + g.lastEvent);
    });
    registerCommands();
    registerOverlays();
    game::addWorldHooks({startHunt, endHunt});
}

const bool kRegistered = (game::addGameModule(&registerGhostHunt), game::addGameContent(OSE_GHOSTHUNT_CONTENT), true);

}  // namespace

Hunt* huntOf(World& w) {
    auto it = hunts().find(&w);
    return it == hunts().end() ? nullptr : it->second.get();
}

Hunt* currentHunt() { return lastWorld ? huntOf(*lastWorld) : nullptr; }

Ghost* Hunt::ghostState() { return world.registry().get<Ghost>(ghost); }

Vec3 Hunt::ghostPos() {
    auto* t = world.registry().get<game::Transform>(ghost);
    return t ? t->origin : Vec3{};
}

void Hunt::loadNav() {
    const fs::FileSystem* fs = world.fileSystem();
    std::string path = "maps/" + map + ".nav";
    if (auto data = fs ? fs->readFile(path) : std::nullopt) {
        auto file = nav::parseNav(*data);
        if (file) {
            nav.fromNav(*file);
            nav.source = path;
            if (world.bsp() && !world.bsp()->path.empty() && file->bspSize)
                if (auto bspData = fs->readFile(world.bsp()->path); bspData && bspData->size() != file->bspSize)
                    logWarn("ghosthunt: {} was made for another version of the map", path);
            logInfo("ghosthunt: {} areas from {}", nav.areas.size(), path);
            return;
        }
        logWarn("ghosthunt: {}: {}", path, file.error());
    }
    // No .nav: walk the map from its spawn points.
    std::vector<Vec3> seeds;
    for (auto [e, cn, t] : world.registry().view<game::ClassName, game::Transform>()) {
        (void)e;
        if (istartsWith(cn.classname, "info_player_")) seeds.push_back(t.origin);
    }
    nav.generate(world, seeds);
    logInfo("ghosthunt: no {}, generated {} areas from {} spawn points", path, nav.areas.size(), seeds.size());
}

void Hunt::loadRooms(std::string_view source) {
    std::string src = toLower(source);
    rooms.clear();
    auto fromFile = [&] {
        const fs::FileSystem* fs = world.fileSystem();
        std::string path = "scripts/ghosthunt/rooms/" + map + ".txt";
        auto data = fs ? fs->readFile(path) : std::nullopt;
        if (!data) return false;
        std::string error;
        rooms = parseRooms(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), &error);
        if (!error.empty()) logWarn("ghosthunt: {}: {}", path, error);
        for (auto& r : rooms) r.source = path;
        return !rooms.empty();
    };
    auto fromNav = [&] {
        // One room per place: its areas, from just below their floor to about a storey above.
        for (size_t p = 0; p < nav.places.size(); ++p) {
            Room r;
            r.name = nav.places[p];
            r.source = "nav place";
            for (const auto& a : nav.areas) {
                if (a.place != p + 1) continue;
                float lo = std::min({a.nw.z, a.se.z, a.neZ, a.swZ}), hi = std::max({a.nw.z, a.se.z, a.neZ, a.swZ});
                r.addBox({{a.nw.x, a.nw.y, lo - 8}, {a.se.x, a.se.y, hi + 100}});
            }
            if (!r.boxes.empty()) rooms.push_back(std::move(r));
        }
        return !rooms.empty();
    };
    auto fromSoundscapes = [&] {
        rooms = guessRoomsFromSoundscapes(world);
        return !rooms.empty();
    };
    bool ok = src == "file" ? fromFile() : src == "nav" ? fromNav() : src == "soundscape" ? fromSoundscapes() : fromFile() || fromNav() || fromSoundscapes();
    logInfo("ghosthunt: {} rooms ({})", rooms.size(), ok ? rooms.front().source : "none found");
    assignAreasToRooms();
}

void Hunt::assignAreasToRooms() {
    areaRoom.assign(nav.areas.size(), -1);
    roomAreas.assign(rooms.size(), {});
    bool byPlace = !rooms.empty() && rooms.front().source == "nav place";
    for (size_t i = 0; i < nav.areas.size(); ++i) {
        const auto& a = nav.areas[i];
        int room = -1;
        if (byPlace) {
            const std::string& place = a.place && a.place <= nav.places.size() ? nav.places[a.place - 1] : std::string();
            for (size_t r = 0; r < rooms.size() && !place.empty(); ++r)
                if (rooms[r].name == place) room = int(r);
        } else room = roomAt(rooms, a.center() + Vec3{0, 0, 16});
        areaRoom[i] = room;
        if (room >= 0) roomAreas[size_t(room)].push_back(int(i));
    }
}

}  // namespace ose::ghosthunt
