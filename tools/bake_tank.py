#!/usr/bin/env python3
"""Bake Quaternius tank.glb into game-ready tank_baked.glb:
- splits into hull (body+tracks) and turret (turret+gun) meshes
- bakes node transforms, reorients barrel to +Z, rescales, grounds at y=0
- converts flat material colors into a palette texture + per-vertex UVs
- emits turret pivot + muzzle metadata"""
import struct, json, zlib, sys, os
import numpy as np

SRC = 'assets/tank/tank.glb'
DST = 'assets/tank/tank_baked.glb'
META = 'assets/tank/tank_meta.txt'
PREVIEW = 'assets/tank/preview.png'
TARGET_LEN = 3.6  # desired hull length (Z) in world units

# ---------- read GLB ----------
data = open(SRC, 'rb').read()
clen, ctype = struct.unpack('<II', data[12:20])
gltf = json.loads(data[20:20+clen])
off = 20 + clen
bin_chunk = b''
while off < len(data):
    l, t = struct.unpack('<II', data[off:off+8])
    if t == 0x004E4942:
        bin_chunk = data[off+8:off+8+l]
    off += 8 + l

CT = {5120:('b',1),5121:('B',1),5122:('h',2),5123:('H',2),5125:('I',4),5126:('f',4)}
NC = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}

def read_accessor(idx):
    a = gltf['accessors'][idx]
    bv = gltf['bufferViews'][a['bufferView']]
    fmt, size = CT[a['componentType']]
    n = NC[a['type']]
    count = a['count']
    start = bv.get('byteOffset',0) + a.get('byteOffset',0)
    stride = bv.get('byteStride', size*n)
    out = np.zeros((count, n), dtype=np.float64 if fmt=='f' else np.int64)
    for i in range(count):
        vals = struct.unpack_from('<'+fmt*n, bin_chunk, start + i*stride)
        out[i] = vals
    return out

def trs_matrix(node):
    m = np.eye(4)
    if 'matrix' in node:
        return np.array(node['matrix'], dtype=np.float64).reshape(4,4).T
    t = node.get('translation',[0,0,0]); q = node.get('rotation',[0,0,0,1]); s = node.get('scale',[1,1,1])
    x,y,z,w = q
    R = np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])
    m[:3,:3] = R @ np.diag(s)
    m[:3,3] = t
    return m

# world transforms
world = {}
def walk(idx, parent):
    node = gltf['nodes'][idx]
    w = parent @ trs_matrix(node)
    world[idx] = w
    for c in node.get('children',[]):
        walk(c, w)
for root in gltf['scenes'][gltf.get('scene',0)]['nodes']:
    walk(root, np.eye(4))

# ---------- gather parts ----------
HULL_NAMES = ('Tank_body','TrackMesh.R','TrackMesh.L')
TURRET_NAMES = ('Tank_Turret','Tank_Gun')
mats = gltf['materials']

def mat_color(mi):
    f = mats[mi].get('pbrMetallicRoughness',{}).get('baseColorFactor',[1,1,1,1])
    return tuple(f[:3])

palette_colors = []  # unique linear colors
def palette_index(c):
    for i,pc in enumerate(palette_colors):
        if all(abs(a-b)<1e-4 for a,b in zip(pc,c)):
            return i
    palette_colors.append(c)
    return len(palette_colors)-1

parts = {'hull': {'pos':[], 'nrm':[], 'cell':[], 'idx':[], 'base':0},
         'turret': {'pos':[], 'nrm':[], 'cell':[], 'idx':[], 'base':0}}
gun_pos_list = []
turret_only_pos = []

for ni, node in enumerate(gltf['nodes']):
    name = node.get('name','')
    if 'mesh' not in node: continue
    if name in HULL_NAMES: part = parts['hull']
    elif name in TURRET_NAMES: part = parts['turret']
    else: continue
    W = world[ni]
    N3 = np.linalg.inv(W[:3,:3]).T
    mesh = gltf['meshes'][node['mesh']]
    for prim in mesh['primitives']:
        attrs = prim['attributes']
        pos = read_accessor(attrs['POSITION'])
        nrm = read_accessor(attrs['NORMAL']) if 'NORMAL' in attrs else np.zeros_like(pos)
        idx = read_accessor(prim['indices']).astype(np.int64).ravel() if 'indices' in prim else np.arange(len(pos))
        wpos = (W[:3,:3] @ pos.T).T + W[:3,3]
        wnrm = (N3 @ nrm.T).T
        ln = np.linalg.norm(wnrm, axis=1, keepdims=True); ln[ln==0]=1
        wnrm = wnrm/ln
        cell = palette_index(mat_color(prim.get('material',0)))
        base = len(part['pos']) and sum(len(p) for p in part['pos']) or 0
        base = sum(len(p) for p in part['pos'])
        part['pos'].append(wpos); part['nrm'].append(wnrm)
        part['cell'].append(np.full(len(wpos), cell))
        part['idx'].append(idx + base)
        if name == 'Tank_Gun': gun_pos_list.append(wpos)
        if name == 'Tank_Turret': turret_only_pos.append(wpos)

for p in parts.values():
    p['pos'] = np.vstack(p['pos']); p['nrm'] = np.vstack(p['nrm'])
    p['cell'] = np.concatenate(p['cell']); p['idx'] = np.concatenate(p['idx'])

gun_pos = np.vstack(gun_pos_list)
tur_pos = np.vstack(turret_only_pos)

# ---------- orient: barrel -> +Z ----------
tur_c = (tur_pos.min(0)+tur_pos.max(0))/2
gun_c = (gun_pos.min(0)+gun_pos.max(0))/2
d = gun_c - tur_c; d[1]=0
ang = np.arctan2(d[0], d[2])   # rotate so d aligns with +Z
c, s = np.cos(ang), np.sin(ang)
RY = np.array([[c,0,-s],[0,1,0],[s,0,c]])  # rotates vector (x,z) by -ang about Y
for p in parts.values():
    p['pos'] = (RY @ p['pos'].T).T
    p['nrm'] = (RY @ p['nrm'].T).T
gun_pos = (RY @ gun_pos.T).T
tur_pos = (RY @ tur_pos.T).T

# ---------- scale + ground ----------
hull = parts['hull']['pos']
scale = TARGET_LEN / (hull[:,2].max()-hull[:,2].min())
for p in parts.values(): p['pos'] *= scale
gun_pos *= scale; tur_pos *= scale
hull = parts['hull']['pos']
all_pos = np.vstack([parts['hull']['pos'], parts['turret']['pos']])
cx = (hull[:,0].max()+hull[:,0].min())/2
cz = (hull[:,2].max()+hull[:,2].min())/2
gy = all_pos[:,1].min()
shift = np.array([cx, gy, cz])
for p in parts.values(): p['pos'] -= shift
gun_pos -= shift; tur_pos -= shift

# ---------- turret pivot + relative verts ----------
pivot = np.array([(tur_pos[:,0].min()+tur_pos[:,0].max())/2,
                  tur_pos[:,1].min(),
                  (tur_pos[:,2].min()+tur_pos[:,2].max())/2])
parts['turret']['pos'] -= pivot
gun_pos -= pivot
# muzzle = center of far-Z 2% of gun verts (pivot space)
zmax = gun_pos[:,2].max()
tip = gun_pos[gun_pos[:,2] > zmax - 0.02*(zmax - gun_pos[:,2].min())]
muzzle = np.array([tip[:,0].mean(), tip[:,1].mean(), zmax])

hull = parts['hull']['pos']
hull_min, hull_max = hull.min(0), hull.max(0)

# ---------- palette png (RGBA, cells of 8px, 4 cols) ----------
def lin2srgb(v):
    v = max(0.0, min(1.0, v))
    return 12.92*v if v <= 0.0031308 else 1.055*(v**(1/2.4))-0.055
cols = 4
rows = (len(palette_colors)+cols-1)//cols
CELL = 8
W, H = cols*CELL, max(rows*CELL, CELL)
img = np.zeros((H, W, 4), dtype=np.uint8)
img[...,3] = 255
for i, cLin in enumerate(palette_colors):
    r8,g8,b8 = [int(round(lin2srgb(ch)*255)) for ch in cLin]
    cx0 = (i%cols)*CELL; cy0 = (i//cols)*CELL
    img[cy0:cy0+CELL, cx0:cx0+CELL, 0] = r8
    img[cy0:cy0+CELL, cx0:cx0+CELL, 1] = g8
    img[cy0:cy0+CELL, cx0:cx0+CELL, 2] = b8

def png_bytes(rgba):
    h, w = rgba.shape[:2]
    raw = b''.join(b'\x00' + rgba[y].tobytes() for y in range(h))
    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag+payload) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
png = png_bytes(img)

def cell_uv(i):
    u = ((i%cols)+0.5)/cols
    v = ((i//cols)+0.5)/max(rows,1)
    return u, v

for p in parts.values():
    uv = np.array([cell_uv(int(cidx)) for cidx in p['cell']], dtype=np.float64)
    p['uv'] = uv

# ---------- write baked GLB ----------
buf = bytearray()
bufferViews = []
accessors = []
def add_bv(payload, target=None):
    while len(buf)%4: buf.append(0)
    bv = {'buffer':0, 'byteOffset':len(buf), 'byteLength':len(payload)}
    if target: bv['target']=target
    buf.extend(payload)
    bufferViews.append(bv)
    return len(bufferViews)-1

def add_acc(arr, ctype, atype, target):
    a32 = arr.astype('<f4') if ctype==5126 else arr.astype('<u4')
    bv = add_bv(a32.tobytes(), target)
    acc = {'bufferView':bv,'componentType':ctype,'count':len(arr),'type':atype}
    if atype!='SCALAR':
        acc['min'] = [float(x) for x in arr.min(0)]
        acc['max'] = [float(x) for x in arr.max(0)]
    accessors.append(acc)
    return len(accessors)-1

meshes = []
nodes = []
for name, tr in (('hull',[0,0,0]), ('turret',[float(pivot[0]),float(pivot[1]),float(pivot[2])])):
    p = parts[name]
    ap = add_acc(p['pos'], 5126, 'VEC3', 34962)
    an = add_acc(p['nrm'], 5126, 'VEC3', 34962)
    at = add_acc(p['uv'],  5126, 'VEC2', 34962)
    ai = add_acc(p['idx'].reshape(-1,1), 5125, 'SCALAR', 34963)
    meshes.append({'name':name,'primitives':[{'attributes':{'POSITION':ap,'NORMAL':an,'TEXCOORD_0':at},'indices':ai,'material':0,'mode':4}]})
    nodes.append({'name':name,'mesh':len(meshes)-1,'translation':tr})

img_bv = add_bv(png)
out = {
 'asset':{'version':'2.0','generator':'tankaq-bake'},
 'scene':0,'scenes':[{'nodes':[0,1]}],
 'nodes':nodes,'meshes':meshes,
 'materials':[{'name':'palette','pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicFactor':0.1,'roughnessFactor':0.8}}],
 'textures':[{'source':0,'sampler':0}],
 'samplers':[{'magFilter':9729,'minFilter':9729,'wrapS':10497,'wrapT':10497}],
 'images':[{'mimeType':'image/png','bufferView':img_bv,'name':'palette'}],
 'bufferViews':bufferViews,'accessors':accessors,
 'buffers':[{'byteLength':len(buf)}],
}
js = json.dumps(out, separators=(',',':')).encode()
while len(js)%4: js += b' '
while len(buf)%4: buf.append(0)
glb = (struct.pack('<III', 0x46546C67, 2, 12+8+len(js)+8+len(buf))
       + struct.pack('<II', len(js), 0x4E4F534A) + js
       + struct.pack('<II', len(buf), 0x004E4942) + bytes(buf))
open(DST,'wb').write(glb)

with open(META,'w') as f:
    f.write(f"turret_pivot {pivot[0]:.4f} {pivot[1]:.4f} {pivot[2]:.4f}\n")
    f.write(f"muzzle {muzzle[0]:.4f} {muzzle[1]:.4f} {muzzle[2]:.4f}\n")
    f.write(f"hull_min {hull_min[0]:.4f} {hull_min[1]:.4f} {hull_min[2]:.4f}\n")
    f.write(f"hull_max {hull_max[0]:.4f} {hull_max[1]:.4f} {hull_max[2]:.4f}\n")

print("hull verts", len(parts['hull']['pos']), "tris", len(parts['hull']['idx'])//3)
print("turret verts", len(parts['turret']['pos']), "tris", len(parts['turret']['idx'])//3)
print("palette colors", len(palette_colors))
print("pivot", pivot, "muzzle", muzzle)
print("hull bounds", hull_min, hull_max)
tp = parts['turret']['pos']
print("turret bounds (pivot space)", tp.min(0), tp.max(0))
print("glb size", len(glb))

# ---------- preview ----------
try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1,3, figsize=(15,5))
    h = parts['hull']['pos']; t = parts['turret']['pos'] + pivot
    for ax, (a,b,ttl) in zip(axes, [(0,1,'front (X-Y)'),(2,1,'side (Z-Y)'),(0,2,'top (X-Z)')]):
        ax.scatter(h[:,a], h[:,b], s=1, c='olive', label='hull')
        ax.scatter(t[:,a], t[:,b], s=1, c='steelblue', label='turret+gun')
        mz = muzzle + pivot
        ax.scatter([mz[a]],[mz[b]], s=60, c='red', marker='x', label='muzzle')
        ax.scatter([pivot[a]],[pivot[b]], s=60, c='black', marker='+', label='pivot')
        ax.set_title(ttl); ax.set_aspect('equal'); ax.grid(True, alpha=0.3)
    axes[0].legend(markerscale=6)
    plt.tight_layout(); plt.savefig(PREVIEW, dpi=80)
    print("preview written")
except Exception as e:
    print("preview skipped:", e)
