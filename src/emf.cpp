// EMF traces the ghost leaves, and what a reader shows.
#include <algorithm>

#include "game/components.h"
#include "ghosthunt.h"
#include "physics/rigid.h"

namespace ose::ghosthunt {

void EmfField::update(World& world) {
    auto& reg = world.registry();
    double now = world.time();
    std::erase_if(sources, [now](const EmfSource& s) { return now >= s.end; });
    for (auto& s : sources) {
        if (!s.follow) continue;
        if (!reg.valid(s.follow)) {
            s.follow = {};
            continue;
        }
        // A physics prop's centre of mass, else its origin.
        if (auto* pp = reg.get<game::PhysicsProp>(s.follow))
            if (auto* b = world.rigid().get(pp->body)) {
                s.pos = b->pos;
                continue;
            }
        if (auto* t = reg.get<game::Transform>(s.follow)) s.pos = t->origin;
    }
}

int EmfField::reading(const Vec3& p, float range, const EmfSource** strongest) const {
    int level = 1;
    if (strongest) *strongest = nullptr;
    for (const auto& s : sources) {
        if (lengthSq(s.pos - p) > range * range || s.level <= level) continue;
        level = s.level;
        if (strongest) *strongest = &s;
    }
    return std::min(level, 5);
}

}  // namespace ose::ghosthunt
