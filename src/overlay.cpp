// What the hunt draws for the local player: the EMF reader on the HUD, and debug views of
// the rooms, the nav mesh, the ghost, its throws and the EMF traces.
#include <cmath>
#include <format>

#include "core/cvar.h"
#include "game/components.h"
#include "game/overlay.h"
#include "ghosthunt.h"

namespace ose::ghosthunt {

namespace {

CVar gh_hud_emf("gh_hud_emf", "1", CVar::None, "Show the EMF reader");
CVar gh_emf_range("gh_emf_range", "100", CVar::None, "How far the EMF reader picks up traces (units)");
CVar gh_debug_rooms("gh_debug_rooms", "0", CVar::None, "Debug the rooms: 1 outlines each room's bounds with its name and the floor of its boxes, 2 every box");
CVar gh_debug_nav("gh_debug_nav", "0", CVar::None, "Debug the nav mesh near the view: 1 areas and links, 2 also area ids and places");
CVar gh_debug_ghost("gh_debug_ghost", "0", CVar::None, "Debug the ghost: state, room, path, timers (seen through walls)");
CVar gh_debug_emf("gh_debug_emf", "0", CVar::None, "Debug EMF: every trace with level, cause and time left, and the reader's range");
CVar gh_debug_range("gh_debug_range", "1200", CVar::None, "How far from the view the nav and room debug views draw");

using Ov = game::Overlay;

Vec4 roomColor(int room, float alpha = 0.9f) {
    if (room < 0) return {0.6f, 0.6f, 0.6f, alpha * 0.7f};
    // Hues spread by the golden ratio, so neighbouring indices differ.
    float h = std::fmod(float(room) * 0.618034f, 1.0f) * 6;
    float x = 1 - std::abs(std::fmod(h, 2.0f) - 1);
    Vec3 c = h < 1 ? Vec3{1, x, 0} : h < 2 ? Vec3{x, 1, 0} : h < 3 ? Vec3{0, 1, x} : h < 4 ? Vec3{0, x, 1} : h < 5 ? Vec3{x, 0, 1} : Vec3{1, 0, x};
    c = c * 0.75f + Vec3{0.25f, 0.25f, 0.25f};
    return {c.x, c.y, c.z, alpha};
}

const Vec4 kText{1, 0.86f, 0.55f, 1}, kDim{0.85f, 0.85f, 0.85f, 0.9f}, kGhost{0.75f, 0.85f, 1, 1}, kThrow{1, 0.35f, 0.2f, 1};

Vec4 emfColor(int led) {
    static const Vec4 c[5] = {{0.2f, 0.9f, 0.3f, 1}, {0.6f, 0.95f, 0.2f, 1}, {1, 0.85f, 0.1f, 1}, {1, 0.5f, 0.1f, 1}, {1, 0.15f, 0.1f, 1}};
    return c[std::clamp(led, 0, 4)];
}

void floorQuad(Ov& ov, const nav::Area& a, const Vec4& color, float lift = 1) {
    Vec3 nw{a.nw.x, a.nw.y, a.nw.z + lift}, ne{a.se.x, a.nw.y, a.neZ + lift}, se{a.se.x, a.se.y, a.se.z + lift}, sw{a.nw.x, a.se.y, a.swZ + lift};
    ov.line(nw, ne, color);
    ov.line(ne, se, color);
    ov.line(se, sw, color);
    ov.line(sw, nw, color);
}

void drawEmfReader(Hunt& h, Ov& ov, const Vec3& at) {
    const EmfSource* src = nullptr;
    int level = h.emf.reading(at, gh_emf_range.getFloat(), &src);
    float w = 176, ht = 58, x = float(ov.width) - w - 24, y = float(ov.height) - ht - 110;
    ov.rect(x, y, w, ht, {0.05f, 0.05f, 0.06f, 0.75f});
    ov.text(x + 8, y + 6, "EMF", kDim, 1.4f);
    ov.text(x + w - 8, y + 6, std::to_string(level), level >= 2 ? emfColor(level - 1) : kDim, 1.4f, Ov::Align::Right);
    // A reading flickers a little, like the needle of a real meter.
    float flicker = level >= 2 ? 0.85f + 0.15f * std::sin(float(h.world.time()) * 37.0f) : 1.0f;
    for (int i = 0; i < 5; ++i) {
        Vec4 c = emfColor(i);
        bool lit = i < level;
        Vec4 shown = lit ? Vec4{c.x * flicker, c.y * flicker, c.z * flicker, 1} : Vec4{c.x * 0.22f, c.y * 0.22f, c.z * 0.22f, 0.9f};
        ov.rect(x + 8 + float(i) * 33, y + 26, 28, 22, shown);
    }
    (void)src;
}

void drawRooms(Hunt& h, Ov& ov, int mode, int ghostRoom) {
    float range = gh_debug_range.getFloat();
    for (size_t i = 0; i < h.rooms.size(); ++i) {
        const auto& r = h.rooms[i];
        Vec3 nearest = vmin(vmax(ov.eye, r.bounds.mins), r.bounds.maxs);
        if (lengthSq(nearest - ov.eye) > range * range) continue;
        bool ghost = int(i) == ghostRoom;
        Vec4 c = ghost ? Vec4{1, 0.2f, 0.2f, 1} : roomColor(int(i));
        ov.box(r.bounds.mins, r.bounds.maxs, {c.x, c.y, c.z, 0.55f});
        for (const auto& b : r.boxes) {
            if (mode >= 2) ov.box(b.mins, b.maxs, {c.x, c.y, c.z, 0.35f});
            else {
                float z = b.mins.z + 9;  // the floor of a nav place's box
                ov.line({b.mins.x, b.mins.y, z}, {b.maxs.x, b.mins.y, z}, c);
                ov.line({b.maxs.x, b.mins.y, z}, {b.maxs.x, b.maxs.y, z}, c);
                ov.line({b.maxs.x, b.maxs.y, z}, {b.mins.x, b.maxs.y, z}, c);
                ov.line({b.mins.x, b.maxs.y, z}, {b.mins.x, b.mins.y, z}, c);
            }
        }
        Vec3 center = r.bounds.center();
        center.z = r.bounds.mins.z + 64;
        ov.label(center, r.name + (ghost ? " (ghost room)" : ""), ghost ? Vec4{1, 0.4f, 0.4f, 1} : c, 1.4f, false);
    }
    int here = roomAt(h.rooms, ov.eye - Vec3{0, 0, 40});
    ov.text(16, 200, "room: " + (here >= 0 ? h.rooms[size_t(here)].name : std::string("none")) + "  (" + std::to_string(h.rooms.size()) +
                         " rooms from " + (h.rooms.empty() ? std::string("nowhere") : h.rooms.front().source) + ")",
            kText, 1.4f);
}

void drawNav(Hunt& h, Ov& ov, int mode) {
    std::vector<int> near;
    h.nav.areasNear(ov.eye, gh_debug_range.getFloat(), near);
    int under = h.nav.findArea(ov.eye - Vec3{0, 0, 64});
    for (int i : near) {
        const auto& a = h.nav.areas[size_t(i)];
        int room = h.areaRoom.empty() ? -1 : h.areaRoom[size_t(i)];
        floorQuad(ov, a, i == under ? Vec4{1, 1, 0.2f, 1} : roomColor(room, 0.8f));
        Vec3 c = a.center() + Vec3{0, 0, 4};
        for (int j : h.nav.links[size_t(i)]) {
            const auto& links = h.nav.links[size_t(j)];
            bool twoWay = std::find(links.begin(), links.end(), i) != links.end();
            if (twoWay && j < i) continue;  // drawn from the other side
            Vec3 d = h.nav.areas[size_t(j)].center() + Vec3{0, 0, 4};
            // One-way links (drops) in red, only the first half so the direction shows.
            if (twoWay) ov.line(c, d, {0.3f, 0.6f, 1, 0.7f});
            else ov.line(c, c + (d - c) * 0.6f, {1, 0.2f, 0.2f, 0.9f});
        }
        if (mode >= 2) {
            const std::string& place = a.place && a.place <= h.nav.places.size() ? h.nav.places[a.place - 1] : std::string();
            ov.label(c + Vec3{0, 0, 8}, "#" + std::to_string(a.id) + (place.empty() ? "" : " " + place), kDim, 1.0f, false);
        }
    }
    std::string status = std::format("nav: {} ({} areas, {} links)", h.nav.source, h.nav.areas.size(), h.nav.linkCount());
    if (under >= 0) {
        const auto& a = h.nav.areas[size_t(under)];
        status += std::format("  under you: #{} {}", a.id, a.place && a.place <= h.nav.places.size() ? h.nav.places[a.place - 1] : "");
    }
    ov.text(16, 218, status, kText, 1.4f);
}

void drawGhost(Hunt& h, Ov& ov) {
    auto* g = h.ghostState();
    if (!g) return;
    Vec3 p = h.ghostPos();
    double now = h.world.time();
    ov.box(p + Vec3{-16, -16, 0}, p + Vec3{16, 16, 72}, {kGhost.x, kGhost.y, kGhost.z, 0.8f}, true);
    // Path still to walk, and where it ends.
    Vec3 prev = p + Vec3{0, 0, 6};
    for (size_t i = g->pathIndex; i < g->path.size(); ++i) {
        Vec3 q = g->path[i] + Vec3{0, 0, 6};
        ov.line(prev, q, {1, 0.3f, 1, 1}, true);
        prev = q;
    }
    if (!g->path.empty()) ov.cross(g->goal + Vec3{0, 0, 6}, 24, {1, 0.3f, 1, 1}, true);
    std::string room = g->room >= 0 ? h.rooms[size_t(g->room)].name : "none";
    int at = roomAt(h.rooms, p + Vec3{0, 0, 16});
    std::string text = std::format("GHOST  {}\nroom {} (now in {})\nEMF 5 evidence: {}\nthrow in {:.0f} s, touch in {:.0f} s", stateName(g->state), room,
                                   at >= 0 ? h.rooms[size_t(at)].name : "none", g->emf5 ? "yes" : "no", std::max(0.0, g->nextThrow - now),
                                   std::max(0.0, g->nextTouch - now));
    if (g->lastEventTime >= 0) text += std::format("\nlast: {} ({:.0f} s ago)", g->lastEvent, now - g->lastEventTime);
    ov.label(p + Vec3{0, 0, 110}, text, kGhost, 1.2f, true);
}

void drawThrows(Hunt& h, Ov& ov) {
    auto* g = h.ghostState();
    double now = h.world.time();
    if (g) {
        CVar* range = CVar::find("gh_throw_range");
        ov.circle(h.ghostPos() + Vec3{0, 0, 40}, range ? range->getFloat() : 220, {1, 0.5f, 0.2f, 0.5f});
    }
    for (const auto& r : h.throws) {
        float age = float(now - r.time);
        float fade = std::clamp(1 - age / 30, 0.2f, 1.0f);
        Vec4 c{kThrow.x, kThrow.y, kThrow.z, fade};
        ov.line(r.ghostPos, r.propStart, {1, 0.9f, 0.3f, fade}, true);  // who threw it
        ov.line(r.propStart, r.propStart + r.velocity * 0.25f, c, true);  // which way
        ov.cross(r.propStart, 12, c, true);
        for (size_t i = 1; i < r.trail.size(); ++i) ov.line(r.trail[i - 1], r.trail[i], {1, 0.6f, 0.2f, fade}, true);
        ov.label(r.propStart + Vec3{0, 0, 20}, std::format("{} {:.0f} u/s, EMF {}, {:.0f} s ago", r.model, length(r.velocity), r.emfLevel, age), c, 1.1f,
                 true);
    }
}

void drawEmfTraces(Hunt& h, Ov& ov) {
    double now = h.world.time();
    for (const auto& s : h.emf.sources) {
        Vec4 c = emfColor(s.level - 1);
        ov.cross(s.pos, 16, c, true);
        ov.label(s.pos + Vec3{0, 0, 12}, std::format("EMF {} {} {:.0f} s", s.level, s.cause, s.end - now), c, 1.1f, true);
    }
    Vec3 hand = ov.eye - Vec3{0, 0, 20};
    ov.circle(hand, gh_emf_range.getFloat(), {0.3f, 1, 0.4f, 0.6f});
}

void drawHunt(World& w, Entity viewer, Ov& ov) {
    Hunt* h = huntOf(w);
    if (!h) return;
    (void)viewer;
    auto* g = h->ghostState();
    if (gh_debug_rooms.getInt() > 0) drawRooms(*h, ov, gh_debug_rooms.getInt(), g ? g->room : -1);
    if (gh_debug_nav.getInt() > 0) drawNav(*h, ov, gh_debug_nav.getInt());
    if (gh_debug_ghost.getBool()) drawGhost(*h, ov);
    if (CVar* t = CVar::find("gh_debug_throws"); t && t->getBool()) drawThrows(*h, ov);
    if (gh_debug_emf.getBool()) drawEmfTraces(*h, ov);
    if (gh_hud_emf.getBool()) drawEmfReader(*h, ov, ov.eye - Vec3{0, 0, 20});
}

}  // namespace

void registerOverlays() { game::addOverlayProvider(drawHunt); }

}  // namespace ose::ghosthunt
