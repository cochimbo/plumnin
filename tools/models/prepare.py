#!/usr/bin/env python3
"""Makes plumnin's console and cartridge models (assets/models/*.glb) out of the
downloaded Sketchfab ones (see assets/models/CREDITS.txt for each source).

    python3 tools/models/prepare.py SOURCE_FOLDER assets/models

SOURCE_FOLDER has the files as Sketchfab gives them (their names in RECIPES). Each model
keeps only the meshes plumnin shows (a cartridge goes in its own file, the NES's game
sleeve is left out), its textures are shrunk and saved as JPEG (PNG if they have
transparency), and the metallic/roughness, occlusion and emission textures are dropped (and
the normal maps on other texture coordinates than the first), as
plumsg does with its models: the shelf does not use them. Needs Pillow.
"""
import io
import json
import re
import struct
import sys

from PIL import Image

# output file: (source file, meshes kept (regular expressions on the node names, None: all),
# meshes dropped, largest texture side[, options]). The options:
#   swap: {material: the one to use instead}
#   cut: {material: ((x0, y0, z0), (x1, y1, z1))}, the triangles of the material in that box
#        (the mesh's own coordinates, by their middles) left out
#   tint: {material: [r, g, b, a]}, its base colour factor
RECIPES = {
    "gameboy.glb": ("game_boy_original_1989.glb", None, [r"^cartridge_", r"^chip_"], 1024),
    # The cartridge in the slot, straight, with the grey Tetris one's look lying beside it
    # (the one in the slot is dark blue).
    "gameboy-cartridge.glb": ("game_boy_original_1989.glb", [r"^cartridge_low_Cartridge_0$"], [], 1024,
                              {"swap": {"Cartridge": "Cartridge_2"}}),
    # A Game Boy Color game's: the same cartridge in black, as theirs are. The Game Boy
    # Color model has one in its slot, but only the half that shows (the top, sticking out),
    # so that one is cut out of it, leaving the slot empty for the game's.
    "gameboycolor-cartridge.glb": ("game_boy_original_1989.glb", [r"^cartridge_low_Cartridge_0$"], [], 1024,
                                   {"swap": {"Cartridge": "Cartridge_2"}, "tint": {"Cartridge_2": [0.3, 0.3, 0.32, 1]}}),
    "gameboycolor.glb": ("game_boy_color.glb", None, [], 1024,
                         {"cut": {"Plastic": ((-0.46, -0.2, 0.32), (0.57, -0.04, 0.9))}}),
    # The NES with its pad and cable; the cartridge lying beside it (its shell and labels,
    # not the board inside) on its own; the sleeve left out.
    "nes.glb": ("nes_og-85.glb", None, [r"^Object_(7|9|10|11|12|13|14)$"], 1024),
    "nes-cartridge.glb": ("nes_og-85.glb", [r"^Object_(10|13|14)$"], [], 1024),
    "snes.glb": ("super_nintendo.glb", None, [], 1024),
}


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2, path
    pos, doc, binary = 12, None, b""
    while pos < len(data):
        length, kind = struct.unpack_from("<II", data, pos)
        chunk = data[pos + 8:pos + 8 + length]
        if kind == 0x4E4F534A:
            doc = json.loads(chunk)
        elif kind == 0x004E4942:
            binary = chunk
        pos += 8 + length
    return doc, binary


def write_glb(path, doc, binary):
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    binary += b"\0" * (-len(binary) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(binary), 0x004E4942) + binary)


def shrink(image_bytes, largest):
    picture = Image.open(io.BytesIO(image_bytes))
    picture.load()
    alpha = picture.mode in ("RGBA", "LA", "P") and picture.convert("RGBA").getchannel("A").getextrema()[0] < 250
    if max(picture.size) > largest:
        k = largest / max(picture.size)
        picture = picture.resize((max(1, round(picture.width * k)), max(1, round(picture.height * k))), Image.LANCZOS)
    out = io.BytesIO()
    if alpha:
        picture.convert("RGBA").save(out, "PNG", optimize=True)
        return out.getvalue(), "image/png"
    picture.convert("RGB").save(out, "JPEG", quality=85, optimize=True)
    return out.getvalue(), "image/jpeg"


def read_accessor(doc, binary, index):
    a = doc["accessors"][index]
    view = doc["bufferViews"][a["bufferView"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    fmt = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[a["componentType"]]
    size = struct.calcsize(fmt) * n
    stride = view.get("byteStride", size)
    start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    return [struct.unpack_from("<" + fmt * n, binary, start + i * stride) for i in range(a["count"])]


def cut(doc, binary, material, low, high):
    """Leaves out the material's triangles whose middles are in the box low..high: new
    indices for them, at the end of the binary (returned)."""
    names = [m.get("name") for m in doc.get("materials", [])]
    for mesh in doc["meshes"]:
        for p in mesh["primitives"]:
            if "material" not in p or names[p["material"]] != material:
                continue
            positions = read_accessor(doc, binary, p["attributes"]["POSITION"])
            indices = [i[0] for i in read_accessor(doc, binary, p["indices"])] if "indices" in p else list(range(len(positions)))
            kept = []
            for t in range(0, len(indices) - 2, 3):
                tri = indices[t:t + 3]
                mid = [sum(positions[v][k] for v in tri) / 3 for k in range(3)]
                if not all(low[k] <= mid[k] <= high[k] for k in range(3)):
                    kept += tri
            binary += b"\0" * (-len(binary) % 4)
            data = struct.pack("<%dI" % len(kept), *kept)
            doc["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)})
            doc["accessors"].append({"bufferView": len(doc["bufferViews"]) - 1, "componentType": 5125, "count": len(kept), "type": "SCALAR"})
            p["indices"] = len(doc["accessors"]) - 1
            binary += data
    return binary


def make(source, keep, drop, largest, target, options):
    doc, binary = read_glb(source)
    names = [m.get("name") for m in doc.get("materials", [])]
    swap = options.get("swap", {})
    for mesh in doc["meshes"]:
        for p in mesh["primitives"]:
            if "material" in p and names[p["material"]] in swap:
                p["material"] = names.index(swap[names[p["material"]]])
    for material, (low, high) in options.get("cut", {}).items():
        binary = cut(doc, binary, material, low, high)
    for material, factor in options.get("tint", {}).items():
        doc["materials"][names.index(material)].setdefault("pbrMetallicRoughness", {})["baseColorFactor"] = factor
    wanted = lambda name: (keep is None or any(re.search(k, name) for k in keep)) and not any(re.search(d, name) for d in drop)
    # Meshes off the nodes not kept (the nodes themselves stay, for their children's places).
    for node in doc["nodes"]:
        if "mesh" in node and not wanted(node.get("name", "")):
            del node["mesh"]
    for material in doc.get("materials", []):
        material.get("pbrMetallicRoughness", {}).pop("metallicRoughnessTexture", None)
        material.pop("occlusionTexture", None)
        material.pop("emissiveTexture", None)
        # The shelf reads only the first texture coordinates: a normal map on others goes.
        if material.get("normalTexture", {}).get("texCoord", 0) != 0:
            del material["normalTexture"]

    # What is still used, renumbered in order.
    def renumber(items, used):
        order = sorted(used)
        return [items[i] for i in order], {old: new for new, old in enumerate(order)}

    mesh_used = {n["mesh"] for n in doc["nodes"] if "mesh" in n}
    meshes, mesh_map = renumber(doc["meshes"], mesh_used)
    for n in doc["nodes"]:
        if "mesh" in n:
            n["mesh"] = mesh_map[n["mesh"]]
    accessor_used, material_used = set(), set()
    for m in meshes:
        for p in m["primitives"]:
            p.pop("targets", None)
            # Only what plumnin reads: no tangents, no second texture coordinates.
            p["attributes"] = {k: v for k, v in p["attributes"].items() if k in ("POSITION", "NORMAL", "TEXCOORD_0", "COLOR_0")}
            accessor_used.update(p["attributes"].values())
            if "indices" in p:
                accessor_used.add(p["indices"])
            if "material" in p:
                material_used.add(p["material"])
    materials, material_map = renumber(doc.get("materials", []), material_used)
    accessors, accessor_map = renumber(doc["accessors"], accessor_used)
    for m in meshes:
        for p in m["primitives"]:
            p["attributes"] = {k: accessor_map[v] for k, v in p["attributes"].items()}
            if "indices" in p:
                p["indices"] = accessor_map[p["indices"]]
            if "material" in p:
                p["material"] = material_map[p["material"]]
    texture_refs = []
    for mat in materials:
        for holder, key in ((mat.get("pbrMetallicRoughness", {}), "baseColorTexture"), (mat, "normalTexture")):
            if key in holder:
                texture_refs.append(holder[key])
    textures, texture_map = renumber(doc.get("textures", []), {r["index"] for r in texture_refs})
    for r in texture_refs:
        r["index"] = texture_map[r["index"]]
    images, image_map = renumber(doc.get("images", []), {t["source"] for t in textures})
    for t in textures:
        t["source"] = image_map[t["source"]]

    # A new buffer with only the views still used, the images shrunk.
    views = doc["bufferViews"]
    out, new_views, view_map = bytearray(), [], {}

    def add_view(old, data=None):
        view = dict(views[old])
        if data is None:
            start = view.get("byteOffset", 0)
            data = binary[start:start + view["byteLength"]]
        out.extend(b"\0" * (-len(out) % 4))
        view["byteOffset"], view["byteLength"], view["buffer"] = len(out), len(data), 0
        out.extend(data)
        new_views.append(view)
        return len(new_views) - 1

    # Each accessor gets a tightly packed view of its own: the files interleave many
    # meshes in one view, which would keep the dropped ones' data.
    components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}
    sizes = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
    for a in accessors:
        a.pop("sparse", None)
        if "bufferView" not in a:
            continue
        view = views[a["bufferView"]]
        element = components[a["type"]] * sizes[a["componentType"]]
        stride = view.get("byteStride", element)
        start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        data = b"".join(binary[start + i * stride:start + i * stride + element] for i in range(a["count"]))
        index = add_view(a["bufferView"], data)
        new_views[index].pop("byteStride", None)
        a["bufferView"], a["byteOffset"] = index, 0
    for im in images:
        view = views[im["bufferView"]]
        start = view.get("byteOffset", 0)
        data, mime = shrink(binary[start:start + view["byteLength"]], largest)
        new_views.append({"buffer": 0, "byteOffset": 0, "byteLength": 0})
        index = len(new_views) - 1
        out.extend(b"\0" * (-len(out) % 4))
        new_views[index].update(byteOffset=len(out), byteLength=len(data))
        out.extend(data)
        im["bufferView"], im["mimeType"] = index, mime
        im.pop("name", None)

    doc.update(meshes=meshes, materials=materials, accessors=accessors, textures=textures, images=images, bufferViews=new_views)
    doc["buffers"] = [{"byteLength": len(out)}]
    doc.pop("animations", None)
    doc.pop("skins", None)
    for key in ("materials", "textures", "images", "samplers"):
        if not doc.get(key):
            doc.pop(key, None)
    write_glb(target, doc, bytes(out))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, target = sys.argv[1].rstrip("/\\"), sys.argv[2].rstrip("/\\")
    for name, (file, keep, drop, largest, *options) in RECIPES.items():
        make(f"{source}/{file}", keep, drop, largest, f"{target}/{name}", options[0] if options else {})
        print(name)


if __name__ == "__main__":
    main()
