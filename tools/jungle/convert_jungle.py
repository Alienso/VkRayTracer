# Converts Intel's Jungle Ruins (Graphics Research Samples) to glTF, from its USD version (USD/JungleRuins_Karma.usda;
# the .blend has the same scene but leaves the plants to the Geo-Scatter add-on). Blender is only used for its Python
# (it comes with USD and numpy) and to read the images:
#   blender -b --factory-startup --python tools/jungle/convert_jungle.py -- <JungleRuins>/USD/JungleRuins_Karma.usda
#           resource/models/JungleRuins [options]
# Add it with "Original size": the default camera's spot is the origin, 1 m = 1 unit, Y up.
#
# The scene: a pyramid in a jungle valley, 8 x 8 km of terrain tiles, about 8.7 million plants (USD point instancers:
# one mesh, then a position, rotation and scale per copy). Far too many for the app (one TLAS entry per copy), so
# each group of plants is kept within its own distance of the center (XY, default the scene's camera):
#   --trees 400       RiverForest, QueenForest, RiverSapling and the banyan (few, large)
#   --seedlings 100   RiverSeedling (2.3 million in total)
#   --pyramid 90      the moss and grass growing on the pyramid (the whole pyramid at 90)
#   --ground 60       grass, shrubs and the other plants on the ground
#   --terrain 1000    terrain tiles touching this distance
#   --keep Moss=0.5   keep this share of a plant type (random, the same every run); repeatable
#   --max-texture 4096  larger textures are scaled down (the terrain's are 8K)
#   --center X Y      in meters, in the USD scene's coordinates
#   --jobs N          texture worker processes; 0 = geometry only
#   --sky-dir DIR     where the sky (the scene's dome light, scaled to 4K) goes; default resource/environments
# Each plant mesh is written once, its copies with EXT_mesh_gpu_instancing (binary arrays, not one node each).
#
# Materials (UsdPreviewSurface) become glTF metallic-roughness: base color with the opacity mask in its alpha
# (BC3, else BC1), roughness (and the terrain's metallic) packed into G / B (BC1), normal maps BC5 (Y flipped where
# a map is DirectX), all with mip levels, through MSFT_texture_dds like the Sponza and Bistro tools. Leaves and grass
# let light through (KHR_materials_diffuse_transmission, as the Bistro's leaves); the river is thin clear water.
# Textures are encoded in several Blender processes at once (--jobs, default the number of cores / 2); finished ones
# are kept, so running again only redoes the geometry.
import bpy, sys, os, json, math, struct, time, argparse, subprocess, hashlib, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sponza'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bistro'))
import numpy as np
import bc_encode as bc
import bistro_dds as dds

GROUPS = {'RiverForest': 'trees', 'QueenForest': 'trees', 'RiverSapling': 'trees', 'RiverSeedling': 'seedlings',
          'Moss': 'pyramid', 'Pyramid': 'pyramid', 'Grass': 'ground', 'Shrub': 'ground', 'ShrubSorrel': 'ground',
          'Nettle': 'ground', 'Anthurium': 'ground'}
TRANSLUCENT = ('TwoSided', 'Leaves', 'Leaf', 'Grass', 'Moss', 'Nettle', 'Anthurium', 'Sorrel', 'Shrub')
LEAF_TRANSLUCENCY = 0.5  # as the Bistro's leaves: about half of what isn't reflected comes out the back


# ------------------------------------------------------------------------------------------------
# Textures (also run as worker processes: "textures <jobs.json> <shard> <shards>")

def load_image(path, max_size):
    """-> (h, w, 4) float32 0-255, rows top to bottom, the stored values (no color conversion)."""
    image = bpy.data.images.load(path, check_existing=False)
    image.colorspace_settings.name = 'Non-Color'
    w, h = image.size
    while max(w, h) > max_size:
        w, h = max(w // 2, 1), max(h // 2, 1)
    if (w, h) != tuple(image.size):
        image.scale(w, h)
    pixels = np.empty(w * h * 4, np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return np.ascontiguousarray(pixels.reshape(h, w, 4)[::-1]) * 255.0  # Blender's rows go bottom-up

def resize_to(pixels, h, w):
    """Nearest-texel resize (masks and data maps that come in another size than the color they go with)."""
    ys = (np.arange(h) * pixels.shape[0] // h)
    xs = (np.arange(w) * pixels.shape[1] // w)
    return pixels[ys][:, xs]

CHANNEL = {'r': 0, 'g': 1, 'b': 2, 'a': 3}

def make_texture(job, out_dir, max_size):
    out_path = os.path.join(out_dir, job['out'])
    if os.path.exists(out_path):
        return False
    kind = job['kind']
    if kind == 'color':
        pixels = load_image(job['file'], max_size)
        h, w = pixels.shape[:2]
        if job.get('alpha'):
            a_file, a_channel = job['alpha']
            alpha = pixels if a_file == job['file'] else resize_to(load_image(a_file, max_size), h, w)
            pixels[..., 3] = alpha[..., CHANNEL[a_channel]]
        fmt = 'DXT5' if job.get('alpha') else 'DXT1'
        levels = bc.mip_chain(pixels, srgb=True)
    elif kind == 'data':
        # glTF: G = roughness, B = metallic. Data stored as sRGB (the file says so) is linearized, as Karma does.
        r_file, r_channel, r_srgb = job['roughness']
        rough = load_image(r_file, max_size)
        h, w = rough.shape[:2]
        pixels = np.zeros((h, w, 4), np.float32)
        pixels[..., 0] = 255.0
        pixels[..., 3] = 255.0
        pixels[..., 1] = rough[..., CHANNEL[r_channel]]
        if r_srgb:
            pixels[..., 1] = bc.srgb_to_linear(pixels[..., 1]) * 255.0
        if job.get('metallic'):
            m_file, m_channel, m_srgb = job['metallic']
            metal = rough if m_file == r_file else resize_to(load_image(m_file, max_size), h, w)
            pixels[..., 2] = metal[..., CHANNEL[m_channel]]
            if m_srgb:
                pixels[..., 2] = bc.srgb_to_linear(pixels[..., 2]) * 255.0
        fmt = 'DXT1'
        levels = bc.mip_chain(pixels, srgb=False)
    else:  # normal
        pixels = load_image(job['file'], max_size)
        h, w = pixels.shape[:2]
        # Y up the image (OpenGL, as glTF) or down (DirectX)? The no-curl test of the Sponza tool.
        x, y = pixels[..., 0] / 127.5 - 1.0, pixels[..., 1] / 127.5 - 1.0
        dx_dyup = -(x[1:, :-1] - x[:-1, :-1])
        dy_dx = y[:-1, 1:] - y[:-1, :-1]
        score = float((dx_dyup * dy_dx).mean() / (np.sqrt((dx_dyup ** 2).mean() * (dy_dx ** 2).mean()) + 1e-12))
        if score < -0.15:
            print('normal map is DirectX (Y down), green flipped: %s (%.2f)' % (os.path.basename(job['file']), score))
            pixels[..., 1] = 255.0 - pixels[..., 1]
        fmt = 'ATI2'
        levels = []
        for level in bc.mip_chain(pixels, srgb=False):
            # Averaged normals get shorter; back to unit length keeps the slope's direction
            n = level[..., :3] / 127.5 - 1.0
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            levels.append(np.concatenate([(n + 1.0) * 127.5, level[..., 3:]], -1))
    tmp = out_path + '.tmp'
    bc.write_dds(tmp, w, h, fmt, [bc.encode(level, fmt) for level in levels])
    os.replace(tmp, out_path)  # a stopped run leaves no half-written file behind
    return True

def texture_worker(jobs_path, shard, shards):
    with open(jobs_path, encoding='utf-8') as f:
        spec = json.load(f)
    for i, job in enumerate(spec['jobs']):
        if i % shards == shard:
            start = time.time()
            if make_texture(job, spec['out_dir'], spec['max_size']):
                print('texture %-60s %.0fs' % (job['out'], time.time() - start), flush=True)


# ------------------------------------------------------------------------------------------------
# glTF writing

class Gltf:
    def __init__(self, out_dir, name):
        self.out_dir, self.name = out_dir, name
        self.json = {'asset': {'version': '2.0', 'generator': 'tools/jungle/convert_jungle.py'},
                     'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [], 'meshes': [], 'materials': [],
                     'textures': [], 'images': [], 'samplers': [], 'accessors': [], 'bufferViews': [], 'buffers': []}
        self.bins = {}  # buffer index -> open file

    def buffer(self, key):
        if key not in self.bins:
            uri = '%s_%s.bin' % (self.name, key)
            self.json['buffers'].append({'uri': uri, 'byteLength': 0})
            self.bins[key] = (len(self.json['buffers']) - 1, open(os.path.join(self.out_dir, uri), 'wb'))
        return self.bins[key]

    def accessor(self, array, key, target=None, minmax=False):
        """array (n,) or (n, k) of float32 / uint32 -> accessor index, data appended to buffer `key`."""
        index, f = self.buffer(key)
        array = np.ascontiguousarray(array)
        offset = f.tell()
        f.write(array.tobytes())
        f.write(b'\0' * ((-f.tell()) % 4))
        view = {'buffer': index, 'byteOffset': offset, 'byteLength': array.nbytes}
        if target:
            view['target'] = target
        self.json['bufferViews'].append(view)
        components = 1 if array.ndim == 1 else array.shape[1]
        acc = {'bufferView': len(self.json['bufferViews']) - 1, 'count': int(array.shape[0]),
               'componentType': {np.dtype(np.float32): 5126, np.dtype(np.uint32): 5125}[array.dtype],
               'type': {1: 'SCALAR', 2: 'VEC2', 3: 'VEC3', 4: 'VEC4'}[components]}
        if minmax:
            acc['min'] = [float(v) for v in array.min(0)]
            acc['max'] = [float(v) for v in array.max(0)]
        self.json['accessors'].append(acc)
        return len(self.json['accessors']) - 1

    def finish(self, extensions_used):
        for index, f in self.bins.values():
            self.json['buffers'][index]['byteLength'] = f.tell()
            f.close()
        self.json['extensionsUsed'] = sorted(extensions_used)
        self.json['extensionsRequired'] = ['MSFT_texture_dds', 'EXT_mesh_gpu_instancing']
        for key in ('textures', 'images', 'samplers', 'materials'):
            if not self.json[key]:
                del self.json[key]
        with open(os.path.join(self.out_dir, self.name + '.gltf'), 'w', encoding='utf-8') as f:
            json.dump(self.json, f, indent=1)


# ------------------------------------------------------------------------------------------------
# Conversion

def convert(args):
    from pxr import Usd, UsdGeom, UsdShade, Gf, Sdf
    start = time.time()
    out_dir = os.path.abspath(args.out_dir)
    if args.sky_dir is None:
        args.sky_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'resource', 'environments')
    os.makedirs(os.path.join(out_dir, 'textures'), exist_ok=True)
    stage = Usd.Stage.Open(os.path.abspath(args.usd))
    assert UsdGeom.GetStageUpAxis(stage) == 'Z'
    units = UsdGeom.GetStageMetersPerUnit(stage)  # 0.01: centimeters
    time_code = Usd.TimeCode.EarliestTime()  # the swaying trees: their first pose
    xforms = UsdGeom.XformCache(time_code)
    print('opened %.0fs' % (time.time() - start))

    camera = next(p for p in stage.Traverse() if p.IsA(UsdGeom.Camera))
    camera_matrix = xforms.GetLocalToWorldTransform(camera)
    if args.center:
        center = np.array(args.center) / units
    else:
        center = np.array(camera_matrix.ExtractTranslation())[:2]
    radius = {g: getattr(args, g) / units for g in ('trees', 'seedlings', 'pyramid', 'ground', 'terrain')}
    keep = {}
    for k in args.keep:
        name, value = k.split('=')
        keep[name] = float(value)

    gltf = Gltf(out_dir, args.name)
    # Root: centimeters -> meters, Z up -> Y up (x, y, z) -> (x, z, -y), the center to the origin
    s = units
    root = np.array([[s, 0, 0, -s * center[0]],
                     [0, 0, s, 0],
                     [0, -s, 0, s * center[1]],
                     [0, 0, 0, 1]], np.float64)
    gltf.json['nodes'].append({'name': 'JungleRuins', 'matrix': [float(v) for v in root.T.reshape(-1)],
                               'children': []})
    def add_node(node):
        gltf.json['nodes'].append(node)
        gltf.json['nodes'][0]['children'].append(len(gltf.json['nodes']) - 1)

    # ---- materials
    texture_jobs = {}  # out name -> job
    texture_index = {}  # out name -> glTF texture index
    samplers = {}
    materials = {}  # signature -> glTF material index
    stats = {'translucent': [], 'mask': [], 'water': []}

    def asset(attribute):
        value = attribute.Get()
        path = value.resolvedPath or value.path
        if not os.path.isabs(path):
            layer = attribute.GetPropertyStack(Usd.TimeCode.Default())[0].layer
            path = os.path.normpath(os.path.join(os.path.dirname(layer.realPath), path))
        assert os.path.exists(path), path
        return path

    def source(shader, name):
        """-> ('const', value) / ('texture', file, channel, srgb, wrap) / None"""
        i = shader.GetInput(name)
        if not i:
            return None
        if i.HasConnectedSource():
            src = i.GetConnectedSources()[0][0]
            tex = UsdShade.Shader(src.source.GetPrim())
            if tex.GetIdAttr().Get() != 'UsdUVTexture' or not tex.GetInput('file') or tex.GetInput('file').Get() is None:
                return None
            cs = tex.GetInput('sourceColorSpace')
            wrap = tex.GetInput('wrapS').Get() if tex.GetInput('wrapS') else 'repeat'
            return ('texture', asset(tex.GetInput('file').GetAttr()), src.sourceName, bool(cs and cs.Get() == 'sRGB'), wrap)
        value = i.Get()
        return None if value is None else ('const', value)

    def texture(job, wrap):
        out = job['out']
        if out not in texture_jobs:
            texture_jobs[out] = job
        key = (out, wrap)
        if key not in texture_index:
            if wrap not in samplers:
                mode = 33071 if wrap == 'clamp' else 10497
                gltf.json['samplers'].append({'magFilter': 9729, 'minFilter': 9987, 'wrapS': mode, 'wrapT': mode})
                samplers[wrap] = len(gltf.json['samplers']) - 1
            gltf.json['images'].append({'uri': out, 'mimeType': 'image/vnd-ms.dds', 'name': os.path.basename(out)})
            gltf.json['textures'].append({'sampler': samplers[wrap],
                                          'extensions': {'MSFT_texture_dds': {'source': len(gltf.json['images']) - 1}}})
            texture_index[key] = len(gltf.json['textures']) - 1
        return {'index': texture_index[key]}

    def stem(path):
        return os.path.splitext(os.path.basename(path))[0]

    def material(usd_material):
        name = usd_material.GetPrim().GetName()
        shader = usd_material.ComputeSurfaceSource()[0]
        inputs = {n: source(shader, n) for n in ('diffuseColor', 'opacity', 'opacityThreshold', 'roughness',
                                                  'metallic', 'normal', 'emissiveColor', 'ior')}
        signature = json.dumps([name, inputs], sort_keys=True, default=str)
        if signature in materials:
            return materials[signature]
        m = {'name': name, 'pbrMetallicRoughness': {}}
        pbr = m['pbrMetallicRoughness']
        ext = {}
        threshold = inputs['opacityThreshold'][1] if inputs['opacityThreshold'] else 0.0
        opacity = inputs['opacity']
        color = inputs['diffuseColor']
        if name == 'River':
            # Clear thin water over the river bed (its color texture isn't connected in the USD file either)
            pbr.update({'baseColorFactor': [1.0, 1.0, 1.0, 1.0], 'metallicFactor': 0.0, 'roughnessFactor': 0.0})
            ext['KHR_materials_transmission'] = {'transmissionFactor': 1.0}
            ext['KHR_materials_ior'] = {'ior': 1.333}
            stats['water'].append(name)
        else:
            if color and color[0] == 'texture':
                job = {'kind': 'color', 'file': color[1]}
                out = 'textures/' + stem(color[1])
                if opacity and opacity[0] == 'texture' and threshold > 0.0:
                    job['alpha'] = [opacity[1], opacity[2]]
                    if opacity[1] != color[1]:
                        out += '+' + stem(opacity[1])
                job['out'] = out + '.dds'
                pbr['baseColorTexture'] = texture(job, color[4])
            elif color:
                pbr['baseColorFactor'] = [float(v) for v in color[1]] + [1.0]
            if threshold > 0.0 and opacity and opacity[0] == 'texture':
                m['alphaMode'] = 'MASK'
                m['alphaCutoff'] = float(threshold)
                stats['mask'].append(name)
            rough, metal = inputs['roughness'], inputs['metallic']
            if rough and rough[0] == 'texture':
                job = {'kind': 'data', 'roughness': [rough[1], rough[2], rough[3]]}
                out = 'textures/' + stem(rough[1]) + '_' + rough[2]
                if metal and metal[0] == 'texture':
                    job['metallic'] = [metal[1], metal[2], metal[3]]
                    out += '+' + stem(metal[1]) + '_' + metal[2]
                    pbr['metallicFactor'] = 1.0
                else:
                    pbr['metallicFactor'] = float(metal[1]) if metal else 0.0
                job['out'] = out + '.dds'
                pbr['roughnessFactor'] = 1.0
                pbr['metallicRoughnessTexture'] = texture(job, rough[4])
            else:
                pbr['roughnessFactor'] = float(rough[1]) if rough else 0.5
                pbr['metallicFactor'] = float(metal[1]) if metal and metal[0] == 'const' else 0.0
            normal = inputs['normal']
            if normal and normal[0] == 'texture':
                job = {'kind': 'normal', 'file': normal[1], 'out': 'textures/' + stem(normal[1]) + '.dds'}
                m['normalTexture'] = texture(job, normal[4])
            if inputs['ior'] and inputs['ior'][0] == 'const' and abs(inputs['ior'][1] - 1.5) > 1e-3:
                ext['KHR_materials_ior'] = {'ior': float(inputs['ior'][1])}
            if any(k in name for k in TRANSLUCENT):
                dt = {'diffuseTransmissionFactor': LEAF_TRANSLUCENCY}
                if 'baseColorTexture' in pbr:
                    dt['diffuseTransmissionColorTexture'] = dict(pbr['baseColorTexture'])
                ext['KHR_materials_diffuse_transmission'] = dt
                stats['translucent'].append(name)
        if ext:
            m['extensions'] = ext
        gltf.json['materials'].append(m)
        materials[signature] = len(gltf.json['materials']) - 1
        return materials[signature]

    # ---- meshes
    def corner_values(values, interpolation, counts, fvi):
        if interpolation == 'faceVarying':
            return values
        if interpolation in ('vertex', 'varying'):
            return values[fvi]
        if interpolation == 'uniform':
            return np.repeat(values, counts, axis=0)
        return np.broadcast_to(values, (len(fvi),) + values.shape[1:])

    def read_mesh(prim, matrix):
        """One USD mesh -> list of (material index, positions, normals, uvs, indices) in the space `matrix` (row
        vectors, USD's convention) takes it to; one entry per material (GeomSubset)."""
        mesh = UsdGeom.Mesh(prim)
        points = np.array(mesh.GetPointsAttr().Get(time_code), np.float64)
        counts = np.array(mesh.GetFaceVertexCountsAttr().Get(time_code), np.int64)
        fvi = np.array(mesh.GetFaceVertexIndicesAttr().Get(time_code), np.int64)
        normals = None
        if mesh.GetNormalsAttr().HasValue():
            normals = corner_values(np.array(mesh.GetNormalsAttr().Get(time_code), np.float32),
                                    mesh.GetNormalsInterpolation(), counts, fvi)
        uvs = None
        st = UsdGeom.PrimvarsAPI(prim).GetPrimvar('st')
        if st and st.HasValue():
            uvs = corner_values(np.array(st.ComputeFlattened(time_code), np.float32), st.GetInterpolation(), counts, fvi)
        left_handed = mesh.GetOrientationAttr().Get() == 'leftHanded'

        # Fan triangulation, in corner indices
        tris_per_face = np.maximum(counts - 2, 0)
        face_start = np.cumsum(counts) - counts
        tri_face = np.repeat(np.arange(len(counts)), tris_per_face)
        k = np.arange(len(tri_face)) - np.repeat(np.cumsum(tris_per_face) - tris_per_face, tris_per_face) + 1
        s0 = face_start[tri_face]
        tris = np.stack([s0, s0 + k, s0 + k + 1], -1)

        m3 = np.array(matrix, np.float64)[:3, :3]
        t = np.array(matrix, np.float64)[3, :3]
        if (np.linalg.det(m3) < 0) != left_handed:
            tris = tris[:, [0, 2, 1]]
        normal_matrix = np.linalg.inv(m3).T

        # Faces -> material: the mesh's own binding, then its GeomSubsets
        bound = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]
        face_material = np.full(len(counts), -1, np.int64)
        default_material = material(bound) if bound else -1
        face_material[:] = default_material
        for subset in UsdGeom.Subset.GetAllGeomSubsets(mesh):
            if subset.GetFamilyNameAttr().Get() != 'materialBind':
                continue
            sub_material = UsdShade.MaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial()[0]
            if sub_material:
                face_material[np.array(subset.GetIndicesAttr().Get(), np.int64)] = material(sub_material)
        tri_material = face_material[tri_face]

        parts = []
        for mat in np.unique(tri_material):
            corners = tris[tri_material == mat]
            used = corners.reshape(-1)
            # One vertex per distinct (point, normal, uv)
            key = [fvi[used].astype(np.int32)[:, None]]
            if normals is not None:
                key.append(normals[used].view(np.int32))
            if uvs is not None:
                key.append(uvs[used].view(np.int32))
            key = np.ascontiguousarray(np.concatenate(key, 1))
            _, first, inverse = np.unique(key.view(np.dtype((np.void, key.dtype.itemsize * key.shape[1]))).reshape(-1),
                                          return_index=True, return_inverse=True)
            vertex_corner = used[first]
            positions = (points[fvi[vertex_corner]] @ m3 + t).astype(np.float32)
            vn = None
            if normals is not None:
                vn = normals[vertex_corner].astype(np.float64) @ normal_matrix
                vn = (vn / np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)).astype(np.float32)
            vuv = None
            if uvs is not None:
                vuv = uvs[vertex_corner].copy()
                vuv[:, 1] = 1.0 - vuv[:, 1]  # USD's V goes up the image, glTF's down
            parts.append((int(mat), positions, vn, vuv, inverse.reshape(-1).astype(np.uint32)))
        return parts

    mesh_index = {}  # key -> glTF mesh index
    triangle_count = {}

    def write_mesh(key, name, parts):
        primitives = []
        tris = 0
        for mat, positions, normals, uvs, indices in parts:
            attributes = {'POSITION': gltf.accessor(positions, 'geometry', 34962, minmax=True)}
            if normals is not None:
                attributes['NORMAL'] = gltf.accessor(normals, 'geometry', 34962)
            if uvs is not None:
                attributes['TEXCOORD_0'] = gltf.accessor(uvs, 'geometry', 34962)
            primitive = {'attributes': attributes, 'indices': gltf.accessor(indices, 'geometry', 34963)}
            if mat >= 0:
                primitive['material'] = mat
            primitives.append(primitive)
            tris += len(indices) // 3
        gltf.json['meshes'].append({'name': name, 'primitives': primitives})
        mesh_index[key] = len(gltf.json['meshes']) - 1
        triangle_count[key] = tris
        return mesh_index[key]

    def visible(prim):
        img = UsdGeom.Imageable(prim)
        return img.ComputeVisibility(time_code) != 'invisible' and img.ComputePurpose() in ('default', 'render')

    # ---- static meshes: terrain tiles, the pyramid, the river, the banyan tree
    bounds = UsdGeom.BBoxCache(time_code, ['default', 'render'])
    def distance_xy(r):
        lo, hi = np.array(r.GetMin())[:2], np.array(r.GetMax())[:2]
        return float(np.linalg.norm(np.maximum(np.maximum(lo - center, center - hi), 0.0)))
    static_tris = 0
    for prim in stage.GetPrimAtPath('/root').GetChildren():
        if prim.GetName() in ('materials', 'Camera') or not visible(prim):
            continue
        name = prim.GetName()
        group = 'terrain' if name.startswith(('M_', 'E_')) else 'trees' if 'Banyan' in name else None
        if group and (distance_xy(bounds.ComputeWorldBound(prim).ComputeAlignedRange()) > radius[group] or
                      (group == 'trees' and args.no_banyan)):
            continue
        for q in Usd.PrimRange(prim, Usd.TraverseInstanceProxies()):
            if q.IsA(UsdGeom.Mesh) and visible(q):
                matrix = xforms.GetLocalToWorldTransform(q)
                index = write_mesh(str(q.GetPath()), q.GetName(), read_mesh(q, matrix))
                add_node({'name': q.GetName(), 'mesh': index})
                static_tris += triangle_count[str(q.GetPath())]
    print('static meshes: %d triangles, %.0fs' % (static_tris, time.time() - start), flush=True)

    # ---- plants: per point instancer, the copies within reach; one mesh per prototype
    per_mesh = {}  # mesh key -> list of (positions, rotations xyzw, scales)
    plant_stats = {}
    checked = False
    for prim in stage.GetPrimAtPath('/World').GetChildren():
        if not prim.IsA(UsdGeom.PointInstancer) or not visible(prim):
            continue
        species = prim.GetName().split('_')[0]
        group = GROUPS.get(species)
        assert group, 'unknown plant type ' + prim.GetName()
        pi = UsdGeom.PointInstancer(prim)
        positions = np.array(pi.GetPositionsAttr().Get(time_code), np.float64)
        if len(positions) == 0:
            continue
        instancer_matrix = np.array(xforms.GetLocalToWorldTransform(prim))
        assert np.allclose(instancer_matrix, np.eye(4)), prim.GetPath()  # positions are in world space
        selected = np.linalg.norm(positions[:, :2] - center, axis=1) < radius[group]
        if species in keep:
            rng = np.random.default_rng(zlib.crc32(prim.GetName().encode()))
            selected &= rng.random(len(positions)) < keep[species]
        invisible = pi.GetInvisibleIdsAttr().Get(time_code)
        if invisible is not None and len(invisible):
            ids = pi.GetIdsAttr().Get(time_code)
            ids = np.array(ids) if ids is not None and len(ids) else np.arange(len(positions))
            selected &= ~np.isin(ids, np.array(invisible))
        stat = plant_stats.setdefault(species, [0, 0])
        stat[0] += len(positions)
        stat[1] += int(selected.sum())
        if not selected.any():
            continue
        proto_indices = np.array(pi.GetProtoIndicesAttr().Get(time_code), np.int64)
        orientations = np.array(pi.GetOrientationsAttr().Get(time_code), np.float32)  # (i, j, k, real) = glTF's xyzw
        orientations /= np.maximum(np.linalg.norm(orientations, axis=1, keepdims=True), 1e-12)
        scales_attr = pi.GetScalesAttr().Get(time_code)
        scales = np.array(scales_attr, np.float32) if scales_attr is not None and len(scales_attr) else \
            np.ones((len(positions), 3), np.float32)
        if not checked:
            # Our T * R * S against USD's own instance matrices (row vectors: S * R * T)
            usd = pi.ComputeInstanceTransformsAtTime(time_code, time_code)
            for i in range(min(5, len(positions))):
                x, y, z, w = orientations[i].astype(np.float64)
                r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                              [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                              [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
                ours = r * scales[i].astype(np.float64)
                theirs = np.array(usd[i])[:3, :3].T  # their rows = our columns
                proto_root = stage.GetPrimAtPath(pi.GetPrototypesRel().GetTargets()[proto_indices[i]])
                proto_m = np.array(UsdGeom.Xformable(proto_root).GetLocalTransformation(time_code))[:3, :3].T
                assert np.allclose(ours @ proto_m, theirs, atol=1e-2 * np.abs(theirs).max()), (ours, theirs)
            checked = True
        targets = pi.GetPrototypesRel().GetTargets()
        for p, target in enumerate(targets):
            chosen = selected & (proto_indices == p)
            if not chosen.any():
                continue
            proto_root = stage.GetPrimAtPath(target)
            # The same plant comes as a prototype of every instancer that uses it: one mesh per referenced plant
            refs = proto_root.GetMetadata('references')
            items = list(refs.GetAddedOrExplicitItems()) if refs else []
            ref_key = '%s:%s' % (os.path.basename(items[0].assetPath), items[0].primPath) if items else str(target)
            parent = proto_root.GetParent()
            for q in Usd.PrimRange(proto_root, Usd.TraverseInstanceProxies()):
                if not (q.IsA(UsdGeom.Mesh) and visible(q)):
                    continue
                # Mesh -> instance space: its transforms up to and including the prototype root
                relative = xforms.ComputeRelativeTransform(q, parent)[0]
                key = ref_key + str(q.GetPath()).replace(str(target), '')
                if key not in mesh_index:
                    write_mesh(key, q.GetName(), read_mesh(q, relative))
                per_mesh.setdefault(key, []).append((positions[chosen].astype(np.float32), orientations[chosen],
                                                     scales[chosen]))
    instance_count = 0
    instanced_tris = 0
    for key, chunks in per_mesh.items():
        t = np.concatenate([c[0] for c in chunks])
        r = np.concatenate([c[1] for c in chunks])
        sc = np.concatenate([c[2] for c in chunks])
        attributes = {'TRANSLATION': gltf.accessor(t, 'instances'), 'ROTATION': gltf.accessor(r, 'instances'),
                      'SCALE': gltf.accessor(sc, 'instances')}
        add_node({'name': gltf.json['meshes'][mesh_index[key]]['name'], 'mesh': mesh_index[key],
                  'extensions': {'EXT_mesh_gpu_instancing': {'attributes': attributes}}})
        instance_count += len(t)
        instanced_tris += len(t) * triangle_count[key]
    for species, (total, kept) in sorted(plant_stats.items()):
        print('  %-14s %9d of %9d' % (species, kept, total))
    plant_tris = sum(triangle_count[k] for k in per_mesh)
    print('plants: %d copies of %d meshes (%d triangles; %.0f million with the copies), %.0fs' %
          (instance_count, len(per_mesh), plant_tris, instanced_tris / 1e6, time.time() - start), flush=True)

    gltf.finish({'MSFT_texture_dds', 'EXT_mesh_gpu_instancing', 'KHR_materials_transmission', 'KHR_materials_ior',
                 'KHR_materials_diffuse_transmission'})
    print('translucent:', sorted(set(stats['translucent'])))
    print('cut-out:', sorted(set(stats['mask'])))

    # The default camera, in the converted scene's coordinates
    cam = np.array(camera_matrix)
    eye = root @ np.append(cam[3, :3], 1.0)
    forward = root[:3, :3] @ -cam[2, :3]
    forward /= np.linalg.norm(forward)
    print('camera: position (%.2f, %.2f, %.2f), looking along (%.3f, %.3f, %.3f)' % (*eye[:3], *forward))

    # ---- the scene's sky (its dome light), at 4K for the app's environments folder: 8K would be 512 MB as RGBA32F
    dome = next((p for p in stage.Traverse() if p.GetTypeName() == 'DomeLight'), None)
    if dome and args.sky_dir:
        sky = asset(dome.GetAttribute('inputs:texture:file'))
        os.makedirs(args.sky_dir, exist_ok=True)
        sky_out = os.path.join(args.sky_dir, stem(sky).replace('_8k', '') + '_4k.hdr')
        if not os.path.exists(sky_out):
            image = bpy.data.images.load(sky, check_existing=False)
            w, h = image.size
            while w > 4096:
                w, h = w // 2, h // 2
            image.scale(w, h)
            image.filepath_raw = sky_out
            image.file_format = 'HDR'
            image.save()
            bpy.data.images.remove(image)
        print('sky:', sky_out)

    # ---- textures, in parallel worker processes
    jobs = sorted(texture_jobs.values(), key=lambda j: j['out'])
    todo = [j for j in jobs if not os.path.exists(os.path.join(out_dir, j['out']))]
    print('textures: %d (%d to make)' % (len(jobs), len(todo)), flush=True)
    if todo and args.jobs == 0:
        print('CONVERTED without textures (--jobs 0), %.0fs' % (time.time() - start))
        return
    if todo:
        jobs_path = os.path.join(out_dir, 'texture_jobs.json')
        with open(jobs_path, 'w', encoding='utf-8') as f:
            json.dump({'out_dir': out_dir, 'max_size': args.max_texture, 'jobs': todo}, f, indent=1)
        shards = max(1, min(args.jobs, len(todo)))
        workers = [subprocess.Popen([bpy.app.binary_path, '-b', '--factory-startup', '--python',
                                     os.path.abspath(__file__), '--', 'textures', jobs_path, str(i), str(shards)])
                   for i in range(shards)]
        failed = sum(w.wait() != 0 for w in workers)
        missing = [j['out'] for j in jobs if not os.path.exists(os.path.join(out_dir, j['out']))]
        assert not failed and not missing, ('texture workers failed', failed, missing[:5])
        os.remove(jobs_path)
    size = sum(os.path.getsize(os.path.join(out_dir, j['out'])) for j in jobs)
    print('CONVERTED in %.0fs: %d meshes, %d nodes, %d materials, textures %.0f MB' %
          (time.time() - start, len(gltf.json['meshes']), len(gltf.json['nodes']), len(gltf.json['materials']),
           size / 2 ** 20))


argv = sys.argv[sys.argv.index('--') + 1:]
if argv and argv[0] == 'textures':
    try:
        texture_worker(argv[1], int(argv[2]), int(argv[3]))
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
else:
    parser = argparse.ArgumentParser(prog='convert_jungle.py')
    parser.add_argument('usd')
    parser.add_argument('out_dir')
    parser.add_argument('--name', default='JungleRuins')
    parser.add_argument('--center', type=float, nargs=2)
    parser.add_argument('--trees', type=float, default=400.0)
    parser.add_argument('--seedlings', type=float, default=100.0)
    parser.add_argument('--pyramid', type=float, default=90.0)
    parser.add_argument('--ground', type=float, default=60.0)
    parser.add_argument('--terrain', type=float, default=1000.0)
    parser.add_argument('--keep', action='append', default=[])
    parser.add_argument('--no-banyan', action='store_true')
    parser.add_argument('--max-texture', type=int, default=4096)
    parser.add_argument('--sky-dir', default=None)
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    try:
        convert(parser.parse_args(argv))
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
