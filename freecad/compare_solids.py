"""Compare two single-solid FreeCAD documents in their saved coordinate system."""
import FreeCAD as App
import Part
import argparse
from pathlib import Path


def saved_solid(path):
    doc = App.openDocument(str(path.resolve()))
    try:
        # A Body owns its feature tree; use the final top-level shape only.
        roots = [obj.Shape for obj in doc.RootObjects
                 if hasattr(obj, "Shape") and not obj.Shape.isNull()]
        if len(roots) != 1 or len(roots[0].Solids) != 1 or not roots[0].isValid():
            raise ValueError(f"{path} must contain one valid top-level solid")
        return roots[0].Solids[0].copy()
    finally:
        App.closeDocument(doc.Name)


def compare(reference, candidate, tolerance):
    old, new = saved_solid(reference), saved_solid(candidate)
    passed = True
    for label, delta in (("added", new.cut(old)), ("removed", old.cut(new))):
        volume = sum(abs(s.Volume) for s in delta.Solids)
        print(f"{label}: {volume:.9f} mm^3")
        if volume > tolerance:
            passed = False
            for solid in delta.Solids:
                print(f"  {solid.Volume:.9f} mm^3 at {solid.BoundBox}")
    return passed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--tolerance", type=float, default=0.00001,
                        help="allowed volume in each direction in mm^3")
    args = parser.parse_args()
    raise SystemExit(0 if compare(args.reference, args.candidate, args.tolerance) else 1)
