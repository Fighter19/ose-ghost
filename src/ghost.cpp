// The ghost: stays in its favourite room, wanders around in it, now and then roams further
// and comes back, and throws physics props that are near it. Its traces show on EMF readers.
#include <algorithm>
#include <cmath>
#include <format>

#include "core/cvar.h"
#include "core/log.h"
#include "game/components.h"
#include "ghosthunt.h"
#include "physics/rigid.h"

namespace ose::ghosthunt {

namespace {

CVar gh_ghost_model("gh_ghost_model", "models/player/t_guerilla.mdl", CVar::None, "Model the ghost is shown with");
CVar gh_ghost_visible("gh_ghost_visible", "1", CVar::None, "Show the ghost all the time (for now it is always visible by default)");
CVar gh_ghost_alpha("gh_ghost_alpha", "0.7", CVar::None, "How opaque the ghost is (0..1)");
CVar gh_ghost_room("gh_ghost_room", "", CVar::None, "Name of the room the ghost haunts (empty: chosen at random when the map starts)");
CVar gh_ghost_emf5("gh_ghost_emf5", "-1", CVar::None, "Whether the ghost leaves EMF 5 evidence: 1 yes, 0 no, -1 chosen at random");
CVar gh_ghost_speed("gh_ghost_speed", "70", CVar::None, "Walking speed of the ghost (units per second); roaming is 1.5 times faster");
CVar gh_roam_chance("gh_roam_chance", "0.2", CVar::None, "Chance that the ghost leaves its room for a while when it picks where to go next");
CVar gh_throw_min("gh_throw_min", "8", CVar::None, "Shortest time between two throws (seconds)");
CVar gh_throw_max("gh_throw_max", "20", CVar::None, "Longest time between two throws (seconds)");
CVar gh_throw_range("gh_throw_range", "220", CVar::None, "How far from the ghost props can be thrown");
CVar gh_throw_speed("gh_throw_speed", "380", CVar::None, "Typical speed of a thrown prop (units per second)");
CVar gh_throw_maxmass("gh_throw_maxmass", "60", CVar::None, "Heaviest prop the ghost throws (kg)");
CVar gh_touch_min("gh_touch_min", "12", CVar::None, "Shortest time between two touches (EMF 2 where the ghost is)");
CVar gh_touch_max("gh_touch_max", "30", CVar::None, "Longest time between two touches");
CVar gh_emf_duration("gh_emf_duration", "20", CVar::None, "How long an EMF trace lasts (seconds)");
CVar gh_emf5_chance("gh_emf5_chance", "0.35", CVar::None, "For a ghost with EMF 5 evidence: chance a throw reads 5 instead of 3");
CVar gh_debug_throws("gh_debug_throws", "0", CVar::None,
                     "Debug the ghost's throws: 1 draws them (where from, direction, the prop's path) and prints each to the console");

float uniform(std::mt19937& rng, float a, float b) { return std::uniform_real_distribution<float>(a, std::max(a, b))(rng); }

// The ghost's chest, where it throws from.
Vec3 chest(const Vec3& feet) { return feet + Vec3{0, 0, 40}; }

int randomAreaIn(Hunt& h, int room) {
    if (room < 0 || size_t(room) >= h.roomAreas.size() || h.roomAreas[size_t(room)].empty()) return -1;
    const auto& list = h.roomAreas[size_t(room)];
    // Larger areas are picked more often, so every part of the room gets visited alike.
    std::vector<float> weights;
    for (int a : list) {
        const auto& ar = h.nav.areas[size_t(a)];
        weights.push_back(std::max(64.0f, (ar.se.x - ar.nw.x) * (ar.se.y - ar.nw.y)));
    }
    return list[std::discrete_distribution<size_t>(weights.begin(), weights.end())(h.rng)];
}

Vec3 randomPointIn(Hunt& h, int area) {
    const auto& a = h.nav.areas[size_t(area)];
    float mx = std::min(12.0f, (a.se.x - a.nw.x) * 0.5f), my = std::min(12.0f, (a.se.y - a.nw.y) * 0.5f);
    float x = uniform(h.rng, a.nw.x + mx, a.se.x - mx), y = uniform(h.rng, a.nw.y + my, a.se.y - my);
    return {x, y, a.floorZ(x, y)};
}

void setEvent(Ghost& g, double now, std::string text) {
    g.lastEvent = std::move(text);
    g.lastEventTime = now;
}

bool goTo(Hunt& h, Ghost& g, const Vec3& from, int area, Ghost::State state) {
    if (area < 0) return false;
    Vec3 goal = randomPointIn(h, area);
    if (!h.nav.findPath(from, goal, g.path)) return false;
    g.pathIndex = 1;
    g.goal = goal;
    g.state = state;
    return true;
}

void decide(Hunt& h, Ghost& g, const Vec3& pos, double now) {
    int here = h.nav.findArea(pos);
    bool inRoom = here >= 0 && h.areaRoom[size_t(here)] == g.room;
    bool ok = false;
    if (g.room >= 0 && !inRoom) ok = goTo(h, g, pos, randomAreaIn(h, g.room), Ghost::State::Return);
    else if (std::uniform_real_distribution<float>(0, 1)(h.rng) < gh_roam_chance.getFloat()) {
        // Somewhere else, not too far away.
        std::vector<int> near;
        h.nav.areasNear(pos, 1200, near);
        std::erase_if(near, [&](int a) { return h.areaRoom[size_t(a)] == g.room && g.room >= 0; });
        if (!near.empty()) ok = goTo(h, g, pos, near[std::uniform_int_distribution<size_t>(0, near.size() - 1)(h.rng)], Ghost::State::Roam);
    }
    if (!ok && g.room >= 0) ok = goTo(h, g, pos, randomAreaIn(h, g.room), Ghost::State::Wander);
    if (!ok && here >= 0) {  // no room: wander around where it is
        std::vector<int> near;
        h.nav.areasNear(pos, 500, near);
        if (!near.empty()) ok = goTo(h, g, pos, near[std::uniform_int_distribution<size_t>(0, near.size() - 1)(h.rng)], Ghost::State::Wander);
    }
    if (!ok) {
        g.state = Ghost::State::Idle;
        g.stateUntil = now + 1;
    }
}

}  // namespace

const char* stateName(Ghost::State s) {
    switch (s) {
        case Ghost::State::Idle: return "idle";
        case Ghost::State::Wander: return "wander";
        case Ghost::State::Roam: return "roam";
        case Ghost::State::Return: return "return";
    }
    return "?";
}

void spawnGhost(Hunt& h) {
    World& w = h.world;
    auto& reg = w.registry();
    Ghost g;
    // The favourite room: the one gh_ghost_room names, else a random room with areas to walk
    // on, preferring rooms that are not spawn areas.
    std::vector<int> candidates, fallback;
    for (size_t i = 0; i < h.rooms.size(); ++i) {
        if (h.roomAreas[i].empty()) continue;
        if (!gh_ghost_room.string().empty() && iequals(h.rooms[i].name, gh_ghost_room.string())) {
            candidates = {int(i)};
            break;
        }
        (toLower(h.rooms[i].name).find("spawn") == std::string::npos ? candidates : fallback).push_back(int(i));
    }
    if (!gh_ghost_room.string().empty() && (candidates.size() != 1 || !iequals(h.rooms[size_t(candidates[0])].name, gh_ghost_room.string())))
        logWarn("ghosthunt: no room named '{}' with nav areas, choosing one", gh_ghost_room.string());
    if (candidates.empty()) candidates = fallback;
    if (!candidates.empty()) g.room = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(h.rng)];
    g.emf5 = gh_ghost_emf5.getInt() < 0 ? std::uniform_int_distribution<int>(0, 1)(h.rng) == 1 : gh_ghost_emf5.getBool();

    Vec3 pos;
    int area = randomAreaIn(h, g.room);
    if (area < 0 && !h.nav.areas.empty()) area = std::uniform_int_distribution<int>(0, int(h.nav.areas.size()) - 1)(h.rng);
    if (area >= 0) pos = randomPointIn(h, area);
    double now = w.time();
    g.stateUntil = now + 2;
    g.nextThrow = now + uniform(h.rng, gh_throw_min.getFloat(), gh_throw_max.getFloat());
    g.nextTouch = now + uniform(h.rng, gh_touch_min.getFloat(), gh_touch_max.getFloat());
    setEvent(g, now, "appeared");

    Entity e = reg.create();
    reg.emplace<game::Transform>(e, game::Transform{pos, {0, uniform(h.rng, -180, 180), 0}});
    reg.emplace<game::ClassName>(e, game::ClassName{"gh_ghost"});
    reg.emplace<game::Name>(e, game::Name{"ghost"});
    game::StudioModel m;
    m.path = gh_ghost_model.string();
    m.renderColor = {0.7f, 0.8f, 1.0f, gh_ghost_alpha.getFloat()};  // pale and see-through
    m.visible = gh_ghost_visible.getBool();
    reg.emplace<game::StudioModel>(e, m);
    reg.emplace<game::PlayerAnimation>(e);
    reg.emplace<Ghost>(e, g);
    game::Think th;
    th.nextThink = 0;
    Hunt* hp = &h;
    th.fn = [hp](Entity self) {
        thinkGhost(*hp, hp->world.tickInterval());
        if (auto* t = hp->world.registry().get<game::Think>(self)) t->nextThink = hp->world.time() + hp->world.tickInterval() * 0.5;
    };
    reg.emplace<game::Think>(e, th);
    h.ghost = e;
    logInfo("ghosthunt: the ghost haunts {} ({}EMF 5 evidence)", g.room >= 0 ? h.rooms[size_t(g.room)].name : "no room", g.emf5 ? "" : "no ");
}

void thinkGhost(Hunt& h, float dt) {
    World& w = h.world;
    auto& reg = w.registry();
    auto* g = reg.get<Ghost>(h.ghost);
    auto* t = reg.get<game::Transform>(h.ghost);
    if (!g || !t) return;
    double now = w.time();
    h.emf.update(w);
    if (auto* m = reg.get<game::StudioModel>(h.ghost)) {
        m->visible = gh_ghost_visible.getBool();
        m->renderColor.w = std::clamp(gh_ghost_alpha.getFloat(), 0.0f, 1.0f);
    }

    // Throw trails for the debug view.
    std::erase_if(h.throws, [now](const ThrowRecord& r) { return now - r.time > 30; });
    for (auto& r : h.throws) {
        if (now - r.time > 4 || !reg.valid(r.prop)) continue;
        if (auto* pp = reg.get<game::PhysicsProp>(r.prop))
            if (auto* b = w.rigid().get(pp->body)) r.trail.push_back(b->pos);
    }

    // Movement along the path.
    Vec3 pos = t->origin;
    if (g->pathIndex < g->path.size()) {
        float speed = gh_ghost_speed.getFloat() * (g->state == Ghost::State::Roam ? 1.5f : 1.0f);
        float step = speed * dt;
        while (step > 0 && g->pathIndex < g->path.size()) {
            Vec3 target = g->path[g->pathIndex];
            Vec3 d = target - pos;
            float len = length(d);
            if (len <= step) {
                pos = target;
                step -= len;
                ++g->pathIndex;
                continue;
            }
            pos += d * (step / len);
            // Turn towards where it goes.
            if (d.x * d.x + d.y * d.y > 1) {
                float want = radToDeg(std::atan2(d.y, d.x));
                float diff = std::remainder(want - t->angles.yaw, 360.0f);
                t->angles.yaw += std::clamp(diff, -360.0f * dt, 360.0f * dt);
            }
            step = 0;
        }
        t->velocity = (pos - t->origin) * (1.0f / std::max(dt, 1e-4f));
        t->origin = pos;
        if (g->pathIndex >= g->path.size()) {
            // Arrived: stand still a moment.
            g->path.clear();
            g->pathIndex = 0;
            Ghost::State was = g->state;
            g->state = Ghost::State::Idle;
            g->stateUntil = now + uniform(h.rng, was == Ghost::State::Roam ? 3.0f : 2.0f, was == Ghost::State::Roam ? 8.0f : 6.0f);
        }
    } else {
        t->velocity = {};
        if (now >= g->stateUntil) decide(h, *g, pos, now);
    }

    // Interactions.
    if (now >= g->nextThrow) {
        bool thrown = ghostThrow(h);
        g = reg.get<Ghost>(h.ghost);
        g->nextThrow = now + (thrown ? uniform(h.rng, gh_throw_min.getFloat(), gh_throw_max.getFloat()) : 2.0f);
    }
    if (now >= g->nextTouch) {
        h.emf.add({chest(pos), {}, 2, now, now + gh_emf_duration.getFloat(), "touch"});
        setEvent(*g, now, "touched something (EMF 2)");
        g->nextTouch = now + uniform(h.rng, gh_touch_min.getFloat(), gh_touch_max.getFloat());
    }
}

bool ghostThrow(Hunt& h) {
    World& w = h.world;
    auto& reg = w.registry();
    auto* g = reg.get<Ghost>(h.ghost);
    auto* t = reg.get<game::Transform>(h.ghost);
    if (!g || !t) return false;
    Vec3 from = chest(t->origin);
    float range = gh_throw_range.getFloat();
    std::vector<Entity> carried;
    for (auto [e, c] : reg.view<game::Carrier>()) {
        (void)e;
        carried.push_back(c.prop);
    }
    std::vector<Entity> candidates;
    for (auto [e, pp] : reg.view<game::PhysicsProp>()) {
        auto* b = w.rigid().get(pp.body);
        if (!b || !b->dynamic() || pp.debris || 1.0f / b->invMass > gh_throw_maxmass.getFloat()) continue;
        if (lengthSq(b->pos - from) > range * range) continue;
        if (std::find(carried.begin(), carried.end(), e) != carried.end()) continue;
        if (w.collision().traceRay(from, b->pos, bsp::Contents::MaskSolid).fraction < 0.97f) continue;  // behind a wall
        candidates.push_back(e);
    }
    double now = w.time();
    if (candidates.empty()) {
        if (gh_debug_throws.getBool()) logInfo("ghosthunt: {:.1f} s: nothing to throw within {:.0f} units of the ghost", now, range);
        return false;
    }
    Entity prop = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(h.rng)];
    auto& pp = *reg.get<game::PhysicsProp>(prop);
    auto* b = w.rigid().get(pp.body);
    // Away from the ghost, a little to either side and upwards.
    Vec3 away = b->pos - from;
    away.z = 0;
    float yaw = length(away) > 4 ? std::atan2(away.y, away.x) : uniform(h.rng, -3.14159f, 3.14159f);
    yaw += degToRad(uniform(h.rng, -35, 35));
    float up = uniform(h.rng, 0.3f, 0.6f);
    Vec3 dir = normalize(Vec3{std::cos(yaw), std::sin(yaw), up});
    float speed = gh_throw_speed.getFloat() * uniform(h.rng, 0.75f, 1.2f);
    Vec3 start = b->pos;
    b->vel = dir * speed;
    b->angVel = Vec3{uniform(h.rng, -1, 1), uniform(h.rng, -1, 1), uniform(h.rng, -1, 1)} * 8.0f;
    w.rigid().wake(pp.body);

    int level = g->emf5 && std::uniform_real_distribution<float>(0, 1)(h.rng) < gh_emf5_chance.getFloat() ? 5 : 3;
    h.emf.add({start, prop, level, now, now + gh_emf_duration.getFloat(), "throw"});
    ThrowRecord rec;
    rec.prop = prop;
    if (auto* m = reg.get<game::StudioModel>(prop)) rec.model = std::string(fileName(m->path));
    rec.ghostPos = from;
    rec.propStart = start;
    rec.velocity = b->vel;
    rec.time = now;
    rec.emfLevel = level;
    rec.trail.push_back(start);
    h.throws.push_back(std::move(rec));
    setEvent(*g, now, std::format("threw {} (EMF {})", h.throws.back().model, level));
    if (gh_debug_throws.getBool())
        logInfo("ghosthunt: {:.1f} s: threw #{} {} ({:.1f} kg, {} candidates) from {:.0f} {:.0f} {:.0f} at {:.0f} u/s, EMF {}", now, prop.index,
                h.throws.back().model, 1.0f / b->invMass, candidates.size(), start.x, start.y, start.z, speed, level);
    return true;
}

}  // namespace ose::ghosthunt
