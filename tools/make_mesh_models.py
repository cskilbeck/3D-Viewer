#!/usr/bin/env python3
"""Writes the generated mesh test models (models/mesh, models/gltf, models/errors)

    python tools/make_mesh_models.py [models directory]

Needs Pillow (for the PNG and WebP textures). Everything is small and made from a
few simple shapes so it's obvious what each file should look like (see models/README.md)
"""

import base64
import io
import json
import math
import os
import struct
import sys
import zipfile

from PIL import Image, ImageDraw, ImageFont

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'models')


def path(*parts):
    p = os.path.join(ROOT, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def write_text(name, text):
    with open(path(*name.split('/')), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('wrote', name.encode('ascii', 'backslashreplace').decode())


def write_bytes(name, data):
    with open(path(*name.split('/')), 'wb') as f:
        f.write(data)
    print('wrote', name.encode('ascii', 'backslashreplace').decode())


######################################################################
# Shapes: (positions, normals, uvs, triangles), flat shaded where it matters

def box(sx, sy, sz):
    """A box from (0,0,0) to (sx,sy,sz), each face with its own vertices, normals and 0..1 UVs.
    UV (0,0) is the bottom left of the image (OBJ convention), seen from outside with Z up"""
    faces = [
        # normal, origin, u axis, v axis (all in units of the box size)
        ((0, 0, 1), (0, 0, 1), (1, 0, 0), (0, 1, 0)),     # top
        ((0, 0, -1), (0, 1, 0), (1, 0, 0), (0, -1, 0)),   # bottom
        ((0, -1, 0), (0, 0, 0), (1, 0, 0), (0, 0, 1)),    # front
        ((0, 1, 0), (1, 1, 0), (-1, 0, 0), (0, 0, 1)),    # back
        ((1, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)),     # right
        ((-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1)),   # left
    ]
    size = (sx, sy, sz)
    positions, normals, uvs, triangles = [], [], [], []
    for n, o, u, v in faces:
        base = len(positions)
        for cu, cv in ((0, 0), (1, 0), (1, 1), (0, 1)):
            positions.append(tuple((o[i] + u[i] * cu + v[i] * cv) * size[i] for i in range(3)))
            normals.append(n)
            uvs.append((cu, cv))
        triangles += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return positions, normals, uvs, triangles


def cylinder(radius, height, segments):
    """Shared vertices, no normals: the sides should come out smooth and the caps flat"""
    positions = []
    for z in (0, height):
        for i in range(segments):
            a = 2 * math.pi * i / segments
            positions.append((radius * math.cos(a), radius * math.sin(a), z))
    bottom_center = len(positions)
    positions.append((0, 0, 0))
    top_center = len(positions)
    positions.append((0, 0, height))
    triangles = []
    for i in range(segments):
        j = (i + 1) % segments
        triangles += [(i, j, segments + j), (i, segments + j, segments + i)]
        triangles.append((bottom_center, j, i))
        triangles.append((top_center, segments + i, segments + j))
    return positions, triangles


def sphere(radius, rings, segments):
    positions, normals, triangles = [], [], []
    for r in range(rings + 1):
        theta = math.pi * r / rings
        for s in range(segments + 1):
            phi = 2 * math.pi * s / segments
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
            normals.append(n)
            positions.append(tuple(radius * c for c in n))
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            triangles += [(a, a + 1, b), (b, a + 1, b + 1)]
    return positions, normals, triangles


######################################################################
# Textures

def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def orientation_image(size=256):
    """Colored corners and a big F: shows up any flip or rotation"""
    image = Image.new('RGBA', (size, size), (230, 230, 230, 255))
    draw = ImageDraw.Draw(image)
    q = size // 4
    draw.rectangle((0, 0, q, q), fill=(220, 40, 40, 255))                      # top left red
    draw.rectangle((size - q, 0, size, q), fill=(40, 180, 40, 255))            # top right green
    draw.rectangle((0, size - q, q, size), fill=(40, 80, 220, 255))            # bottom left blue
    draw.rectangle((size - q, size - q, size, size), fill=(240, 200, 30, 255))  # bottom right yellow
    draw.text((size // 2, size // 2), 'F', fill=(20, 20, 20, 255), font=font(size // 2), anchor='mm')
    draw.text((size // 2, q // 2), 'TOP', fill=(20, 20, 20, 255), font=font(size // 8), anchor='mm')
    return image


def alpha_gradient_image(size=128):
    """Opaque on the left fading to clear on the right, with stripes so you can see through it"""
    image = Image.new('RGBA', (size, size))
    for x in range(size):
        alpha = int(255 * (1 - x / (size - 1)))
        for y in range(size):
            stripe = (y // 16) % 2
            image.putpixel((x, y), (200, 60, 30, alpha) if stripe else (240, 200, 60, alpha))
    return image


def dimples_normal_map(size=256, cells=4):
    """Round dimples in a grid (tangent space, +Y up the image)"""
    image = Image.new('RGB', (size, size))
    cell = size / cells
    for y in range(size):
        for x in range(size):
            dx = ((x + 0.5) % cell) / cell * 2 - 1
            dy = ((y + 0.5) % cell) / cell * 2 - 1
            r2 = dx * dx + dy * dy
            if r2 < 0.64:
                # inside a dimple: the normal leans towards the middle; image y goes down, normal +Y goes up
                nx, ny = dx * 0.8, -dy * 0.8
            else:
                nx, ny = 0.0, 0.0
            nz = math.sqrt(max(0.0, 1 - nx * nx - ny * ny))
            image.putpixel((x, y), (int((nx * 0.5 + 0.5) * 255), int((ny * 0.5 + 0.5) * 255), int((nz * 0.5 + 0.5) * 255)))
    return image


def encode(image, fmt):
    data = io.BytesIO()
    image.save(data, fmt, **({'lossless': True} if fmt == 'WEBP' else {}))
    return data.getvalue()


######################################################################
# Text mesh formats (millimeters)

def stl_ascii(name, positions, triangles):
    lines = [f'solid {name}']
    for a, b, c in triangles:
        pa, pb, pc = positions[a], positions[b], positions[c]
        e1 = [pb[i] - pa[i] for i in range(3)]
        e2 = [pc[i] - pa[i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        length = math.sqrt(sum(c * c for c in n)) or 1
        lines.append(f'  facet normal {n[0] / length:g} {n[1] / length:g} {n[2] / length:g}')
        lines.append('    outer loop')
        for p in (pa, pb, pc):
            lines.append(f'      vertex {p[0]:g} {p[1]:g} {p[2]:g}')
        lines.append('    endloop')
        lines.append('  endfacet')
    lines.append(f'endsolid {name}')
    return '\n'.join(lines) + '\n'


def stl_binary(positions, triangles):
    data = bytearray(b'binary STL written by make_mesh_models.py'.ljust(80, b' '))
    data += struct.pack('<I', len(triangles))
    for a, b, c in triangles:
        data += struct.pack('<3f', 0, 0, 0)
        for p in (positions[a], positions[b], positions[c]):
            data += struct.pack('<3f', *p)
        data += struct.pack('<H', 0)
    return bytes(data)


def to_y_up(p):
    """Z up to Y up (the front, -Y, becomes +Z)"""
    return (p[0], p[2], -p[1])


######################################################################
# glTF

class Gltf:
    """Just enough glTF 2.0 writing for the test files (meters, Y up)"""

    def __init__(self):
        self.json = {'asset': {'version': '2.0', 'generator': 'make_mesh_models.py'}, 'scene': 0, 'scenes': [{'nodes': []}],
                     'nodes': [], 'meshes': [], 'materials': [], 'accessors': [], 'bufferViews': [], 'buffers': []}
        self.bin = bytearray()

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin += b'\0'
        view = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)}
        if target:
            view['target'] = target
        self.bin += data
        self.json['bufferViews'].append(view)
        return len(self.json['bufferViews']) - 1

    def _accessor(self, values, kind, components):
        flat = [c for v in values for c in (v if isinstance(v, (tuple, list)) else (v,))]
        if kind == 'index':
            view = self._view(struct.pack(f'<{len(flat)}I', *flat), 34963)
            accessor = {'bufferView': view, 'componentType': 5125, 'count': len(flat), 'type': 'SCALAR'}
        else:
            view = self._view(struct.pack(f'<{len(flat)}f', *flat), 34962)
            types = {2: 'VEC2', 3: 'VEC3', 4: 'VEC4'}
            accessor = {'bufferView': view, 'componentType': 5126, 'count': len(values), 'type': types[components]}
            if kind == 'position':
                accessor['min'] = [min(v[i] for v in values) for i in range(3)]
                accessor['max'] = [max(v[i] for v in values) for i in range(3)]
        self.json['accessors'].append(accessor)
        return len(self.json['accessors']) - 1

    def image(self, data, mime):
        view = self._view(data)
        self.json.setdefault('images', []).append({'bufferView': view, 'mimeType': mime})
        return len(self.json['images']) - 1

    def texture(self, image, webp=False):
        self.json.setdefault('samplers', [{'magFilter': 9729, 'minFilter': 9987}])
        texture = {'sampler': 0}
        if webp:
            texture['extensions'] = {'EXT_texture_webp': {'source': image}}
            for key in ('extensionsUsed', 'extensionsRequired'):
                self.json.setdefault(key, [])
                if 'EXT_texture_webp' not in self.json[key]:
                    self.json[key].append('EXT_texture_webp')
        else:
            texture['source'] = image
        self.json.setdefault('textures', []).append(texture)
        return len(self.json['textures']) - 1

    def material(self, **m):
        self.json['materials'].append(m)
        return len(self.json['materials']) - 1

    def mesh(self, name, positions, triangles, material, normals=None, uvs=None):
        attributes = {'POSITION': self._accessor(positions, 'position', 3)}
        if normals:
            attributes['NORMAL'] = self._accessor(normals, 'normal', 3)
        if uvs:
            attributes['TEXCOORD_0'] = self._accessor(uvs, 'uv', 2)
        primitive = {'attributes': attributes, 'indices': self._accessor(triangles, 'index', 1), 'material': material}
        self.json['meshes'].append({'name': name, 'primitives': [primitive]})
        return len(self.json['meshes']) - 1

    def node(self, name, mesh=None, children=None, root=True, **transform):
        node = {'name': name}
        if mesh is not None:
            node['mesh'] = mesh
        if children:
            node['children'] = children
        node.update(transform)
        self.json['nodes'].append(node)
        index = len(self.json['nodes']) - 1
        if root:
            self.json['scenes'][0]['nodes'].append(index)
        return index

    def use_extension(self, name):
        self.json.setdefault('extensionsUsed', [])
        if name not in self.json['extensionsUsed']:
            self.json['extensionsUsed'].append(name)

    def glb(self):
        self.json['buffers'] = [{'byteLength': len(self.bin)}]
        text = json.dumps(self.json, separators=(',', ':')).encode()
        text += b' ' * (-len(text) % 4)
        binary = bytes(self.bin) + b'\0' * (-len(self.bin) % 4)
        total = 12 + 8 + len(text) + 8 + len(binary)
        return (struct.pack('<4sII', b'glTF', 2, total) + struct.pack('<I4s', len(text), b'JSON') + text +
                struct.pack('<I4s', len(binary), b'BIN\0') + binary)

    def gltf(self, bin_name=None):
        if bin_name:
            self.json['buffers'] = [{'byteLength': len(self.bin), 'uri': bin_name}]
        else:
            self.json['buffers'] = [{'byteLength': len(self.bin),
                                     'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(self.bin)).decode()}]
        return json.dumps(self.json, indent=1)


def gltf_box(g, name, sx, sy, sz, material, textured=False):
    """A box in meters (given in millimeters), Y up, glTF UVs (0,0 top left)"""
    positions, normals, uvs, triangles = box(sx, sy, sz)
    positions = [tuple(c / 1000 for c in to_y_up(p)) for p in positions]
    normals = [to_y_up(n) for n in normals]
    uvs = [(u, 1 - v) for u, v in uvs] if textured else None
    return g.mesh(name, positions, triangles, material, normals, uvs)


def gltf_quad(g, name, width, height, material):
    """A square facing the front (+Z in glTF), meters"""
    w, h = width / 1000, height / 1000
    positions = [(0, 0, 0), (w, 0, 0), (w, h, 0), (0, h, 0)]
    normals = [(0, 0, 1)] * 4
    uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
    return g.mesh(name, positions, [(0, 1, 2), (0, 2, 3)], material, normals, uvs)


def make_gltf():
    red = {'name': 'red', 'pbrMetallicRoughness': {'baseColorFactor': [0.7, 0.05, 0.05, 1], 'metallicFactor': 0, 'roughnessFactor': 0.5}}

    # the same red box three ways
    for kind in ('separate', 'embedded', 'glb'):
        g = Gltf()
        g.node('box', gltf_box(g, 'box', 10, 20, 30, g.material(**red)))
        if kind == 'separate':
            write_bytes('gltf/box.bin', bytes(g.bin))
            write_text('gltf/box.gltf', g.gltf('box.bin'))
        elif kind == 'embedded':
            write_text('gltf/box_embedded.gltf', g.gltf())
        else:
            write_bytes('gltf/box.glb', g.glb())

    # textured box, PNG and WebP
    for webp in (False, True):
        g = Gltf()
        image = g.image(encode(orientation_image(), 'WEBP' if webp else 'PNG'), 'image/webp' if webp else 'image/png')
        material = g.material(name='orientation', pbrMetallicRoughness={'baseColorTexture': {'index': g.texture(image, webp)},
                                                                         'metallicFactor': 0, 'roughnessFactor': 0.6})
        g.node('textured_box', gltf_box(g, 'textured_box', 30, 30, 30, material, textured=True))
        write_bytes('gltf/textured_webp.glb' if webp else 'gltf/textured.glb', g.glb())

    # texture transforms: plain, mirrored left/right, rotated 90 degrees
    g = Gltf()
    image = g.image(encode(orientation_image(), 'PNG'), 'image/png')
    texture = g.texture(image)
    g.use_extension('KHR_texture_transform')
    transforms = [('plain', None),
                  ('mirrored', {'scale': [-1, 1]}),
                  ('rotated', {'rotation': math.pi / 2, 'offset': [0, 1]})]
    for i, (name, transform) in enumerate(transforms):
        info = {'index': texture}
        if transform:
            info['extensions'] = {'KHR_texture_transform': transform}
        material = g.material(name=name, pbrMetallicRoughness={'baseColorTexture': info, 'metallicFactor': 0, 'roughnessFactor': 0.6})
        g.node(name, gltf_quad(g, name, 20, 20, material), translation=[i * 0.025, 0, 0])
    write_bytes('gltf/texture_transform.glb', g.glb())

    # alpha modes: opaque (alpha ignored), mask (cut off half way), blend
    g = Gltf()
    image = g.image(encode(alpha_gradient_image(), 'PNG'), 'image/png')
    texture = g.texture(image)
    for i, mode in enumerate(('OPAQUE', 'MASK', 'BLEND')):
        material = g.material(name=mode.lower(), alphaMode=mode, doubleSided=True,
                              pbrMetallicRoughness={'baseColorTexture': {'index': texture}, 'metallicFactor': 0, 'roughnessFactor': 0.6})
        if mode == 'MASK':
            g.json['materials'][material]['alphaCutoff'] = 0.5
        g.node(mode.lower(), gltf_quad(g, mode.lower(), 20, 20, material), translation=[i * 0.025, 0, 0])
    # something behind them to see through to
    blue = g.material(name='blue', pbrMetallicRoughness={'baseColorFactor': [0.1, 0.2, 0.8, 1], 'metallicFactor': 0, 'roughnessFactor': 0.5})
    g.node('backdrop', gltf_box(g, 'backdrop', 70, 2, 10, blue), translation=[0, 0.005, -0.01])
    write_bytes('gltf/alpha_modes.glb', g.glb())

    # metallic (left to right 0, 0.5, 1) x roughness (bottom to top 0.1, 0.5, 1) spheres
    g = Gltf()
    positions, normals, triangles = sphere(0.004, 16, 32)
    for i, metallic in enumerate((0.0, 0.5, 1.0)):
        for j, roughness in enumerate((0.1, 0.5, 1.0)):
            material = g.material(name=f'm{metallic}_r{roughness}', pbrMetallicRoughness={
                'baseColorFactor': [0.95, 0.65, 0.25, 1], 'metallicFactor': metallic, 'roughnessFactor': roughness})
            g.node(f'sphere_m{metallic}_r{roughness}', g.mesh('sphere', positions, triangles, material, normals),
                   translation=[i * 0.01, j * 0.01, 0])
    write_bytes('gltf/pbr_spheres.glb', g.glb())

    # a plate with a normal map of dimples
    g = Gltf()
    image = g.image(encode(dimples_normal_map(), 'PNG'), 'image/png')
    material = g.material(name='dimpled', normalTexture={'index': g.texture(image)},
                          pbrMetallicRoughness={'baseColorFactor': [0.8, 0.8, 0.85, 1], 'metallicFactor': 1, 'roughnessFactor': 0.3})
    g.node('dimpled_plate', gltf_quad(g, 'dimpled_plate', 40, 40, material))
    write_bytes('gltf/normal_map.glb', g.glb())

    # glowing box (emissive with KHR_materials_emissive_strength) next to a plain one
    g = Gltf()
    g.use_extension('KHR_materials_emissive_strength')
    glow = g.material(name='glow', emissiveFactor=[1.0, 0.4, 0.05], extensions={'KHR_materials_emissive_strength': {'emissiveStrength': 3.0}},
                      pbrMetallicRoughness={'baseColorFactor': [0.1, 0.1, 0.1, 1], 'metallicFactor': 0, 'roughnessFactor': 0.5})
    g.node('glowing_box', gltf_box(g, 'glowing_box', 10, 10, 10, glow))
    g.node('plain_box', gltf_box(g, 'plain_box', 10, 10, 10, g.material(**red)), translation=[0.015, 0, 0])
    write_bytes('gltf/emissive.glb', g.glb())

    # one mesh used several times: as is, rotated, mirrored, stretched, nested three deep
    g = Gltf()
    image = g.image(encode(orientation_image(), 'PNG'), 'image/png')
    material = g.material(name='orientation', pbrMetallicRoughness={'baseColorTexture': {'index': g.texture(image)},
                                                                     'metallicFactor': 0, 'roughnessFactor': 0.6})
    mesh = gltf_box(g, 'box', 10, 10, 10, material, textured=True)
    plain = g.node('plain', mesh, root=False)
    rotated = g.node('rotated', mesh, root=False, translation=[0.02, 0, 0], rotation=[0, 0.3826834, 0, 0.9238795])
    mirrored = g.node('mirrored', mesh, root=False, translation=[0.05, 0, 0], scale=[-1, 1, 1])
    stretched = g.node('stretched', mesh, root=False, translation=[0.06, 0, 0], scale=[1, 2, 0.5])
    deepest = g.node('deepest', mesh, root=False)
    middle = g.node('middle', children=[deepest], root=False, translation=[0, 0.015, 0])
    nested = g.node('nested', children=[middle], root=False, translation=[0.08, 0, 0])
    g.node('instances', children=[plain, rotated, mirrored, stretched, nested])
    write_bytes('gltf/hierarchy.glb', g.glb())

    # three see-through planes through each other (red XY, green YZ, blue XZ) and an opaque box poking through
    # them: sorting whole parts can't get this right, sorting triangles mostly can, depth peeling does
    g = Gltf()
    n = 10
    grid = [(i / n, j / n) for j in range(n + 1) for i in range(n + 1)]
    cells = [(j * (n + 1) + i, j * (n + 1) + i + 1, (j + 1) * (n + 1) + i + 1, j * (n + 1) + i, (j + 1) * (n + 1) + i + 1, (j + 1) * (n + 1) + i)
             for j in range(n) for i in range(n)]
    triangles = [t for c in cells for t in (c[0:3], c[3:6])]
    size = 0.04
    planes = [('red', [0.9, 0.1, 0.1, 0.5], lambda u, v: ((u - 0.5) * size, (v - 0.5) * size, 0), (0, 0, 1)),
              ('green', [0.1, 0.8, 0.1, 0.5], lambda u, v: (0, (v - 0.5) * size, (u - 0.5) * size), (1, 0, 0)),
              ('blue', [0.1, 0.3, 0.9, 0.5], lambda u, v: ((u - 0.5) * size, 0, (v - 0.5) * size), (0, 1, 0))]
    for name, color, position, normal in planes:
        material = g.material(name=name, alphaMode='BLEND', doubleSided=True,
                              pbrMetallicRoughness={'baseColorFactor': color, 'metallicFactor': 0, 'roughnessFactor': 0.4})
        g.node(f'{name}_plane', g.mesh(f'{name}_plane', [position(u, v) for u, v in grid], triangles, material, [normal] * len(grid)))
    grey = g.material(name='grey', pbrMetallicRoughness={'baseColorFactor': [0.6, 0.6, 0.6, 1], 'metallicFactor': 0, 'roughnessFactor': 0.5})
    g.node('box', gltf_box(g, 'box', 10, 10, 10, grey), translation=[0.005, 0.005, -0.005])
    write_bytes('gltf/transparency.glb', g.glb())

    # a long way from the origin (a kilometer)
    g = Gltf()
    g.node('far_box', gltf_box(g, 'far_box', 10, 10, 10, g.material(**red)), translation=[1000, 0, -1000])
    write_bytes('gltf/far_from_origin.glb', g.glb())


######################################################################
# Other mesh formats: the 10 x 20 x 30 mm box, Z up when it's loaded

def make_mesh():
    positions, normals, uvs, triangles = box(10, 20, 30)

    write_text('mesh/box_ascii.stl', stl_ascii('box', positions, triangles))
    write_bytes('mesh/box_binary.stl', stl_binary(positions, triangles))
    write_bytes('mesh/ünïcødé_名前.stl', stl_binary(positions, triangles))

    # OBJ is Y up, with a texture on every face
    obj = ['# 10 x 20 x 30 mm box, the orientation texture on every face', 'mtllib textured_box.mtl', 'o textured_box']
    obj += [f'v {p[0]:g} {p[1]:g} {p[2]:g}' for p in map(to_y_up, positions)]
    obj += [f'vt {u:g} {v:g}' for u, v in uvs]
    obj += [f'vn {n[0]:g} {n[1]:g} {n[2]:g}' for n in map(to_y_up, normals)]
    obj.append('usemtl orientation')
    obj += [f'f {a + 1}/{a + 1}/{a + 1} {b + 1}/{b + 1}/{b + 1} {c + 1}/{c + 1}/{c + 1}' for a, b, c in triangles]
    write_text('mesh/textured_box.obj', '\n'.join(obj) + '\n')
    write_text('mesh/textured_box.mtl', 'newmtl orientation\nKd 1 1 1\nNs 50\nmap_Kd orientation.png\n')
    write_bytes('mesh/orientation.png', encode(orientation_image(), 'PNG'))

    write_text('mesh/missing_texture.obj', '\n'.join(['mtllib missing_texture.mtl', 'o missing_texture'] +
                                                      [f'v {p[0]:g} {p[1]:g} {p[2]:g}' for p in map(to_y_up, positions)] +
                                                      [f'vt {u:g} {v:g}' for u, v in uvs] + ['usemtl missing'] +
                                                      [f'f {a + 1}/{a + 1} {b + 1}/{b + 1} {c + 1}/{c + 1}' for a, b, c in triangles]) + '\n')
    write_text('mesh/missing_texture.mtl', 'newmtl missing\nKd 0.8 0.3 0.3\nmap_Kd this_file_does_not_exist.png\n')

    # no normals: smooth sides and sharp edges round the caps
    cyl_positions, cyl_triangles = cylinder(5, 20, 32)
    obj = ['# r5 x 20 mm cylinder with shared vertices and no normals', 'o cylinder']
    obj += [f'v {p[0]:g} {p[1]:g} {p[2]:g}' for p in map(to_y_up, cyl_positions)]
    obj += [f'f {a + 1} {b + 1} {c + 1}' for a, b, c in cyl_triangles]
    write_text('mesh/cylinder_no_normals.obj', '\n'.join(obj) + '\n')

    # PLY (Y up)
    yup = [to_y_up(p) for p in positions]
    # (numbers and x's in the comment confuse Assimp's ASCII PLY reader)
    header = ['ply', 'format {}', 'comment a box', f'element vertex {len(yup)}', 'property float x', 'property float y',
              'property float z', f'element face {len(triangles)}', 'property list uchar int vertex_indices', 'end_header']
    text = '\n'.join(header).format('ascii 1.0') + '\n'
    text += ''.join(f'{p[0]:g} {p[1]:g} {p[2]:g}\n' for p in yup)
    text += ''.join(f'3 {a} {b} {c}\n' for a, b, c in triangles)
    write_text('mesh/box_ascii.ply', text)
    data = ('\n'.join(header).format('binary_little_endian 1.0') + '\n').encode()
    data += b''.join(struct.pack('<3f', *p) for p in yup)
    data += b''.join(struct.pack('<B3i', 3, a, b, c) for a, b, c in triangles)
    write_bytes('mesh/box_binary.ply', data)

    # OFF (Z up, like STL)
    text = f'OFF\n{len(positions)} {len(triangles)} 0\n'
    text += ''.join(f'{p[0]:g} {p[1]:g} {p[2]:g}\n' for p in positions)
    text += ''.join(f'3 {a} {b} {c}\n' for a, b, c in triangles)
    write_text('mesh/box.off', text)

    # Collada in centimeters with Z up: it should be the same size and way up as the others
    cm = [tuple(c / 10 for c in p) for p in positions]
    write_text('mesh/box_z_up_cm.dae', f'''<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="centimeter" meter="0.01"/><up_axis>Z_UP</up_axis></asset>
  <library_effects><effect id="green-effect"><profile_COMMON><technique sid="common"><phong>
    <diffuse><color>0.1 0.6 0.2 1</color></diffuse>
  </phong></technique></profile_COMMON></effect></library_effects>
  <library_materials><material id="green" name="green"><instance_effect url="#green-effect"/></material></library_materials>
  <library_geometries><geometry id="box" name="box"><mesh>
    <source id="box-positions"><float_array id="box-positions-array" count="{len(cm) * 3}">{' '.join(f'{c:g}' for p in cm for c in p)}</float_array>
      <technique_common><accessor source="#box-positions-array" count="{len(cm)}" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common></source>
    <vertices id="box-vertices"><input semantic="POSITION" source="#box-positions"/></vertices>
    <triangles material="green" count="{len(triangles)}"><input semantic="VERTEX" source="#box-vertices" offset="0"/>
      <p>{' '.join(str(i) for t in triangles for i in t)}</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node id="box_node" name="box">
    <instance_geometry url="#box"><bind_material><technique_common><instance_material symbol="green" target="#green"/></technique_common></bind_material></instance_geometry>
  </node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>
''')

    # X3D (meters, Y up)
    meters = [tuple(c / 1000 for c in to_y_up(p)) for p in positions]
    write_text('mesh/box.x3d', f'''<?xml version="1.0" encoding="UTF-8"?>
<X3D profile="Interchange" version="3.3">
  <Scene>
    <Shape>
      <Appearance><Material diffuseColor="0.9 0.6 0.1"/></Appearance>
      <IndexedFaceSet solid="true" coordIndex="{' '.join(f'{a} {b} {c} -1' for a, b, c in triangles)}">
        <Coordinate point="{', '.join(f'{p[0]:g} {p[1]:g} {p[2]:g}' for p in meters)}"/>
      </IndexedFaceSet>
    </Shape>
  </Scene>
</X3D>
''')

    # AMF (millimeters, Z up)
    write_text('mesh/box.amf', '<?xml version="1.0" encoding="UTF-8"?>\n<amf unit="millimeter">\n  <object id="0">\n    <mesh>\n      <vertices>\n' +
               ''.join(f'        <vertex><coordinates><x>{p[0]:g}</x><y>{p[1]:g}</y><z>{p[2]:g}</z></coordinates></vertex>\n' for p in positions) +
               '      </vertices>\n      <volume>\n' +
               ''.join(f'        <triangle><v1>{a}</v1><v2>{b}</v2><v3>{c}</v3></triangle>\n' for a, b, c in triangles) +
               '      </volume>\n    </mesh>\n  </object>\n</amf>\n')

    # 3MF (millimeters, Z up)
    model = ('<?xml version="1.0" encoding="UTF-8"?>\n'
             '<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">\n'
             '  <resources>\n    <object id="1" type="model" name="box">\n      <mesh>\n        <vertices>\n' +
             ''.join(f'          <vertex x="{p[0]:g}" y="{p[1]:g}" z="{p[2]:g}"/>\n' for p in positions) +
             '        </vertices>\n        <triangles>\n' +
             ''.join(f'          <triangle v1="{a}" v2="{b}" v3="{c}"/>\n' for a, b, c in triangles) +
             '        </triangles>\n      </mesh>\n    </object>\n  </resources>\n'
             '  <build><item objectid="1"/></build>\n</model>\n')
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('[Content_Types].xml', '<?xml version="1.0" encoding="UTF-8"?>\n'
                   '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
        z.writestr('_rels/.rels', '<?xml version="1.0" encoding="UTF-8"?>\n'
                   '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Target="/3D/3dmodel.model" Id="rel0" '
                   'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        z.writestr('3D/3dmodel.model', model)
    write_bytes('mesh/box.3mf', data.getvalue())


######################################################################
# Files which shouldn't load (an error, not a crash)

def make_errors():
    write_bytes('errors/empty.stl', b'')
    write_bytes('errors/empty.glb', b'')
    write_text('errors/not_really.step', 'This is not a STEP file.\n')
    write_text('errors/not_really.obj', '\0\1\2 not an OBJ file \xff\n')
    g = Gltf()
    g.node('box', gltf_box(g, 'box', 10, 20, 30, g.material(name='red')))
    whole = g.glb()
    write_bytes('errors/truncated.glb', whole[:len(whole) // 2])


make_gltf()
make_mesh()
make_errors()
