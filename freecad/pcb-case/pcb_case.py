"""Build the Shore Spotter PCB carrier. Dimensions are millimetres."""
import FreeCAD as App
import MeshPart
import Part
from math import cos, pi, sin, sqrt
from pathlib import Path

V = App.Vector
X, Y, Z = V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)
HEIGHT = 50
POST_PITCH = 48.79
POST_RADIUS = 6
POST_BORE = 7
FRONT_Y = -21
SCREW_DIAMETER = 3.5

# The USB board slides between the web and the outer mounting wall.
USB_OUTER_X = POST_PITCH + 36
USB_INNER_X = USB_OUTER_X - 3
USB_WEB_X = USB_INNER_X - 18
USB_Y_MIN, USB_Y_MAX = -18.6, 52.4
USB_CENTER_Y = (FRONT_Y + POST_PITCH + POST_RADIUS) / 2
USB_HOLE_PITCH = 64
USB_EXTENSION = 11
DESIGN_REVISION = "9d489250"


def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))


def bore(center, axis, diameter, depth):
    return Part.makeCylinder(diameter / 2, depth, V(*center), axis)


def round_edges(shape, radius, points):
    """A point inside each straight edge identifies the design corner."""
    edges = []
    for point in points:
        probe = Part.Vertex(V(*point))
        matches = [e for e in shape.Edges if e.distToShape(probe)[0] < 1e-6]
        if len(matches) != 1:
            raise ValueError(f"Expected one edge at {point}, found {len(matches)}")
        edges.append(matches[0])
    return shape.makeFillet(radius, edges)


def window_x(x0, x1, y0, y1, z0, z1):
    blank = box(x0, x1, y0, y1, z0, z1)
    return round_edges(blank, 3, [((x0+x1)/2, y, z)
                                  for y in (y0, y1) for z in (z0, z1)])


def window_y(y0, y1, x0, x1, z0, z1, radius, bottom=True):
    blank = box(x0, x1, y0, y1, z0, z1)
    levels = (z0, z1) if bottom else (z1,)
    return round_edges(blank, radius, [(x, (y0+y1)/2, z)
                                       for x in (x0, x1) for z in levels])


def frame():
    # XY outline of the two board rails, four bosses, and the GPS socket.
    p, r = POST_PITCH, POST_RADIUS
    diagonal = r / sqrt(2)
    rail_end = 105
    gps_radius = 7.5
    gps_y = rail_end + sqrt(gps_radius**2 - (r/2)**2)
    start = (p+r, -r)
    current = start
    edges = []

    def line(x, y):
        nonlocal current
        edges.append(Part.makeLine(V(*current, 0), V(x, y, 0)))
        current = (x, y)

    def arc(mid_x, mid_y, x, y):
        nonlocal current
        edges.append(Part.Arc(V(*current, 0), V(mid_x, mid_y, 0), V(x, y, 0)).toShape())
        current = (x, y)

    line(0, -r)
    line(0, FRONT_Y)
    line(-r, FRONT_Y)
    line(-r, rail_end)
    arc(-r/2, gps_y + gps_radius, 0, rail_end)
    line(0, p+r)
    arc(r, p, 0, p-r)
    line(0, r)
    arc(diagonal, diagonal, r, 0)
    line(p-r, 0)
    arc(p-diagonal, diagonal, p, r)
    arc(p+diagonal, 2*r-diagonal, p+r, 2*r)
    line(p+r, p-2*r)
    arc(p+diagonal, p-2*r+diagonal, p, p-r)
    arc(p-r, p, p, p+r)
    line(USB_OUTER_X, p+r)
    line(USB_OUTER_X, FRONT_Y)
    line(p+r, FRONT_Y)
    line(*start)
    solid = Part.Face(Part.Wire(edges)).extrude(Z * HEIGHT)
    for x in (0, p):
        for y in (0, p):
            solid = solid.cut(bore((x, y, 0), Z, POST_BORE, HEIGHT))
    # The GPS socket leaves a 5 mm floor with a smaller through bore.
    solid = solid.cut(bore((-r/2, gps_y, 0), Z, 4.5, 5))
    return solid.cut(bore((-r/2, gps_y, 5), Z, POST_BORE, HEIGHT-5))


def lilygo_and_servo(solid):
    p, r = POST_PITCH, POST_RADIUS
    sill, lintel = 12, HEIGHT-5
    for y0, y1 in ((r, p-r), (p+r, 96)):
        solid = solid.cut(window_x(-r, 0, y0, y1, 5, lintel))
    for y in (-9, 101):
        for z in (7, HEIGHT-3):
            solid = solid.cut(bore((-r, y, z), X, SCREW_DIAMETER, r))
    solid = round_edges(solid, 3, [(-r, FRONT_Y, HEIGHT/2)])

    # Three sides of the servo board frame and the short return at the front.
    wall = box(3, p+r, FRONT_Y, -r, 0, HEIGHT)
    wall = wall.cut(box(3, 50, FRONT_Y, -r, sill, lintel))
    for z0, z1 in ((0, sill), (lintel, HEIGHT)):
        wall = wall.fuse(box(0, 3, FRONT_Y, -12.5, z0, z1))
    for z in (7, HEIGHT-3):
        wall = wall.cut(bore((3, -9, z), X, SCREW_DIAMETER, 4))
    solid = solid.fuse(wall).removeSplitter()
    solid = solid.cut(window_y(-r, 0, r, p-r, sill, lintel, 3))
    solid = solid.cut(window_y(-r, -r+2, 45, 50, 22, 36, 0.5))
    solid = solid.cut(window_y(FRONT_Y, 0, 5.5, p-5.5, 0, 8.75, 3, bottom=False))

    # Tool access to the servo screw has rounded lips at both elevations.
    y0, y1 = p/2-10, p/2+10
    solid = solid.cut(box(p, p+20, y0, y1, HEIGHT-10, HEIGHT))
    return round_edges(solid, 3, [(p+12, y, z) for y in (y0, y1)
                                  for z in (HEIGHT-10, HEIGHT)])


def nut_trap(x, y, z, radius, depth):
    corners = [V(x, y+radius*cos(i*pi/3), z+radius*sin(i*pi/3)) for i in range(6)]
    return Part.Face(Part.makePolygon(corners + corners[:1])).extrude(-X * depth)


def usb_mount(solid):
    p, r = POST_PITCH, POST_RADIUS
    outer, inner, web = USB_OUTER_X, USB_INNER_X, USB_WEB_X
    y0, y1 = USB_Y_MIN, USB_Y_MAX
    front, rear = FRONT_Y, p+r
    foot = -USB_EXTENSION
    solid = solid.cut(box(web, inner, y0, y1, 0, HEIGHT))
    lip = box(inner-4, outer, front, rear, foot, 0)
    lip = lip.cut(box(inner-4, inner, y0, y1, foot, 0))
    solid = solid.fuse(lip).removeSplitter()
    solid = round_edges(solid, 3, [(inner-4, (front+y0)/2, 0),
                                  (inner-4, (rear+y1)/2, 0)])
    solid = solid.cut(box(web+3, inner-3, front, y0, 20, HEIGHT))
    solid = solid.cut(window_x(p, web, r, p-r, 5, 35))

    for side in (-1, 1):
        y = USB_CENTER_Y + side * USB_HOLE_PITCH/2
        pad_y0, pad_y1 = (y0, y+3) if side < 0 else (y-3, y1)
        for z, bottom, top in ((foot+4, foot, foot+7), (HEIGHT-3, HEIGHT-6, HEIGHT)):
            solid = solid.fuse(box(inner-2, inner, pad_y0, pad_y1, bottom, top))
            solid = solid.cut(bore((inner-2, y, z), X, SCREW_DIAMETER, outer-inner+2))
        solid = solid.cut(nut_trap(outer, y, foot+4, 3, 1.5))
    solid = solid.removeSplitter()
    return round_edges(solid, 3, [(outer, front, 10), (outer, rear, 10)])


def engrave_revision(solid):
    # Viewed from the open side, -Y points right and +Z points up.
    from matplotlib import get_data_path
    font = Path(get_data_path()) / "fonts/ttf/DejaVuSansMono-Bold.ttf"
    glyphs = Part.makeWireString(DESIGN_REVISION, str(font.parent) + "/",
                                font.name, 5.0)
    faces = [Part.makeFace(wires, "Part::FaceMakerBullseye") for wires in glyphs]
    if len(faces) != len(DESIGN_REVISION):
        raise ValueError("Font did not produce the complete revision")
    text = Part.makeCompound(faces)
    bounds = text.BoundBox
    text.translate(V(-bounds.Center.x, -bounds.Center.y, 0))
    text.Placement = App.Placement(V(), App.Rotation(-Y, Z, -X, "ZXY")) * text.Placement
    text.translate(V(USB_INNER_X, USB_CENTER_Y, -USB_EXTENSION/2))
    return solid.cut(text.extrude(X * 0.5)).removeSplitter()


def build():
    shape = engrave_revision(usb_mount(lilygo_and_servo(frame())))
    if len(shape.Solids) != 1 or not shape.isValid():
        raise ValueError("PCB case must be one valid solid")
    return shape.Solids[0]


def export(directory):
    shape = build()
    mesh = MeshPart.meshFromShape(Shape=shape, LinearDeflection=0.05,
                                  AngularDeflection=0.15, Relative=False)
    if not mesh.isSolid():
        raise ValueError("STL mesh is not closed")
    directory.mkdir(parents=True, exist_ok=True)
    doc = App.newDocument("PCBCase")
    try:
        doc.addObject("Part::Feature", "PCBCase").Shape = shape
        doc.recompute()
        doc.saveAs(str(directory / "pcb_case.FCStd"))
        mesh.write(str(directory / "pcb_case.stl"))
    finally:
        App.closeDocument(doc.Name)


if __name__ == "__main__":
    export(Path(__file__).resolve().parent / "build")
