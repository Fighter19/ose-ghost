// Navigation over walkable areas: the map's .nav areas or a generated grid, and A* paths.
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

#include "ghosthunt.h"

namespace ose::ghosthunt {

namespace {
constexpr float kStepHeight = 18, kMaxDrop = 64;
const Vec3 kHullMins{-16, -16, 0}, kHullMaxs{16, 16, 72};
constexpr int kMask = bsp::Contents::MaskNpcSolid;
}  // namespace

void NavMesh::fromNav(const nav::NavFile& file) {
    areas = file.areas;
    places = file.places;
    std::unordered_map<uint32_t, int> byId;
    for (size_t i = 0; i < areas.size(); ++i) byId[areas[i].id] = int(i);
    links.assign(areas.size(), {});
    for (size_t i = 0; i < areas.size(); ++i)
        for (const auto& dir : areas[i].connections)
            for (uint32_t id : dir)
                if (auto it = byId.find(id); it != byId.end() && it->second != int(i)) links[i].push_back(it->second);
    buildIndex();
}

void NavMesh::generate(World& world, const std::vector<Vec3>& seeds, float cell, size_t maxAreas) {
    areas.clear();
    links.clear();
    places.clear();
    auto& col = world.collision();
    float half = cell * 0.5f;
    // Cells by grid column; several floors can share a column.
    std::unordered_map<int64_t, std::vector<int>> columns;
    auto cellOf = [&](float v) { return int(std::floor(v / cell)); };
    auto findCell = [&](int cx, int cy, float z) -> int {
        auto it = columns.find(bucketKey(cx, cy));
        if (it == columns.end()) return -1;
        for (int i : it->second)
            if (std::abs(areas[size_t(i)].nw.z - z) < kStepHeight + 2) return i;
        return -1;
    };
    // Floor below `p` (a hull standing there), or false.
    auto settle = [&](const Vec3& p, float drop, Vec3& floor) {
        auto tr = col.traceBox(p, p - Vec3{0, 0, drop}, kHullMins, kHullMaxs, kMask);
        if (tr.startSolid || tr.fraction >= 1 || tr.plane.normal.z < 0.7f) return false;
        floor = tr.endpos;
        return true;
    };
    std::queue<int> open;
    auto addCell = [&](int cx, int cy, float z) {
        nav::Area a;
        a.id = uint32_t(areas.size() + 1);
        a.nw = {float(cx) * cell, float(cy) * cell, z};
        a.se = {float(cx + 1) * cell, float(cy + 1) * cell, z};
        a.neZ = a.swZ = z;
        areas.push_back(a);
        links.emplace_back();
        columns[bucketKey(cx, cy)].push_back(int(areas.size() - 1));
        open.push(int(areas.size() - 1));
        return int(areas.size() - 1);
    };
    for (const Vec3& s : seeds) {
        int cx = cellOf(s.x), cy = cellOf(s.y);
        Vec3 floor;
        Vec3 start{float(cx) * cell + half, float(cy) * cell + half, s.z + kStepHeight};
        if (!settle(start, kStepHeight + kMaxDrop, floor) || findCell(cx, cy, floor.z) >= 0) continue;
        addCell(cx, cy, floor.z);
    }
    static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    while (!open.empty()) {
        int i = open.front();
        open.pop();
        Vec3 from = areas[size_t(i)].center();
        int cx = cellOf(from.x), cy = cellOf(from.y);
        for (const auto& d : dirs) {
            int nx = cx + d[0], ny = cy + d[1];
            Vec3 lifted = from + Vec3{0, 0, kStepHeight};
            Vec3 to{float(nx) * cell + half, float(ny) * cell + half, lifted.z};
            if (col.traceBox(lifted, to, kHullMins, kHullMaxs, kMask).fraction < 1) continue;
            Vec3 floor;
            if (!settle(to, kStepHeight + kMaxDrop, floor)) continue;
            int j = findCell(nx, ny, floor.z);
            if (j < 0) {
                if (areas.size() >= maxAreas) continue;
                j = addCell(nx, ny, floor.z);
            }
            links[size_t(i)].push_back(j);
            // Back up only when it is a step, not a drop.
            if (floor.z - from.z < kStepHeight + 1 && floor.z - from.z > -(kStepHeight + 1)) links[size_t(j)].push_back(i);
        }
    }
    for (auto& l : links) {
        std::sort(l.begin(), l.end());
        l.erase(std::unique(l.begin(), l.end()), l.end());
    }
    source = "generated";
    buildIndex();
}

void NavMesh::buildIndex() {
    buckets_.clear();
    for (size_t i = 0; i < areas.size(); ++i) {
        const auto& a = areas[i];
        for (int x = int(std::floor(a.nw.x / kBucket)); x <= int(std::floor(a.se.x / kBucket)); ++x)
            for (int y = int(std::floor(a.nw.y / kBucket)); y <= int(std::floor(a.se.y / kBucket)); ++y) buckets_[bucketKey(x, y)].push_back(int(i));
    }
}

size_t NavMesh::linkCount() const {
    size_t n = 0;
    for (const auto& l : links) n += l.size();
    return n;
}

void NavMesh::areasNear(const Vec3& p, float radius, std::vector<int>& out) const {
    out.clear();
    int x0 = int(std::floor((p.x - radius) / kBucket)), x1 = int(std::floor((p.x + radius) / kBucket));
    int y0 = int(std::floor((p.y - radius) / kBucket)), y1 = int(std::floor((p.y + radius) / kBucket));
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y) {
            auto it = buckets_.find(bucketKey(x, y));
            if (it == buckets_.end()) continue;
            for (int i : it->second) {
                const auto& a = areas[size_t(i)];
                Vec3 c = a.center();
                if (std::abs(c.z - p.z) > radius) continue;
                float dx = std::max({a.nw.x - p.x, 0.0f, p.x - a.se.x}), dy = std::max({a.nw.y - p.y, 0.0f, p.y - a.se.y});
                if (dx * dx + dy * dy <= radius * radius) out.push_back(i);
            }
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

int NavMesh::findArea(const Vec3& p) const {
    int best = -1;
    float bestDz = 1e30f;
    if (auto it = buckets_.find(bucketKey(int(std::floor(p.x / kBucket)), int(std::floor(p.y / kBucket)))); it != buckets_.end())
        for (int i : it->second) {
            const auto& a = areas[size_t(i)];
            if (!a.containsXY(p.x, p.y)) continue;
            float dz = p.z - a.floorZ(p.x, p.y);
            if (dz < -kStepHeight || dz > 120) continue;
            if (std::abs(dz) < bestDz) best = i, bestDz = std::abs(dz);
        }
    if (best >= 0) return best;
    std::vector<int> near;
    areasNear(p, 256, near);
    float bestD = 1e30f;
    for (int i : near) {
        float d = lengthSq(areas[size_t(i)].center() - p);
        if (d < bestD) best = i, bestD = d;
    }
    return best;
}

bool NavMesh::findPath(const Vec3& start, const Vec3& goal, std::vector<Vec3>& points) const {
    points.clear();
    int from = findArea(start), to = findArea(goal);
    if (from < 0 || to < 0) return false;
    std::vector<float> cost(areas.size(), 1e30f);
    std::vector<int> prev(areas.size(), -1);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    Vec3 goalCenter = areas[size_t(to)].center();
    cost[size_t(from)] = 0;
    open.push({length(areas[size_t(from)].center() - goalCenter), from});
    while (!open.empty()) {
        auto [f, i] = open.top();
        open.pop();
        if (i == to) break;
        Vec3 ci = areas[size_t(i)].center();
        if (f - length(ci - goalCenter) > cost[size_t(i)] + 1e-3f) continue;  // stale entry
        for (int j : links[size_t(i)]) {
            Vec3 cj = areas[size_t(j)].center();
            float c = cost[size_t(i)] + length(cj - ci);
            if (c >= cost[size_t(j)]) continue;
            cost[size_t(j)] = c;
            prev[size_t(j)] = i;
            open.push({c + length(cj - goalCenter), j});
        }
    }
    if (from != to && prev[size_t(to)] < 0) return false;
    std::vector<int> chain;
    for (int i = to; i >= 0; i = prev[size_t(i)]) {
        chain.push_back(i);
        if (i == from) break;
    }
    std::reverse(chain.begin(), chain.end());
    points.push_back(start);
    // Through the edge each pair of areas shares: the point of the overlap closest to the
    // middle between their centres.
    for (size_t k = 1; k < chain.size(); ++k) {
        const auto& a = areas[size_t(chain[k - 1])];
        const auto& b = areas[size_t(chain[k])];
        Vec3 mid = (a.center() + b.center()) * 0.5f;
        float x0 = std::max(a.nw.x, b.nw.x), x1 = std::min(a.se.x, b.se.x);
        float y0 = std::max(a.nw.y, b.nw.y), y1 = std::min(a.se.y, b.se.y);
        float x = x0 <= x1 ? std::clamp(mid.x, x0, x1) : mid.x, y = y0 <= y1 ? std::clamp(mid.y, y0, y1) : mid.y;
        points.push_back({x, y, std::max(a.floorZ(x, y), b.floorZ(x, y))});
    }
    points.push_back({goal.x, goal.y, areas[size_t(to)].floorZ(goal.x, goal.y)});
    return true;
}

}  // namespace ose::ghosthunt
