"""Build the GPS antenna tray in millimetres, centred on its mounting bore."""
import FreeCAD as App
import MeshPart
import Part
from pathlib import Path

V = App.Vector
WIDTH, LENGTH, HEIGHT = 57.0, 62.0, 10.0
RIM_WIDTH, SEAT_DEPTH = 3.0, 3.0
NOTCH_WIDTH = 3.0
BORE_DIAMETER, COUNTERBORE_DIAMETER, COUNTERBORE_DEPTH = 4.5, 10.0, 4.0
POCKET_DEPTH, POCKET_WIDTH = 4.0, 39.0
POCKET_END_INSET, HUB_RADIUS = 5.0, 10.0
CASE_FILLET, POCKET_FILLET = 2.0, 2.0


def centered_box(width, length, base, height):
    return Part.makeBox(width, length, height, V(-width/2, -length/2, base))


def floor_pocket(seat_z):
    """North pocket: 45-degree flanks meet the circular central land."""
    end_y = LENGTH/2 - RIM_WIDTH - POCKET_END_INSET
    half_width = POCKET_WIDTH/2
    base = seat_z - POCKET_DEPTH
    left, right = V(-half_width, end_y, base), V(half_width, end_y, base)
    apex = V(0, end_y - half_width, base)
    outline = Part.makePolygon([left, right, apex, left])
    pocket = Part.Face(outline).extrude(V(0, 0, POCKET_DEPTH))
    # Round the two outer corners before trimming the tip around the hub.
    outer_edges = [edge for edge in pocket.Edges
                   if edge.BoundBox.ZLength > 0 and abs(edge.CenterOfMass.y - end_y) < 1e-7]
    pocket = pocket.makeFillet(POCKET_FILLET, outer_edges)
    hub = Part.makeCylinder(HUB_RADIUS, POCKET_DEPTH, V(0, 0, base))
    return pocket.cut(hub)


def build():
    tray = centered_box(WIDTH, LENGTH, 0, HEIGHT)
    # Blend the four upright corners and the underside perimeter together.
    lower_edges = [edge for edge in tray.Edges if edge.BoundBox.ZMin < 1e-7]
    tray = tray.makeFillet(CASE_FILLET, lower_edges)

    seat_z = HEIGHT - SEAT_DEPTH
    seat = centered_box(WIDTH - 2*RIM_WIDTH, LENGTH - 2*RIM_WIDTH, seat_z, SEAT_DEPTH)
    # The slot across the seat opens just the centres of the two end walls.
    notches = centered_box(NOTCH_WIDTH, LENGTH, seat_z, SEAT_DEPTH)
    tray = tray.cut(seat.fuse(notches))
    tray = tray.cut(Part.makeCylinder(BORE_DIAMETER/2, seat_z))
    counterbore = Part.makeCylinder(COUNTERBORE_DIAMETER/2, COUNTERBORE_DEPTH,
                                    V(0, 0, seat_z - COUNTERBORE_DEPTH))
    tray = tray.cut(counterbore)
    pocket = floor_pocket(seat_z)
    for angle in (0, 90, 180, 270):
        tray = tray.cut(pocket.rotated(V(), V(0, 0, 1), angle))
    tray = tray.removeSplitter()
    if len(tray.Solids) != 1 or not tray.isValid():
        raise ValueError("Expected one valid antenna tray solid")
    return tray.Solids[0]


def export(directory):
    tray = build()
    mesh = MeshPart.meshFromShape(Shape=tray, LinearDeflection=0.05,
                                  AngularDeflection=0.15, Relative=False)
    if not mesh.isSolid() or mesh.countComponents() != 1:
        raise ValueError("Expected one closed antenna tray mesh")
    directory.mkdir(parents=True, exist_ok=True)
    doc = App.newDocument("GPSAntennaCase")
    try:
        doc.addObject("Part::Feature", "GPSAntennaCase").Shape = tray
        doc.recompute()
        doc.saveAs(str(directory / "gps_antenna_case.FCStd"))
        mesh.write(str(directory / "gps_antenna_case.stl"))
    finally:
        App.closeDocument(doc.Name)


if __name__ == "__main__":
    export(Path(__file__).resolve().parent / "build")
