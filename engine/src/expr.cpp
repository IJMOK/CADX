#include "expr.hpp"

#include <cctype>
#include <cmath>
#include <functional>
#include <numbers>
#include <set>
#include <vector>

using nlohmann::json;

namespace cadx {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

// Recursive-descent parser. Grammar:
//   expr   := term (('+'|'-') term)*
//   term   := unary (('*'|'/') unary)*
//   unary  := ('-'|'+') unary | power
//   power  := atom ('^' unary)?
//   atom   := number | name | name '(' args ')' | '(' expr ')'
class Parser {
 public:
  using Lookup = std::function<double(const std::string&)>;

  Parser(const std::string& text, Lookup lookup) : s_(text), lookup_(std::move(lookup)) {}

  double parse() {
    const double v = expr();
    skip();
    if (pos_ != s_.size()) fail(std::string("unexpected '") + s_[pos_] + "'");
    return v;
  }

 private:
  [[noreturn]] void fail(const std::string& what) const {
    throw ExprError(what + " in \"" + s_ + "\"");
  }
  void skip() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
  }
  bool eat(char c) {
    skip();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  double expr() {
    double v = term();
    for (;;) {
      if (eat('+')) v += term();
      else if (eat('-')) v -= term();
      else return v;
    }
  }
  double term() {
    double v = unary();
    for (;;) {
      if (eat('*')) v *= unary();
      else if (eat('/')) {
        const double d = unary();
        if (d == 0) fail("division by zero");
        v /= d;
      } else return v;
    }
  }
  double unary() {
    if (eat('-')) return -unary();
    if (eat('+')) return unary();
    return power();
  }
  double power() {
    const double base = atom();
    if (eat('^')) return std::pow(base, unary());
    return base;
  }
  double atom() {
    skip();
    if (pos_ >= s_.size()) fail("unexpected end");
    const char c = s_[pos_];
    if (c == '(') {
      ++pos_;
      const double v = expr();
      if (!eat(')')) fail("missing ')'");
      return v;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      std::size_t used = 0;
      double v = 0;
      try {
        v = std::stod(s_.substr(pos_), &used);
      } catch (const std::exception&) {
        fail("bad number");
      }
      pos_ += used;
      return v;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const std::size_t start = pos_;
      while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
      const std::string name = s_.substr(start, pos_ - start);
      if (eat('(')) {
        std::vector<double> args;
        if (!eat(')')) {
          do args.push_back(expr());
          while (eat(','));
          if (!eat(')')) fail("missing ')' after arguments of " + name);
        }
        return call(name, args);
      }
      if (name == "pi") return std::numbers::pi;
      if (name == "e") return std::numbers::e;
      return lookup_(name);
    }
    fail(std::string("unexpected '") + c + "'");
  }

  double call(const std::string& f, const std::vector<double>& a) {
    auto need = [&](std::size_t n) {
      if (a.size() != n) fail(f + " takes " + std::to_string(n) + " argument(s)");
    };
    if (f == "sin") { need(1); return std::sin(a[0] * kDeg); }
    if (f == "cos") { need(1); return std::cos(a[0] * kDeg); }
    if (f == "tan") { need(1); return std::tan(a[0] * kDeg); }
    if (f == "asin") { need(1); return std::asin(a[0]) / kDeg; }
    if (f == "acos") { need(1); return std::acos(a[0]) / kDeg; }
    if (f == "atan") { need(1); return std::atan(a[0]) / kDeg; }
    if (f == "atan2") { need(2); return std::atan2(a[0], a[1]) / kDeg; }
    if (f == "sqrt") {
      need(1);
      if (a[0] < 0) fail("sqrt of a negative number");
      return std::sqrt(a[0]);
    }
    if (f == "abs") { need(1); return std::abs(a[0]); }
    if (f == "floor") { need(1); return std::floor(a[0]); }
    if (f == "ceil") { need(1); return std::ceil(a[0]); }
    if (f == "round") { need(1); return std::round(a[0]); }
    if (f == "min" || f == "max") {
      if (a.empty()) fail(f + " needs at least one argument");
      double v = a[0];
      for (double x : a) v = f == "min" ? std::min(v, x) : std::max(v, x);
      return v;
    }
    fail("unknown function " + f);
  }

  const std::string& s_;
  Lookup lookup_;
  std::size_t pos_ = 0;
};

// Fields that hold numbers and may be written as expressions.
const std::set<std::string>& numericKeys() {
  static const std::set<std::string> keys{
      "distance", "distance2", "radius", "value", "angle", "thickness", "diameter", "depth",
      "count", "count2", "spacing", "spacing2", "offset", "x", "y", "z", "height", "r", "r1", "r2",
      "cbDiameter", "cbDepth", "csDiameter", "csAngle", "position", "length", "width"};
  return keys;
}

void resolveIn(json& j, const ParamValues& params, const std::string& where, std::vector<std::string>* errors) {
  if (j.is_object()) {
    // Errors name the object they are in: "features/Extrude1.distance: ...".
    const std::string here = j.contains("id") && j.at("id").is_string() ? where + "/" + j.at("id").get<std::string>() : where;
    auto eval = [&](json& v, const std::string& key) {
      if (!v.is_string()) return;
      try {
        v = evaluate(v.get<std::string>(), params);
      } catch (const ExprError& e) {
        if (errors) errors->push_back(here + "." + key + ": " + e.what());
      }
    };
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (numericKeys().count(it.key())) {
        if (it->is_array()) {
          for (auto& x : *it) eval(x, it.key());
        } else {
          eval(*it, it.key());
        }
      } else if (it->is_object() || it->is_array()) {
        resolveIn(*it, params, here, errors);
      }
    }
  } else if (j.is_array()) {
    for (auto& x : j) resolveIn(x, params, where, errors);
  }
}

}  // namespace

bool isValidParameterName(const std::string& name) {
  if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return name != "pi" && name != "e";
}

double evaluate(const std::string& expression, const ParamValues& params) {
  Parser p(expression, [&](const std::string& name) {
    auto it = params.find(name);
    if (it == params.end()) throw ExprError("unknown name '" + name + "' in \"" + expression + "\"");
    return it->second;
  });
  const double v = p.parse();
  if (!std::isfinite(v)) throw ExprError("\"" + expression + "\" is not a finite number");
  return v;
}

ParamValues evaluateParameters(const json& parameters) {
  std::map<std::string, std::string> exprs;
  std::vector<std::string> order;
  if (parameters.is_array()) {
    for (const auto& p : parameters) {
      const std::string name = p.at("name").get<std::string>();
      if (!isValidParameterName(name)) throw ExprError("invalid parameter name '" + name + "'");
      if (exprs.count(name)) throw ExprError("duplicate parameter '" + name + "'");
      const json& e = p.contains("expression") ? p.at("expression") : p.at("value");
      exprs[name] = e.is_string() ? e.get<std::string>() : e.dump();
      order.push_back(name);
    }
  }

  ParamValues values;
  std::set<std::string> visiting;
  std::function<double(const std::string&)> valueOf = [&](const std::string& name) -> double {
    if (auto it = values.find(name); it != values.end()) return it->second;
    auto e = exprs.find(name);
    if (e == exprs.end()) throw ExprError("unknown name '" + name + "'");
    if (visiting.count(name)) throw ExprError("parameter '" + name + "' refers to itself");
    visiting.insert(name);
    double v = 0;
    try {
      Parser p(e->second, valueOf);
      v = p.parse();
    } catch (const ExprError& err) {
      const std::string msg = err.what();
      if (msg.rfind("parameter '", 0) == 0) throw;
      throw ExprError("parameter '" + name + "': " + msg);
    }
    visiting.erase(name);
    values[name] = v;
    return v;
  };
  for (const auto& name : order) valueOf(name);
  return values;
}

json resolveExpressions(const json& document, const ParamValues& params, std::vector<std::string>* errors) {
  json out = document;
  if (out.contains("sketches")) resolveIn(out["sketches"], params, "sketches", errors);
  if (out.contains("features")) resolveIn(out["features"], params, "features", errors);
  return out;
}

}  // namespace cadx
