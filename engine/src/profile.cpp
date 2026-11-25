#include "profile.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <Geom_Circle.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace cadx {

namespace {

using sketch::Entity;
using sketch::EntityType;
using sketch::Vec2;

constexpr double kMergeTol = 1e-6;  // mm: endpoints closer than this are the same point
constexpr double kDeg = std::numbers::pi / 180.0;

std::string at(const Vec2& p) {
  std::ostringstream s;
  s << "(" << p.x << ", " << p.y << ")";
  return s.str();
}

// One open curve of the sketch (line or arc), between two merged vertices.
struct Segment {
  const Entity* entity;
  int v0, v1;  // vertex indices of its start and end
};

struct Loop {
  std::vector<std::pair<int, bool>> steps;  // segment index, traversed forwards?
  const Entity* circle = nullptr;           // set for a single full circle
  double area = 0;                          // signed: > 0 counter-clockwise
  std::vector<Vec2> polygon;                // sampled outline, for area and containment
};

void sampleSegment(const Entity& e, bool forward, std::vector<Vec2>& out) {
  if (e.type == EntityType::Line) {
    out.push_back(forward ? e.a : e.b);
    return;
  }
  double a0 = e.startDeg, a1 = e.endDeg;
  while (a1 <= a0) a1 += 360;
  const int n = std::max(4, static_cast<int>((a1 - a0) / 10));
  for (int i = 0; i < n; ++i) {  // excludes the far end, which the next segment adds
    const double t = forward ? a0 + (a1 - a0) * i / n : a1 - (a1 - a0) * i / n;
    out.push_back({e.center.x + e.radius * std::cos(t * kDeg), e.center.y + e.radius * std::sin(t * kDeg)});
  }
}

double signedArea(const std::vector<Vec2>& p) {
  double a = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const Vec2& u = p[i];
    const Vec2& v = p[(i + 1) % p.size()];
    a += u.x * v.y - v.x * u.y;
  }
  return a / 2;
}

bool inside(const Vec2& q, const std::vector<Vec2>& poly) {
  bool in = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Vec2& a = poly[i];
    const Vec2& b = poly[j];
    if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

}  // namespace

Profile buildProfile(const sketch::Sketch& s) {
  // 1. Merge endpoints into vertices and collect segments and circles.
  std::vector<Vec2> verts;
  auto vertexIndex = [&](const Vec2& p) {
    for (std::size_t i = 0; i < verts.size(); ++i) {
      if (std::hypot(verts[i].x - p.x, verts[i].y - p.y) < kMergeTol) return static_cast<int>(i);
    }
    verts.push_back(p);
    return static_cast<int>(verts.size() - 1);
  };

  std::vector<Segment> segs;
  std::vector<Loop> loops;
  for (const auto& e : s.entities) {
    if (e.construction) continue;
    switch (e.type) {
      case EntityType::Point: break;
      case EntityType::Line: {
        const int a = vertexIndex(e.a), b = vertexIndex(e.b);
        if (a == b) throw ProfileError("line " + e.id + " has zero length");
        segs.push_back({&e, a, b});
        break;
      }
      case EntityType::Arc: {
        const int a = vertexIndex(e.arcStart()), b = vertexIndex(e.arcEnd());
        if (a == b) throw ProfileError("arc " + e.id + " has zero length");
        segs.push_back({&e, a, b});
        break;
      }
      case EntityType::Circle: {
        if (e.radius <= 0) throw ProfileError("circle " + e.id + " has no radius");
        Loop l;
        l.circle = &e;
        for (int i = 0; i < 36; ++i) {
          l.polygon.push_back({e.center.x + e.radius * std::cos(i * 10 * kDeg),
                               e.center.y + e.radius * std::sin(i * 10 * kDeg)});
        }
        loops.push_back(l);
        break;
      }
    }
  }

  // 2. Every vertex must join exactly two segments, or the profile is open or branches.
  std::vector<std::vector<int>> at_vertex(verts.size());
  for (std::size_t i = 0; i < segs.size(); ++i) {
    at_vertex[segs[i].v0].push_back(static_cast<int>(i));
    at_vertex[segs[i].v1].push_back(static_cast<int>(i));
  }
  for (std::size_t v = 0; v < verts.size(); ++v) {
    if (at_vertex[v].size() == 1) throw ProfileError("the profile is open at " + at(verts[v]));
    if (at_vertex[v].size() > 2) throw ProfileError("the profile branches at " + at(verts[v]));
  }

  // 3. Walk the loops.
  std::vector<bool> used(segs.size(), false);
  for (std::size_t start = 0; start < segs.size(); ++start) {
    if (used[start]) continue;
    Loop l;
    int seg = static_cast<int>(start);
    int v = segs[seg].v0;
    while (!used[seg]) {
      used[seg] = true;
      const bool forward = segs[seg].v0 == v;
      l.steps.emplace_back(seg, forward);
      sampleSegment(*segs[seg].entity, forward, l.polygon);
      v = forward ? segs[seg].v1 : segs[seg].v0;
      const auto& next = at_vertex[v];
      seg = next[0] == seg ? next[1] : next[0];
    }
    loops.push_back(l);
  }
  if (loops.empty()) throw ProfileError("sketch " + s.id + " has no closed profile");
  for (auto& l : loops) l.area = signedArea(l.polygon);

  // 4. Nesting: each loop's parent is the smallest loop containing it.
  //    Even depth = outer boundary of a region, odd depth = a hole in its parent.
  std::sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return std::abs(a.area) > std::abs(b.area); });
  std::vector<int> parent(loops.size(), -1), depth(loops.size(), 0);
  for (std::size_t i = 0; i < loops.size(); ++i) {
    for (std::size_t j = i; j-- > 0;) {  // smaller-index loops are larger; scan from smallest
      if (inside(loops[i].polygon.front(), loops[j].polygon)) {
        parent[i] = static_cast<int>(j);
        depth[i] = depth[j] + 1;
        break;
      }
    }
  }

  // 5. Build OCCT geometry with shared vertices, so each edge keeps its identity.
  std::vector<TopoDS_Vertex> occVerts;
  for (const auto& p : verts) occVerts.push_back(BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, 0)).Vertex());

  Profile out;
  std::vector<TopoDS_Edge> segEdges(segs.size());
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const Entity& e = *segs[i].entity;
    if (e.type == EntityType::Line) {
      segEdges[i] = BRepBuilderAPI_MakeEdge(occVerts[segs[i].v0], occVerts[segs[i].v1]).Edge();
    } else {
      double a0 = e.startDeg * kDeg, a1 = e.endDeg * kDeg;
      while (a1 <= a0) a1 += 2 * std::numbers::pi;
      Handle(Geom_Circle) c = new Geom_Circle(gp_Ax2(gp_Pnt(e.center.x, e.center.y, 0), gp::DZ()), e.radius);
      BRepBuilderAPI_MakeEdge mk(c, occVerts[segs[i].v0], occVerts[segs[i].v1], a0, a1);
      if (!mk.IsDone()) throw ProfileError("could not build arc " + e.id);
      segEdges[i] = mk.Edge();
    }
    out.edges.emplace_back(segEdges[i], s.id + "/" + e.id);
  }

  auto wireOf = [&](const Loop& l) {
    BRepBuilderAPI_MakeWire mw;
    if (l.circle) {
      const Entity& e = *l.circle;
      TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(
          gp_Circ(gp_Ax2(gp_Pnt(e.center.x, e.center.y, 0), gp::DZ()), e.radius)).Edge();
      out.edges.emplace_back(edge, s.id + "/" + e.id);
      mw.Add(edge);
    } else {
      for (const auto& [seg, forward] : l.steps) mw.Add(segEdges[seg]);
    }
    if (!mw.IsDone()) throw ProfileError("could not join the profile edges of " + s.id);
    return mw.Wire();
  };

  // Outer boundaries counter-clockwise, holes clockwise.
  std::vector<int> faceOf(loops.size(), -1);
  std::vector<BRepBuilderAPI_MakeFace> makers;
  for (std::size_t i = 0; i < loops.size(); ++i) {
    if (depth[i] % 2 != 0) continue;
    TopoDS_Wire outer = wireOf(loops[i]);
    if (loops[i].area < 0) outer.Reverse();
    makers.emplace_back(gp_Pln(gp::Origin(), gp::DZ()), outer, true);
    faceOf[i] = static_cast<int>(makers.size() - 1);
  }
  for (std::size_t i = 0; i < loops.size(); ++i) {
    if (depth[i] % 2 == 0) continue;
    TopoDS_Wire hole = wireOf(loops[i]);
    if (loops[i].area > 0) hole.Reverse();
    makers[faceOf[parent[i]]].Add(hole);
  }
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  for (auto& mf : makers) {
    if (!mf.IsDone()) throw ProfileError("could not make a face from " + s.id);
    out.faces.push_back(mf.Face());
    builder.Add(compound, mf.Face());
  }
  out.shape = out.faces.size() == 1 ? TopoDS_Shape(out.faces.front()) : TopoDS_Shape(compound);
  return out;
}

}  // namespace cadx
