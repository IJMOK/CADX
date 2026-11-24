# PlaneGCS (vendored)

The 2D geometric constraint solver from FreeCAD's Sketcher, copied unmodified from
[FreeCAD 1.0.0](https://github.com/FreeCAD/FreeCAD/tree/1.0.0/src/Mod/Sketcher/App/planegcs)
into `App/planegcs/`.

- Licence: GNU LGPL v2.1 or later (see the headers of each file). CADX links it as a separate
  static library and only talks to it through `engine/src/sketch/` — nothing else in CADX includes
  PlaneGCS headers, so it can be swapped for another solver.
- `SketcherGlobal.h` and `shim/` replace the few FreeCAD headers PlaneGCS includes
  (export macro, config header, `Base::Console`, a Boost wrapper).
- Needs Eigen 3.3+ and Boost (header-only: graph, math).

To update: copy the files from a newer FreeCAD tag into `App/planegcs/` and rebuild.
