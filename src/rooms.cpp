// Rooms: read from and written to rooms files, and guessed from the map's soundscapes.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>

#include "game/inspect.h"
#include "ghosthunt.h"
#include "kv/keyvalues.h"

namespace ose::ghosthunt {

bool Room::contains(const Vec3& p) const {
    for (const auto& b : boxes)
        if (p.x >= b.mins.x && p.y >= b.mins.y && p.z >= b.mins.z && p.x <= b.maxs.x && p.y <= b.maxs.y && p.z <= b.maxs.z) return true;
    return false;
}

std::vector<Room> parseRooms(std::string_view text, std::string* error) {
    std::vector<Room> rooms;
    auto doc = kv::parse(text);
    if (!doc) {
        if (error) *error = doc.error();
        return rooms;
    }
    const kv::KeyValues* root = doc->find("Rooms");
    if (!root) root = &*doc;
    for (const auto* r : root->findAll("room")) {
        Room room;
        room.name = std::string(r->get("name", "unnamed"));
        room.source = "file";
        for (const auto* b : r->findAll("box")) {
            auto v = splitWhitespace(b->value);
            if (v.size() != 6) {
                if (error) *error = "room " + room.name + ": a box needs six numbers";
                continue;
            }
            Vec3 a{parseFloat(v[0]), parseFloat(v[1]), parseFloat(v[2])}, c{parseFloat(v[3]), parseFloat(v[4]), parseFloat(v[5])};
            room.addBox({vmin(a, c), vmax(a, c)});
        }
        if (!room.boxes.empty()) rooms.push_back(std::move(room));
    }
    return rooms;
}

std::string writeRooms(const std::vector<Room>& rooms, std::string_view comment) {
    std::string out = "// " + std::string(comment) + "\n// box: min x y z, max x y z (world units). A room may have several boxes.\n\"Rooms\"\n{\n";
    for (const auto& r : rooms) {
        out += "\t\"room\"\n\t{\n\t\t\"name\" \"" + r.name + "\"\n";
        if (!r.source.empty()) out += "\t\t// from " + r.source + "\n";
        for (const auto& b : r.boxes)
            out += std::format("\t\t\"box\" \"{:.0f} {:.0f} {:.0f} {:.0f} {:.0f} {:.0f}\"\n", b.mins.x, b.mins.y, b.mins.z, b.maxs.x, b.maxs.y,
                               b.maxs.z);
        out += "\t}\n";
    }
    return out + "}\n";
}

int roomAt(const std::vector<Room>& rooms, const Vec3& p) {
    int best = -1;
    float bestVolume = 0;
    for (size_t i = 0; i < rooms.size(); ++i) {
        if (!rooms[i].contains(p)) continue;
        Vec3 s = rooms[i].bounds.maxs - rooms[i].bounds.mins;
        float v = s.x * s.y * s.z;
        if (best < 0 || v < bestVolume) best = int(i), bestVolume = v;
    }
    return best;
}

// ------------------------------------------------------------------ soundscape guesses
namespace {

constexpr int kMask = bsp::Contents::MaskPlayerSolid;

float boxVolume(const AABB& b) {
    Vec3 s = vmax(b.maxs - b.mins, Vec3{});
    return s.x * s.y * s.z;
}

float overlapVolume(const AABB& a, const AABB& b) { return boxVolume({vmax(a.mins, b.mins), vmin(a.maxs, b.maxs)}); }

// "sodamachinesound" -> "Sodamachine", "office.indoorsa" -> "Indoorsa".
std::string niceName(std::string_view raw) {
    std::string s(raw.substr(raw.rfind('.') == std::string_view::npos ? 0 : raw.rfind('.') + 1));
    for (std::string_view suffix : {"_sound", "sound", "_snd", "snd"})
        if (s.size() > suffix.size() + 2 && iendsWith(s, suffix)) {
            s.resize(s.size() - suffix.size());
            break;
        }
    while (!s.empty() && (s.back() == '_' || s.back() == '-')) s.pop_back();
    if (!s.empty()) s[0] = char(std::toupper(uint8_t(s[0])));
    return s.empty() ? "Room" : s;
}

// Target names that say nothing about the place ("pos0", "target3", "sound12").
bool genericName(std::string_view name) {
    std::string s = toLower(name);
    while (!s.empty() && (std::isdigit(uint8_t(s.back())) || s.back() == '_')) s.pop_back();
    for (std::string_view g : {"", "pos", "position", "target", "snd", "sound", "speaker", "point", "p", "s"})
        if (s == g) return true;
    return false;
}

// The walls around `origin`: the floor below it, the ceiling above, and per side the median
// distance of the rays that leave towards that side (a door or a window lets single rays
// through; the median ignores them), limited to `radius`.
bool traceRoomBox(World& world, const Vec3& origin, float radius, AABB& out) {
    auto& col = world.collision();
    if (col.traceRay(origin, origin, kMask).startSolid) return false;
    auto down = col.traceRay(origin, origin - Vec3{0, 0, 1024}, kMask);
    if (down.fraction >= 1) return false;
    float floorZ = down.endpos.z;
    Vec3 eye{origin.x, origin.y, floorZ + 40};
    auto up = col.traceRay(eye, eye + Vec3{0, 0, 512}, kMask);
    float ceilZ = std::min(up.endpos.z, floorZ + 320);
    float limit = std::clamp(radius > 0 ? radius : 768.0f, 64.0f, 1536.0f);
    constexpr int kRays = 72;
    std::vector<float> side[4];  // +x, +y, -x, -y
    for (int i = 0; i < kRays; ++i) {
        float a = float(i) / kRays * 6.2831853f;
        Vec3 dir{std::cos(a), std::sin(a), 0};
        auto tr = col.traceRay(eye, eye + dir * limit, kMask);
        float d = tr.fraction * limit;
        // Rays within 30 degrees of an axis measure that side (projected onto the axis).
        for (int s = 0; s < 4; ++s) {
            float axisAngle = float(s) * 1.5707963f;
            float diff = std::abs(std::remainder(a - axisAngle, 6.2831853f));
            if (diff <= 0.5236f) side[s].push_back(d * std::cos(diff));
        }
    }
    float ext[4];
    for (int s = 0; s < 4; ++s) {
        auto& v = side[s];
        std::sort(v.begin(), v.end());
        ext[s] = v.empty() ? 32 : std::max(16.0f, v[v.size() / 2]);
    }
    out.mins = {eye.x - ext[2], eye.y - ext[3], floorZ - 8};
    out.maxs = {eye.x + ext[0], eye.y + ext[1], std::max(ceilZ, floorZ + 64)};
    return true;
}

}  // namespace

std::vector<Room> guessRoomsFromSoundscapes(World& world) {
    auto& reg = world.registry();
    std::vector<Room> rooms;
    std::map<std::string, int> nameCount;
    auto addRoom = [&](std::string name, const AABB& box, std::string source) {
        // Mostly the same space as an earlier guess: keep the earlier one.
        for (const auto& r : rooms)
            if (overlapVolume(r.boxes[0], box) > 0.6f * std::min(boxVolume(r.boxes[0]), boxVolume(box))) return;
        int n = ++nameCount[name];
        Room room;
        room.name = n > 1 ? name + " " + std::to_string(n) : name;
        room.source = std::move(source);
        room.addBox(box);
        rooms.push_back(std::move(room));
    };
    // Named sound sources (position0..7 targets) inside a box name it.
    auto nameFromTargets = [&](const kv::KeyValues& kv, const AABB& box) -> std::string {
        for (int i = 0; i < 8; ++i) {
            std::string_view tgt = kv.get("position" + std::to_string(i));
            if (tgt.empty() || genericName(tgt)) continue;
            for (Entity te : world.findTargets(tgt)) {
                auto* t = reg.get<game::Transform>(te);
                if (!t) continue;
                const Vec3& p = t->origin;
                if (p.x >= box.mins.x && p.y >= box.mins.y && p.x <= box.maxs.x && p.y <= box.maxs.y && p.z >= box.mins.z - 64 &&
                    p.z <= box.maxs.z + 64)
                    return niceName(tgt);
            }
        }
        return {};
    };

    // trigger_soundscape brushes are rooms already.
    for (auto [e, cn] : reg.view<game::ClassName>()) {
        if (cn.classname != "trigger_soundscape") continue;
        Vec3 mn, mx;
        Mat3x4 xf = game::entityBounds(world, e, mn, mx);
        AABB box;
        for (int i = 0; i < 8; ++i) box.add(transformPoint(xf, {i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z}));
        auto* kvc = reg.get<game::KeyValuesComp>(e);
        Entity scape = kvc ? world.findByName(kvc->get("soundscape")) : Entity{};
        auto* skv = scape ? reg.get<game::KeyValuesComp>(scape) : nullptr;
        std::string name = skv ? nameFromTargets(skv->kv, box) : "";
        std::string scapeName = skv ? std::string(skv->get("soundscape")) : "";
        if (name.empty()) name = niceName(scapeName.empty() ? "Area" : scapeName);
        addRoom(name, box, "trigger_soundscape " + scapeName);
    }
    for (auto [e, cn, t, kvc] : reg.view<game::ClassName, game::Transform, game::KeyValuesComp>()) {
        if (cn.classname != "env_soundscape" && cn.classname != "env_soundscape_proxy") continue;
        AABB box;
        if (!traceRoomBox(world, t.origin, kvc.kv.getFloat("radius", -1), box)) continue;
        const kv::KeyValues* scapeKv = &kvc.kv;
        if (cn.classname == "env_soundscape_proxy")
            if (Entity main = world.findByName(kvc.get("MainSoundscapeName")))
                if (auto* mk = reg.get<game::KeyValuesComp>(main)) scapeKv = &mk->kv;
        std::string scapeName(scapeKv->get("soundscape"));
        std::string name = nameFromTargets(*scapeKv, box);
        if (name.empty()) name = niceName(scapeName.empty() ? "Area" : scapeName);
        addRoom(name, box, std::format("{} {} at {:.0f} {:.0f} {:.0f}", cn.classname, scapeName, t.origin.x, t.origin.y, t.origin.z));
    }
    return rooms;
}

}  // namespace ose::ghosthunt
