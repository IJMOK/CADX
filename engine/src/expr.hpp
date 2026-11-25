#pragma once
// Parameters and expressions.
//
// Any numeric input in a document (feature distances, constraint values, ...)
// may be a number or a string expression such as "width / 2 + 3". Expressions
// use + - * / ^, parentheses, unary minus, the constants pi and e, functions
// sin cos tan asin acos atan atan2 sqrt abs min max floor ceil round, and the
// names of document parameters. Trigonometry works in degrees, like the rest
// of CADX.
//
// Document JSON:  "parameters": [ { "name": "width", "expression": "40" }, ... ]
// Parameters may refer to each other in any order; cycles are an error.

#include <nlohmann/json.hpp>

#include <map>
#include <stdexcept>
#include <string>

namespace cadx {

struct ExprError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using ParamValues = std::map<std::string, double>;

// Evaluate one expression. Throws ExprError.
double evaluate(const std::string& expression, const ParamValues& params);

// Evaluate every parameter of a document. Throws ExprError naming the parameter.
ParamValues evaluateParameters(const nlohmann::json& parameters);

// A copy of `document` with every string in a numeric field replaced by its
// value. `errors` collects "<where>: <message>" for expressions that fail;
// those fields are left as strings so the feature using them fails clearly.
nlohmann::json resolveExpressions(const nlohmann::json& document, const ParamValues& params,
                                  std::vector<std::string>* errors = nullptr);

bool isValidParameterName(const std::string& name);

}  // namespace cadx
