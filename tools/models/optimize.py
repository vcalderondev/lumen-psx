#!/usr/bin/env python3
"""Turn the full-detail models Blender exports (.lmdl v1: a triangle soup per object)
into what Lumen ships and draws (.lmdl v2, see src/meshes.inc). Plain Python, no Blender.

  python3 tools/models/optimize.py <dir with v1 models> <assets/models> [--eps MM] [key...]
  python3 tools/models/optimize.py --info <dir with models>      (parts, triangles, sizes)

What it does, per model:
  * merges the objects that draw the same way (kind, colours, gloss, animation) into one
    part: one draw call each instead of one per screw, ridge and contact;
  * welds the vertices and decimates each part by edge collapse, keeping every original
    face plane within a tolerance (0.05 mm for the cartridges and cases, 0.10 mm for the
    consoles, drawn smaller: well under a pixel at 1080p in the launch animation; --eps
    sets one for all), so bevels and cylinders lose the segments nobody can see and flat
    faces their spare vertices; hard edges, the shading normals and anything deeper than
    the tolerance are kept. Do not raise it lightly: at 0.2 mm the grip grooves of the NES
    cartridge go, and at 0.1 mm the UMD's clamp sinks into the layer under it (parts that
    lie a fraction of a millimetre over one another swap places);
    printed parts (label, screen, back, spine) are left alone (their UVs are projected);
  * makes the winding counter-clockwise seen from outside (Lumen culls back faces);
  * writes one shared vertex buffer (20 bytes a vertex: float position, signed-byte
    normal, 16-bit UV) and 16-bit indices, parts ordered opaque first, glass last.
"""
import heapq, math, os, struct, sys

KINDS = ["plastic", "label", "screen", "light", "glass", "dark", "back", "spine"]
PRINTED = (1, 2, 6, 7)
ORDER = {0: 0, 5: 0, 3: 1, 1: 2, 2: 2, 6: 2, 7: 3, 4: 4}   # flat, lights, textured, spine, glass


def load_v1(path):
    d = open(path, "rb").read()
    if d[:4] != b"LMDL":
        raise SystemExit(f"{path}: not a model")
    ver, n = struct.unpack_from("<II", d, 4)
    if ver != 1:
        raise SystemExit(f"{path}: version {ver}, expected the v1 Blender export")
    o = 12
    parts = []
    for _ in range(n):
        name = d[o:o + 48].split(b"\0")[0].decode(); o += 48
        mat = struct.unpack_from("<10f3I7f", d, o); o += 80   # rgba on gloss spec | kind anim chan | pivot axis amount
        nv, = struct.unpack_from("<I", d, o); o += 4
        vs = struct.unpack_from(f"<{nv * 8}f", d, o); o += nv * 32
        parts.append((name, mat, vs))
    return parts


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def unit(a):
    l = math.sqrt(dot(a, a))
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-20 else (0.0, 0.0, 0.0)


class Mesh:
    """Triangles over welded positions; every corner keeps its own normal and UV."""

    def __init__(self):
        self.P = []          # positions
        self.F = []          # [a, b, c] position ids (None once removed)
        self.N = []          # per face: 3 corner normals
        self.UV = []         # per face: 3 corner uvs
        self._ids = {}

    def vid(self, p):
        k = (round(p[0] * 2000), round(p[1] * 2000), round(p[2] * 2000))   # 0.5 micron grid
        i = self._ids.get(k)
        if i is None:
            i = self._ids[k] = len(self.P)
            self.P.append(p)
        return i

    def add_soup(self, vs, keep_uv):
        flipped = 0
        for t in range(0, len(vs), 24):
            c = [vs[t + k * 8:t + k * 8 + 8] for k in range(3)]
            ids = [self.vid(x[0:3]) for x in c]
            if len(set(ids)) < 3:
                continue
            g = cross(sub(c[1], c[0]), sub(c[2], c[0]))
            if dot(g, g) < 1e-14:
                continue
            nr = [unit(x[5:8]) for x in c]
            uv = [(x[3], x[4]) if keep_uv else (0.0, 0.0) for x in c]
            if dot(g, (nr[0][0] + nr[1][0] + nr[2][0], nr[0][1] + nr[1][1] + nr[2][1], nr[0][2] + nr[1][2] + nr[2][2])) < 0:
                ids = [ids[0], ids[2], ids[1]]; nr = [nr[0], nr[2], nr[1]]; uv = [uv[0], uv[2], uv[1]]
                flipped += 1
            self.F.append(ids); self.N.append(nr); self.UV.append(uv)
        return flipped

    def fnormal(self, f, repl=None):
        a, b, c = (self.P[repl[1]] if repl and i == repl[0] else self.P[i] for i in self.F[f])
        return cross(sub(b, a), sub(c, a))

    def decimate(self, eps):
        P, F, N = self.P, self.F, self.N
        nv = len(P)
        vf = [set() for _ in range(nv)]
        for f, t in enumerate(F):
            for i in t:
                vf[i].add(f)
        Q = [[0.0] * 10 for _ in range(nv)]

        def add_plane(i, n, d, w=1.0):
            q = Q[i]; a, b, c = n
            q[0] += w * a * a; q[1] += w * a * b; q[2] += w * a * c; q[3] += w * a * d
            q[4] += w * b * b; q[5] += w * b * c; q[6] += w * b * d
            q[7] += w * c * c; q[8] += w * c * d; q[9] += w * d * d

        edges = {}
        for f, t in enumerate(F):
            n = unit(self.fnormal(f))
            d = -dot(n, P[t[0]])
            for k in range(3):
                add_plane(t[k], n, d)
                e = (min(t[k], t[(k + 1) % 3]), max(t[k], t[(k + 1) % 3]))
                edges.setdefault(e, []).append(f)
        for (a, b), fs in edges.items():   # open borders: a plane through the edge, square to the face
            if len(fs) == 1:
                n = unit(cross(sub(P[b], P[a]), unit(self.fnormal(fs[0]))))
                d = -dot(n, P[a])
                add_plane(a, n, d, 4.0); add_plane(b, n, d, 4.0)

        def err(q, p):
            x, y, z = p
            return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z
                    + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9])

        ver = [0] * nv
        lim = eps * eps
        heap = []

        def push(u, v):   # u goes to v
            c = err(Q[u], P[v])
            if c <= lim:
                heapq.heappush(heap, (c, u, v, ver[u], ver[v]))

        for (a, b) in edges:
            push(a, b); push(b, a)
        del edges
        removed = 0
        cos_keep = math.cos(math.radians(12.0))
        cos_flat = math.cos(math.radians(2.0))
        while heap:
            c, u, v, su, sv = heapq.heappop(heap)
            if ver[u] != su or ver[v] != sv:
                continue
            shared = vf[u] & vf[v]
            if not shared:
                continue
            # link condition: the only common neighbours are the tips of the shared faces
            nu = {i for f in vf[u] for i in F[f]}; nvs = {i for f in vf[v] for i in F[f]}
            if len((nu & nvs) - {u, v}) != len(shared) or len(shared) > 2:
                continue
            # the corners that move to v take v's normal where one matches closely (a
            # smooth surface), else keep their own (a hard edge); no face may end up
            # shaded further from its own plane than it was (a tilted normal on one
            # corner of a large flat face shows as a gradient across all of it)
            vn = [N[f][F[f].index(v)] for f in vf[v] - shared]
            ok = True
            moved = {}
            for f in vf[u] - shared:
                n0 = self.fnormal(f); n1 = self.fnormal(f, (u, v))
                l0 = math.sqrt(dot(n0, n0)); l1 = math.sqrt(dot(n1, n1))
                if l1 < 1e-9 or dot(n0, n1) < 0.8 * l0 * l1:   # no flips, no slivers folding over
                    ok = False; break
                g0 = (n0[0] / l0, n0[1] / l0, n0[2] / l0); g1 = (n1[0] / l1, n1[1] / l1, n1[2] / l1)
                k = F[f].index(u)
                nu_ = N[f][k]
                best = max(vn, key=lambda x: dot(x, nu_), default=None)
                cand = best if best is not None and dot(best, nu_) >= cos_keep else nu_
                moved[f] = (k, cand)
                for j in range(3):
                    nj = cand if j == k else N[f][j]
                    if dot(nj, g1) < min(dot(N[f][j], g0), cos_flat) - 0.004:
                        ok = False; break
                if not ok:
                    break
            if not ok:
                continue
            for f in shared:
                for i in F[f]:
                    vf[i].discard(f)
                F[f] = None
                removed += 1
            for f in list(vf[u]):
                k, cand = moved[f]
                F[f][k] = v
                N[f][k] = cand
                vf[v].add(f)
            vf[u] = set()
            qv, qu = Q[v], Q[u]
            for i in range(10):
                qv[i] += qu[i]
            ver[u] += 1; ver[v] += 1
            for w in {i for f in vf[v] for i in F[f]} - {v}:
                ver[w] += 1
                push(w, v); push(v, w)
                for x in {i for f in vf[w] for i in F[f]} - {w, v}:
                    push(w, x); push(x, w)
        return removed

    def tris(self):
        for f, t in enumerate(self.F):
            if t is not None:
                yield [(self.P[t[k]], self.N[f][k], self.UV[f][k]) for k in range(3)]


def optimize(src, dst, eps):
    parts = load_v1(src)
    groups, order = {}, []
    for name, mat, vs in parts:
        if mat not in groups:
            groups[mat] = [name, []]; order.append(mat)
        groups[mat][1].append(vs)
    order.sort(key=lambda m: ORDER.get(m[10], 0))   # stable: the scene order within a class
    verts, vmap, idx, out_parts = [], {}, [], []
    t_in = t_out = flipped = 0
    for mat in order:
        name, soups = groups[mat]
        kind = mat[10]
        m = Mesh()
        for vs in soups:
            flipped += m.add_soup(vs, kind in PRINTED)
            t_in += len(vs) // 24
        if kind not in PRINTED and eps > 0:
            m.decimate(eps)
        first = len(idx)
        for tri in m.tris():
            for p, n, uv in tri:
                key = (struct.pack("<3f", *p), tuple(max(-127, min(127, round(c * 127))) for c in n),
                       tuple(max(0, min(65535, round(c * 65535))) for c in uv))
                i = vmap.get(key)
                if i is None:
                    i = vmap[key] = len(verts); verts.append(key)
                idx.append(i)
        t_out += (len(idx) - first) // 3
        if len(idx) > first:
            out_parts.append((name, mat, first, len(idx) - first))
    if len(verts) > 65535:
        raise SystemExit(f"{src}: {len(verts)} vertices do not fit 16-bit indices (raise --eps)")
    with open(dst, "wb") as f:
        f.write(b"LMDL" + struct.pack("<4I", 2, len(out_parts), len(verts), len(idx)))
        for name, mat, first, count in out_parts:
            f.write(name.encode()[:47].ljust(48, b"\0") + struct.pack("<10f3I7f", *mat) + struct.pack("<2I", first, count))
        for p, n, uv in verts:
            f.write(p + struct.pack("<3bx2H", *n, *uv))
        f.write(struct.pack(f"<{len(idx)}H", *idx))
    print(f"[optimize] {os.path.basename(dst)[:-5]:13s} parts {len(parts):3d} -> {len(out_parts):2d}, triangles {t_in:6d} -> {t_out:6d}, "
          f"vertices {t_in * 3:6d} -> {len(verts):5d}, {os.path.getsize(src) // 1024:5d} -> {os.path.getsize(dst) // 1024:4d} kB"
          + (f", {flipped} faces turned" if flipped else ""))


def info(d):
    tt = tb = 0
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".lmdl"):
            continue
        raw = open(os.path.join(d, fn), "rb").read()
        ver, n = struct.unpack_from("<II", raw, 4)
        if ver == 1:
            tris = sum(len(vs) // 24 for _, _, vs in load_v1(os.path.join(d, fn))); nv = tris * 3
        else:
            nv, ni = struct.unpack_from("<II", raw, 12); tris = ni // 3
        tt += tris; tb += len(raw)
        print(f"{fn[:-5]:13s} v{ver} parts {n:3d} triangles {tris:6d} vertices {nv:6d} {len(raw) / 1024:7.0f} kB")
    print(f"{'total':13s}    {'':9s} triangles {tt:6d} {'':15s} {tb / 1024:7.0f} kB")


def main():
    a = sys.argv[1:]
    if a[:1] == ["--info"] and len(a) == 2:
        return info(a[1])
    eps = None
    if "--eps" in a:
        i = a.index("--eps"); eps = float(a[i + 1]); del a[i:i + 2]
    if len(a) < 2:
        raise SystemExit(__doc__)
    src, dst, keys = a[0], a[1], a[2:]
    os.makedirs(dst, exist_ok=True)
    for fn in sorted(os.listdir(src)):
        if fn.endswith(".lmdl") and (not keys or fn[:-5] in keys):
            optimize(os.path.join(src, fn), os.path.join(dst, fn), eps if eps is not None else 0.10 if fn.startswith("console_") else 0.05)


if __name__ == "__main__":
    main()
