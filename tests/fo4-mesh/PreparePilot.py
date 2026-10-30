"""Prepare a private FO4 mesh pilot by changing one existing shape NAME only.

Reads an extracted NIF, never an installed archive. Geometry payloads, native
LOD counts, materials, collision, block references and all other blocks stay
identical. The fixture must be installed only by the private benchmark harness.
Local primary schema: NifSkope 2018-02-22 nif.xml, BSTriShape/BSMeshLODTriShape.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, count):
        if count < 0 or count > len(self.data) - self.pos:
            raise ValueError("truncated NIF")
        result = self.data[self.pos:self.pos + count]
        self.pos += count
        return result

    def get(self, fmt):
        values = struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))
        return values[0] if len(values) == 1 else values

    def string(self):
        return self.take(self.get("I"))


class Nif:
    def __init__(self, data):
        r = Reader(data)
        end = data.find(b"\n", 0, 100)
        if end < 0:
            raise ValueError("missing NIF signature")
        r.take(end + 1)
        version, endian, user, count, bethesda = r.get("IBIII")
        if (version, endian, user, bethesda) != (0x14020007, 1, 12, 130) or count > 100000:
            raise ValueError("requires native Fallout 4 Bethesda130 NIF")
        for _ in range(4):
            r.take(r.get("B"))
        self.prefix = data[:r.pos]
        self.types = [r.string() for _ in range(r.get("H"))]
        self.ids = [r.get("H") for _ in range(count)]
        if any(i >= len(self.types) for i in self.ids):
            raise ValueError("invalid type reference")
        sizes = [r.get("I") for _ in range(count)]
        strings, self.max_string = r.get("II")
        self.strings = [r.string() for _ in range(strings)]
        if self.max_string != max(map(len, self.strings), default=0):
            raise ValueError("noncanonical string maximum")
        self.groups = [r.get("I") for _ in range(r.get("I"))]
        self.blocks = [r.take(size) for size in sizes]
        self.footer = r.take(len(data) - r.pos)
        footer = Reader(self.footer)
        roots = [footer.get("i") for _ in range(footer.get("I"))]
        if footer.pos != len(self.footer) or any(i < -1 or i >= count for i in roots):
            raise ValueError("invalid roots/footer")

    def serialize(self):
        parts = [self.prefix, struct.pack("<H", len(self.types))]
        for kind in self.types:
            parts.extend((struct.pack("<I", len(kind)), kind))
        parts.extend((struct.pack("<" + "H" * len(self.ids), *self.ids),
                      struct.pack("<" + "I" * len(self.blocks), *map(len, self.blocks)),
                      struct.pack("<II", len(self.strings), max(map(len, self.strings), default=0))))
        for text in self.strings:
            parts.extend((struct.pack("<I", len(text)), text))
        parts.extend((struct.pack("<I", len(self.groups)), struct.pack("<" + "I" * len(self.groups), *self.groups),
                      *self.blocks, self.footer))
        return b"".join(parts)

    def mesh(self, block):
        if self.types[self.ids[block]] != b"BSMeshLODTriShape":
            raise ValueError("selected block must already be native BSMeshLODTriShape")
        r = Reader(self.blocks[block])
        name = r.get("i")
        if not 0 <= name < len(self.strings):
            raise ValueError("invalid geometry name")
        extras = r.get("I")
        if extras:
            raise ValueError("extra-data-dependent selected shape excluded")
        if r.get("i") != -1:
            raise ValueError("controlled selected shape excluded")
        flags = r.get("I")
        transform = r.take(52)
        collision = r.get("i")
        bound = r.take(16)
        reference_offset = r.pos
        skin, shader, alpha, descriptor, triangles, vertices, size = r.get("iiiQIHI")
        stride = (descriptor & 15) * 4
        if skin != -1 or descriptor & (1 << 50) or not descriptor & (1 << 44) or not stride or not vertices or not triangles:
            raise ValueError("selected shape is not rigid embedded vertex data")
        if size != vertices * stride + triangles * 6:
            raise ValueError("unexpected embedded payload size")
        vertex_data = r.take(vertices * stride)
        index_data = r.take(triangles * 6)
        counts = r.get("III")
        if r.pos != len(r.data) or sum(counts) != triangles or not counts[0] or counts[0] == triangles:
            raise ValueError("requires nonempty native full and reduced cumulative prefixes")
        indices = struct.unpack("<" + "H" * (triangles * 3), index_data)
        if max(indices) >= vertices:
            raise ValueError("native index exceeds vertices")
        prefixes = []
        for n in (3, 2, 1):
            count = sum(counts[:n]) * 3
            maximum = max(indices[:count]) + 1 if count else 0
            prefixes.append(dict(mode=n, index_count=count, vertex_prefix=maximum,
                                 logical_bytes=((maximum * stride + 3) & ~3) + ((count * 2 + 3) & ~3)))
        return dict(name=self.strings[name].decode("ascii"), name_index=name, flags=flags, collision=collision,
                    descriptor=descriptor, vertices=vertices, triangles=triangles, counts=list(counts), stride=stride,
                    vertex_data=vertex_data, index_data=index_data, prefixes=prefixes,
                    skin=skin, shader=shader, alpha=alpha, reference_offset=reference_offset)


def prune_rigid_dependency_closure(nif, block):
    selected = nif.mesh(block)
    if selected["collision"] != -1 or selected["alpha"] != -1:
        raise ValueError("pruned fixture requires no selected shape collision/alpha")
    shader_id = selected["shader"]
    if shader_id < 0 or nif.types[nif.ids[shader_id]] != b"BSLightingShaderProperty":
        raise ValueError("pruned fixture requires exact Lighting property")
    shader = nif.blocks[shader_id]
    # Bethesda130 Lighting: shaderType uint32; NiObjectNET name/extras/controller;
    # flags1/flags2 uint32; UV offset/scale float2; TextureSet Ref int32 at40.
    r = Reader(shader)
    shader_type, material_name, extras, controller = r.get("IiIi")
    if shader_type not in (0, 1) or extras or controller != -1 or not 0 <= material_name < len(nif.strings):
        raise ValueError("unproven Lighting dependency layout")
    r.take(24)
    texture_id = r.get("i")
    if texture_id < 0 or nif.types[nif.ids[texture_id]] != b"BSShaderTextureSet":
        raise ValueError("invalid Lighting texture dependency")
    texture = nif.blocks[texture_id]
    tr = Reader(texture)
    count = tr.get("I")
    if count != 10:
        raise ValueError("requires native Fallout4 texture slots")
    for _ in range(count):
        tr.string()
    if tr.pos != len(texture):
        raise ValueError("unexpected texture dependency tail")
    shader = shader[:40] + struct.pack("<i", 3) + shader[44:]
    mesh = nif.blocks[block]
    offset = selected["reference_offset"]
    assert struct.unpack_from("<iii", mesh, offset) == (-1, shader_id, -1)
    mesh = mesh[:offset] + struct.pack("<iii", -1, 2, -1) + mesh[offset + 12:]
    root_name = len(nif.strings)
    nif.strings.append(b"DTLMeshPilotPlainRoot")
    identity = (0., 0., 0., 1., 0., 0., 0., 1., 0., 0., 0., 1., 1.)
    root = struct.pack("<iIiI13fiIi", root_name, 0, -1, 14, *identity, -1, 1, 1)
    # Serialize ONLY the four reachable blocks. Orphan damage nodes must not
    # run LinkObjects and assign another parent to the selected geometry.
    count_offset = nif.prefix.find(b"\n", 0, 100) + 1 + 9
    prefix = bytearray(nif.prefix)
    struct.pack_into("<I", prefix, count_offset, 4)
    nif.prefix = bytes(prefix)
    nif.types = [b"NiNode", b"BSMeshLODTriShape", b"BSLightingShaderProperty", b"BSShaderTextureSet"]
    nif.ids = [0, 1, 2, 3]
    nif.blocks = [root, mesh, shader, texture]
    nif.groups = []
    nif.footer = struct.pack("<Ii", 1, 0)
    return selected, dict(original_blocks=[block, shader_id, texture_id],
                         new_blocks=["NiNode:0", "BSMeshLODTriShape:1", "BSLightingShaderProperty:2", "BSShaderTextureSet:3"],
                         root_children=[1], no_collision=True, no_extra_data=True, no_controllers=True, no_skin=True,
                         no_damage_nodes=True, no_multiple_parents=True,
                         shader_changed_bytes="TextureSet reference only", mesh_changed_bytes="ShaderProperty reference only")


def prepare(source: bytes, block: int, installed_fixture: Path, pruned=False):
    nif = Nif(source)
    if nif.serialize() != source:
        raise ValueError("parser did not preserve exact source bytes")
    selected = nif.mesh(block)
    source_hash = hashlib.sha256(source).hexdigest()
    tag = "DTLMeshPilot_" + source_hash[:16] + "_" + str(block)
    original_blocks = nif.blocks[:]
    nif.blocks[block] = struct.pack("<i", len(nif.strings)) + nif.blocks[block][4:]
    nif.strings.append(tag.encode("ascii"))
    output = nif.serialize()
    check = Nif(output)
    after = check.mesh(block)
    assert after["name"] == tag and after["vertex_data"] == selected["vertex_data"] and after["index_data"] == selected["index_data"]
    assert after["counts"] == selected["counts"] and check.blocks[block][4:] == original_blocks[block][4:]
    assert all(a == b for i, (a, b) in enumerate(zip(check.blocks, original_blocks)) if i != block)
    closure = None
    if pruned:
        _, closure = prune_rigid_dependency_closure(nif, block)
        output = nif.serialize()
        check = Nif(output)
        after = check.mesh(1)
        assert len(check.blocks) == 4 and check.ids == [0, 1, 2, 3]
        assert after["vertex_data"] == selected["vertex_data"] and after["index_data"] == selected["index_data"]
        assert after["descriptor"] == selected["descriptor"] and after["counts"] == selected["counts"]
    fixture_hash = hashlib.sha256(output).hexdigest()
    hashes = {"vertices": hashlib.sha256(selected["vertex_data"]).hexdigest(),
              "indices": hashlib.sha256(selected["index_data"]).hexdigest()}
    manifest = ("[Pilot]\nMode=SingleFixtureRetainedFull\nMaximumRecords=1\nNetMemorySavingClaim=0\n"
                f"FixturePath={installed_fixture}\nFixtureSHA256={fixture_hash}\nGeometryName={tag}\n"
                f"Hierarchy={'PlainRigidDependencyClosure' if pruned else 'OriginalAssetHierarchy'}\n"
                f"DescriptorHex={selected['descriptor']:016X}\nVertexCount={selected['vertices']}\n"
                f"TriangleCount={selected['triangles']}\n" +
                "".join(f"Segment{i}={count}\n" for i, count in enumerate(selected["counts"])) +
                f"VerticesSHA256={hashes['vertices']}\nIndicesSHA256={hashes['indices']}\n")
    receipt = dict(source_sha256=source_hash, fixture_sha256=fixture_hash, block=block, name_before=selected["name"],
                   name_after=tag, descriptor=f"{selected['descriptor']:016X}", segments=selected["counts"],
                   triangles=selected["triangles"], vertices=selected["vertices"], stride=selected["stride"],
                   source_hashes=hashes, prefixes=selected["prefixes"],
                   change="append one string; selected block's first int32 name index only",
                   unchanged="vertex/index payloads, native counts, materials, collision, hierarchy and all other blocks",
                   installed_fixture=str(installed_fixture), runtime_validated=False, net_memory_saving_claim=False,
                   scope="single-fixture retained-full-pair native reduction/restoration diagnostic")
    if pruned:
        receipt.update(closure=closure, change="new identity NiNode; selected mesh/material dependency closure only; name and block references remapped",
                       unchanged="selected vertex/index payloads, descriptor, native counts, transform, bounds, material properties/texture paths",
                       runtime_selected_block=1)
    return output, manifest, receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--block", required=True, type=int)
    parser.add_argument("--installed-fixture", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--pruned", action="store_true")
    args = parser.parse_args()
    if not args.installed_fixture.is_absolute() or args.output.exists():
        raise SystemExit("requires absolute private destination and new output directory")
    fixture, manifest, receipt = prepare(args.source.read_bytes(), args.block, args.installed_fixture, args.pruned)
    args.output.mkdir(parents=True)
    (args.output / "fixture.nif").write_bytes(fixture)
    (args.output / "pilot.ini").write_text(manifest, encoding="utf-8")
    (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
