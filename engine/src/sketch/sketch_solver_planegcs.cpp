// PlaneGCS backend for cadx::sketch::solve. Nothing outside this file
// includes PlaneGCS headers.

#include "sketch/sketch.hpp"

#include "App/planegcs/GCS.h"

#include <cmath>
#include <deque>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>

namespace cadx::sketch {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

struct BadSketch : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};

// Parameter storage. A deque never moves its elements, so the raw pointers
// PlaneGCS keeps stay valid as we add more.
class Params {
 public:
  double* add(double v, bool fixed = false) {
    store_.push_back(v);
    double* p = &store_.back();
    if (fixed) fixed_.insert(p);
    else all_.push_back(p);
    return p;
  }
  void fix(double* p) { fixed_.insert(p); }
  GCS::VEC_pD unknowns() const {
    GCS::VEC_pD out;
    for (double* p : all_) {
      if (!fixed_.count(p)) out.push_back(p);
    }
    return out;
  }

 private:
  std::deque<double> store_;
  std::vector<double*> all_;
  std::set<double*> fixed_;
};

struct Geo {
  const Entity* entity = nullptr;
  GCS::Point point;   // point entities
  GCS::Line line;
  GCS::Circle circle;
  GCS::Arc arc;
};

// Normalise arc angles (radians) so that start < end <= start + 2pi.
void normaliseArc(double& start, double& end) {
  while (end <= start) end += 2 * kPi;
  while (end > start + 2 * kPi) end -= 2 * kPi;
}

class Builder {
 public:
  explicit Builder(const Sketch& s) : sketch_(s) {
    origin_ = GCS::Point(params_.add(0, true), params_.add(0, true));
    xaxis_.p1 = origin_;
    xaxis_.p2 = GCS::Point(params_.add(1, true), params_.add(0, true));
    yaxis_.p1 = origin_;
    yaxis_.p2 = GCS::Point(params_.add(0, true), params_.add(1, true));

    for (const auto& e : s.entities) {
      Geo g;
      g.entity = &e;
      switch (e.type) {
        case EntityType::Point:
          g.point = point(e.a);
          break;
        case EntityType::Line:
          g.line.p1 = point(e.a);
          g.line.p2 = point(e.b);
          break;
        case EntityType::Circle:
          g.circle.center = point(e.center);
          g.circle.rad = params_.add(e.radius);
          break;
        case EntityType::Arc: {
          double a0 = e.startDeg * kDeg, a1 = e.endDeg * kDeg;
          normaliseArc(a0, a1);
          g.arc.center = point(e.center);
          g.arc.rad = params_.add(e.radius);
          g.arc.startAngle = params_.add(a0);
          g.arc.endAngle = params_.add(a1);
          g.arc.start = point(e.arcStart());
          g.arc.end = point(e.arcEnd());
          break;
        }
      }
      geos_.emplace(e.id, g);
    }
    // Arcs need their internal rules (endpoints follow centre, radius, angles).
    for (auto& [id, g] : geos_) {
      if (g.entity->type == EntityType::Arc) sys_.addConstraintArcRules(g.arc, 0);
    }
    for (std::size_t i = 0; i < s.constraints.size(); ++i) {
      addConstraint(s.constraints[i], static_cast<int>(i + 1));
    }
  }

  SolveResult run() {
    SolveResult out;
    out.sketch = sketch_;

    GCS::VEC_pD unknowns = params_.unknowns();
    sys_.declareUnknowns(unknowns);
    sys_.initSolution(GCS::DogLeg);

    GCS::VEC_I conflicting, redundant;
    sys_.getConflicting(conflicting);
    sys_.getRedundant(redundant);
    out.dof = sys_.dofsNumber();
    out.conflicting = tagsToIds(conflicting);
    out.redundant = tagsToIds(redundant);

    // Conflicts that are not merely redundant mean the sketch cannot be solved.
    std::set<std::string> red(out.redundant.begin(), out.redundant.end());
    bool realConflict = false;
    for (const auto& c : out.conflicting) {
      if (!red.count(c)) realConflict = true;
    }
    if (realConflict) {
      out.state = SolveState::Conflicting;
      out.message = "conflicting constraints";
      return out;
    }

    int ret = GCS::Failed;
    for (GCS::Algorithm alg : {GCS::DogLeg, GCS::LevenbergMarquardt, GCS::BFGS}) {
      ret = sys_.solve(true, alg);
      if (ret == GCS::Success || ret == GCS::Converged) break;
      sys_.undoSolution();
    }
    if (ret != GCS::Success && ret != GCS::Converged) {
      out.state = SolveState::Failed;
      out.message = "the solver did not converge";
      return out;
    }
    sys_.applySolution();
    writeBack(out.sketch);
    out.state = SolveState::Solved;
    if (!out.redundant.empty()) out.message = "redundant constraints (harmless, but can be removed)";
    return out;
  }

 private:
  GCS::Point point(const Vec2& v) { return GCS::Point(params_.add(v.x), params_.add(v.y)); }

  const Geo& geo(const std::string& id) {
    auto it = geos_.find(id);
    if (it == geos_.end()) throw BadSketch("unknown entity: " + id);
    return it->second;
  }

  GCS::Point pointRef(const std::string& ref) {
    if (ref == "origin") return origin_;
    const auto dot = ref.find('.');
    const std::string id = ref.substr(0, dot);
    const std::string part = dot == std::string::npos ? "" : ref.substr(dot + 1);
    const Geo& g = geo(id);
    switch (g.entity->type) {
      case EntityType::Point:
        if (part.empty()) return g.point;
        break;
      case EntityType::Line:
        if (part == "start") return g.line.p1;
        if (part == "end") return g.line.p2;
        break;
      case EntityType::Circle:
        if (part == "center") return g.circle.center;
        break;
      case EntityType::Arc:
        if (part == "center") return g.arc.center;
        if (part == "start") return g.arc.start;
        if (part == "end") return g.arc.end;
        break;
    }
    throw BadSketch("not a point reference: " + ref);
  }

  bool isPointRef(const std::string& ref) {
    if (ref == "origin") return true;
    if (ref.find('.') != std::string::npos) return true;
    auto it = geos_.find(ref);
    return it != geos_.end() && it->second.entity->type == EntityType::Point;
  }

  GCS::Line& lineRef(const std::string& ref) {
    if (ref == "xaxis") return xaxis_;
    if (ref == "yaxis") return yaxis_;
    auto it = geos_.find(ref);
    if (it == geos_.end() || it->second.entity->type != EntityType::Line) throw BadSketch("not a line: " + ref);
    return it->second.line;
  }

  EntityType kind(const std::string& ref) {
    if (ref == "xaxis" || ref == "yaxis") return EntityType::Line;
    return geo(ref).entity->type;
  }

  GCS::Circle& circleRef(const std::string& ref) {
    auto& g = geos_.at(ref);
    return g.circle;
  }
  GCS::Arc& arcRef(const std::string& ref) { return geos_.at(ref).arc; }

  void need(const Constraint& c, std::size_t n) {
    if (c.refs.size() != n) {
      throw BadSketch(c.id + ": " + c.type + " needs " + std::to_string(n) + " reference(s)");
    }
  }

  void addConstraint(const Constraint& c, int tag) {
    const auto& r = c.refs;
    const std::string& t = c.type;
    double* value = params_.add(c.value, true);  // driving dimension, never solved for

    if (t == "coincident") {
      need(c, 2);
      GCS::Point a = pointRef(r[0]), b = pointRef(r[1]);
      sys_.addConstraintP2PCoincident(a, b, tag);
    } else if (t == "horizontal" || t == "vertical") {
      const bool h = t == "horizontal";
      if (r.size() == 1) {
        GCS::Line& l = lineRef(r[0]);
        h ? sys_.addConstraintHorizontal(l, tag) : sys_.addConstraintVertical(l, tag);
      } else {
        need(c, 2);
        GCS::Point a = pointRef(r[0]), b = pointRef(r[1]);
        h ? sys_.addConstraintHorizontal(a, b, tag) : sys_.addConstraintVertical(a, b, tag);
      }
    } else if (t == "parallel") {
      need(c, 2);
      sys_.addConstraintParallel(lineRef(r[0]), lineRef(r[1]), tag);
    } else if (t == "perpendicular") {
      need(c, 2);
      sys_.addConstraintPerpendicular(lineRef(r[0]), lineRef(r[1]), tag);
    } else if (t == "tangent") {
      need(c, 2);
      addTangent(c, tag);
    } else if (t == "equal") {
      need(c, 2);
      addEqual(c, tag);
    } else if (t == "pointOn") {
      need(c, 2);
      GCS::Point p = pointRef(r[0]);
      switch (kind(r[1])) {
        case EntityType::Line: sys_.addConstraintPointOnLine(p, lineRef(r[1]), tag); break;
        case EntityType::Circle: sys_.addConstraintPointOnCircle(p, circleRef(r[1]), tag); break;
        case EntityType::Arc: sys_.addConstraintPointOnArc(p, arcRef(r[1]), tag); break;
        default: throw BadSketch(c.id + ": pointOn needs a line, circle or arc");
      }
    } else if (t == "midpoint") {
      need(c, 2);
      GCS::Point p = pointRef(r[0]);
      GCS::Line& l = lineRef(r[1]);
      sys_.addConstraintPointOnLine(p, l, tag);
      sys_.addConstraintPointOnPerpBisector(p, l, tag);
    } else if (t == "symmetric") {
      need(c, 3);
      GCS::Point a = pointRef(r[0]), b = pointRef(r[1]);
      if (isPointRef(r[2])) {
        GCS::Point m = pointRef(r[2]);
        sys_.addConstraintP2PSymmetric(a, b, m, tag);
      } else {
        sys_.addConstraintP2PSymmetric(a, b, lineRef(r[2]), tag);
      }
    } else if (t == "distance") {
      if (c.value <= 0) throw BadSketch(c.id + ": distance must be positive");
      if (r.size() == 1) {
        GCS::Line& l = lineRef(r[0]);
        sys_.addConstraintP2PDistance(l.p1, l.p2, value, tag);
      } else {
        need(c, 2);
        GCS::Point a = pointRef(r[0]);
        if (isPointRef(r[1])) {
          GCS::Point b = pointRef(r[1]);
          sys_.addConstraintP2PDistance(a, b, value, tag);
        } else {
          sys_.addConstraintP2LDistance(a, lineRef(r[1]), value, tag);
        }
      }
    } else if (t == "distanceX" || t == "distanceY") {
      const bool x = t == "distanceX";
      GCS::Point a = r.size() == 1 ? origin_ : pointRef(r[0]);
      GCS::Point b = pointRef(r.size() == 1 ? r[0] : r[1]);
      if (r.size() > 2 || r.empty()) throw BadSketch(c.id + ": " + t + " needs 1 or 2 points");
      sys_.addConstraintDifference(x ? a.x : a.y, x ? b.x : b.y, value, tag);
    } else if (t == "angle") {
      need(c, 2);
      *value = c.value * kDeg;
      sys_.addConstraintL2LAngle(lineRef(r[0]), lineRef(r[1]), value, tag);
    } else if (t == "radius" || t == "diameter") {
      need(c, 1);
      if (c.value <= 0) throw BadSketch(c.id + ": " + t + " must be positive");
      const bool d = t == "diameter";
      switch (kind(r[0])) {
        case EntityType::Circle:
          d ? sys_.addConstraintCircleDiameter(circleRef(r[0]), value, tag)
            : sys_.addConstraintCircleRadius(circleRef(r[0]), value, tag);
          break;
        case EntityType::Arc:
          d ? sys_.addConstraintArcDiameter(arcRef(r[0]), value, tag)
            : sys_.addConstraintArcRadius(arcRef(r[0]), value, tag);
          break;
        default: throw BadSketch(c.id + ": " + t + " needs a circle or arc");
      }
    } else if (t == "fix") {
      need(c, 1);
      fixRef(r[0]);
    } else {
      throw BadSketch(c.id + ": unknown constraint type '" + t + "'");
    }
    tagIds_[tag] = c.id;
  }

  GCS::Curve& curveRef(const std::string& id) {
    if (id == "xaxis") return xaxis_;
    if (id == "yaxis") return yaxis_;
    auto it = geos_.find(id);
    if (it == geos_.end()) throw BadSketch("unknown entity: " + id);
    switch (it->second.entity->type) {
      case EntityType::Line: return it->second.line;
      case EntityType::Circle: return it->second.circle;
      case EntityType::Arc: return it->second.arc;
      default: throw BadSketch("not a curve: " + id);
    }
  }

  void addTangent(const Constraint& c, int tag) {
    // Endpoint-to-endpoint tangency ("L1.end", "A1.start"): join the points
    // and lock the angle between the curves there to 0 or 180 degrees,
    // whichever is nearer now. "Line tangent to circle" plus a coincidence
    // would be degenerate at the joint, which is why this form exists.
    if (c.refs[0].find('.') != std::string::npos && c.refs[1].find('.') != std::string::npos) {
      const std::string idA = c.refs[0].substr(0, c.refs[0].find('.'));
      const std::string idB = c.refs[1].substr(0, c.refs[1].find('.'));
      GCS::Point pa = pointRef(c.refs[0]), pb = pointRef(c.refs[1]);
      GCS::Curve& ca = curveRef(idA);
      GCS::Curve& cb = curveRef(idB);
      double* angle = params_.add(0, true);
      *angle = std::cos(sys_.calculateAngleViaPoint(ca, cb, pa)) < 0 ? kPi : 0;
      sys_.addConstraintP2PCoincident(pa, pb, tag);
      sys_.addConstraintAngleViaPoint(ca, cb, pa, angle, tag);
      return;
    }
    std::string a = c.refs[0], b = c.refs[1];
    EntityType ka = kind(a), kb = kind(b);
    // Order so that a line comes first, and a circle before an arc.
    if (kb == EntityType::Line || (ka == EntityType::Arc && kb == EntityType::Circle)) {
      std::swap(a, b);
      std::swap(ka, kb);
    }
    if (ka == EntityType::Line && kb == EntityType::Circle) sys_.addConstraintTangent(lineRef(a), circleRef(b), tag);
    else if (ka == EntityType::Line && kb == EntityType::Arc) sys_.addConstraintTangent(lineRef(a), arcRef(b), tag);
    else if (ka == EntityType::Circle && kb == EntityType::Circle) sys_.addConstraintTangent(circleRef(a), circleRef(b), tag);
    else if (ka == EntityType::Circle && kb == EntityType::Arc) sys_.addConstraintTangent(circleRef(a), arcRef(b), tag);
    else if (ka == EntityType::Arc && kb == EntityType::Arc) sys_.addConstraintTangent(arcRef(a), arcRef(b), tag);
    else throw BadSketch(c.id + ": tangent needs a line, circle or arc pair (not two lines)");
  }

  void addEqual(const Constraint& c, int tag) {
    std::string a = c.refs[0], b = c.refs[1];
    EntityType ka = kind(a), kb = kind(b);
    if (ka == EntityType::Line && kb == EntityType::Line) {
      sys_.addConstraintEqualLength(lineRef(a), lineRef(b), tag);
      return;
    }
    if (ka == EntityType::Arc && kb == EntityType::Circle) {
      std::swap(a, b);
      std::swap(ka, kb);
    }
    if (ka == EntityType::Circle && kb == EntityType::Circle) sys_.addConstraintEqualRadius(circleRef(a), circleRef(b), tag);
    else if (ka == EntityType::Circle && kb == EntityType::Arc) sys_.addConstraintEqualRadius(circleRef(a), arcRef(b), tag);
    else if (ka == EntityType::Arc && kb == EntityType::Arc) sys_.addConstraintEqualRadius(arcRef(a), arcRef(b), tag);
    else throw BadSketch(c.id + ": equal needs two lines or two circles/arcs");
  }

  void fixPoint(const GCS::Point& p) {
    params_.fix(p.x);
    params_.fix(p.y);
  }

  void fixRef(const std::string& ref) {
    if (isPointRef(ref)) {
      fixPoint(pointRef(ref));
      return;
    }
    Geo& g = geos_.at(geo(ref).entity->id);
    switch (g.entity->type) {
      case EntityType::Point: fixPoint(g.point); break;
      case EntityType::Line: fixPoint(g.line.p1); fixPoint(g.line.p2); break;
      case EntityType::Circle: fixPoint(g.circle.center); params_.fix(g.circle.rad); break;
      case EntityType::Arc:
        fixPoint(g.arc.center);
        fixPoint(g.arc.start);
        fixPoint(g.arc.end);
        params_.fix(g.arc.rad);
        params_.fix(g.arc.startAngle);
        params_.fix(g.arc.endAngle);
        break;
    }
  }

  std::vector<std::string> tagsToIds(const GCS::VEC_I& tags) const {
    std::set<std::string> ids;
    for (int t : tags) {
      auto it = tagIds_.find(t);
      if (it != tagIds_.end()) ids.insert(it->second);
    }
    return {ids.begin(), ids.end()};
  }

  void writeBack(Sketch& s) {
    auto v = [](const GCS::Point& p) { return Vec2{*p.x, *p.y}; };
    for (auto& e : s.entities) {
      const Geo& g = geos_.at(e.id);
      switch (e.type) {
        case EntityType::Point: e.a = v(g.point); break;
        case EntityType::Line: e.a = v(g.line.p1); e.b = v(g.line.p2); break;
        case EntityType::Circle: e.center = v(g.circle.center); e.radius = *g.circle.rad; break;
        case EntityType::Arc: {
          double a0 = *g.arc.startAngle, a1 = *g.arc.endAngle;
          normaliseArc(a0, a1);
          e.center = v(g.arc.center);
          e.radius = *g.arc.rad;
          e.startDeg = a0 / kDeg;
          e.endDeg = a1 / kDeg;
          break;
        }
      }
    }
  }

  const Sketch& sketch_;
  Params params_;
  GCS::System sys_;
  GCS::Point origin_;
  GCS::Line xaxis_, yaxis_;
  std::map<std::string, Geo> geos_;
  std::map<int, std::string> tagIds_;
};

}  // namespace

SolveResult solve(const Sketch& sketch) {
  try {
    Builder b(sketch);
    return b.run();
  } catch (const BadSketch& e) {
    SolveResult out;
    out.state = SolveState::Invalid;
    out.sketch = sketch;
    out.message = e.what();
    return out;
  }
}

}  // namespace cadx::sketch
