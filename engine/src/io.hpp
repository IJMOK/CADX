#pragma once
// Exchange formats and measurements. Paths are UTF-8.

#include <TopoDS_Shape.hxx>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace cadx {

struct IoError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void exportStep(const TopoDS_Shape& shape, const std::string& path, const std::string& name);
void exportIges(const TopoDS_Shape& shape, const std::string& path);
void exportStl(const TopoDS_Shape& shape, const std::string& path, double deflection);
void exportBrep(const TopoDS_Shape& shape, const std::string& path);

// Reads STEP, IGES or BREP (by extension) and returns the shape.
TopoDS_Shape importShape(const std::string& path);

// OCCT BRep text, for storing imported geometry inside a document.
std::string toBrepText(const TopoDS_Shape& shape);

// Volume, area, centre of mass, bounding box, counts.
nlohmann::json measure(const TopoDS_Shape& shape);

}  // namespace cadx
