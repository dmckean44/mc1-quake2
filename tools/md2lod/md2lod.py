#!/usr/bin/env python3
"""Build reduced-polygon versions of the MD2 models in a Quake II pak file.

Vertices are removed with half-edge collapses ranked by quadric error summed
over every animation frame, so the surviving vertices keep their original
compressed positions and normals in all frames. A vertex on a texture seam is
only collapsed along the seam, so each side keeps its own skin coordinates,
and vertices on open mesh borders are never removed.

The output is a pak file that the engine searches before pak0.pak. By default
the simplified models replace the originals; with --suffix _lod they are
stored as foo_lod.md2 instead, and the renderer switches to them for models
that are small on screen (see the sw_lodsize cvar).
"""

import argparse
import fnmatch
import heapq
import struct
import sys

import numpy as np

MD2_HEADER = struct.Struct("<4s16i")


class Md2:
    def __init__(self, data):
        (ident, version, self.skinwidth, self.skinheight, self.framesize,
         num_skins, num_xyz, num_st, num_tris, num_glcmds, num_frames,
         ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_glcmds, ofs_end) = MD2_HEADER.unpack_from(data)
        if ident != b"IDP2" or version != 8:
            raise ValueError("not an MD2 file")
        self.skins = [data[ofs_skins + i * 64:ofs_skins + (i + 1) * 64] for i in range(num_skins)]
        self.st = np.frombuffer(data, np.int16, num_st * 2, ofs_st).reshape(-1, 2).copy()
        tris = np.frombuffer(data, np.uint16, num_tris * 6, ofs_tris).reshape(-1, 6)
        self.tri_xyz = tris[:, :3].astype(np.int64)
        self.tri_st = tris[:, 3:].astype(np.int64)
        self.frame_headers = []
        self.frame_verts = np.empty((num_frames, num_xyz, 4), np.uint8)
        for f in range(num_frames):
            base = ofs_frames + f * self.framesize
            self.frame_headers.append(data[base:base + 40])
            self.frame_verts[f] = np.frombuffer(data, np.uint8, num_xyz * 4, base + 40).reshape(-1, 4)

    def positions(self):
        hdr = np.array([struct.unpack("<6f", h[:24]) for h in self.frame_headers], np.float64)
        scale, translate = hdr[:, None, 0:3], hdr[:, None, 3:6]
        return self.frame_verts[:, :, :3].astype(np.float64) * scale + translate

    def serialize(self):
        num_xyz = self.frame_verts.shape[1]
        framesize = 40 + num_xyz * 4
        glcmds = []
        for (a, b, c), (sa, sb, sc) in zip(self.tri_xyz, self.tri_st):
            glcmds.append(3)
            for v, s in ((a, sa), (b, sb), (c, sc)):
                u = (self.st[s, 0] + 0.5) / self.skinwidth
                t = (self.st[s, 1] + 0.5) / self.skinheight
                glcmds += [struct.unpack("<i", struct.pack("<f", u))[0],
                           struct.unpack("<i", struct.pack("<f", t))[0], int(v)]
        glcmds.append(0)

        ofs_skins = MD2_HEADER.size
        ofs_st = ofs_skins + 64 * len(self.skins)
        ofs_tris = ofs_st + 4 * len(self.st)
        ofs_frames = ofs_tris + 12 * len(self.tri_xyz)
        ofs_glcmds = ofs_frames + framesize * len(self.frame_headers)
        ofs_end = ofs_glcmds + 4 * len(glcmds)

        out = bytearray(MD2_HEADER.pack(
            b"IDP2", 8, self.skinwidth, self.skinheight, framesize,
            len(self.skins), num_xyz, len(self.st), len(self.tri_xyz), len(glcmds),
            len(self.frame_headers), ofs_skins, ofs_st, ofs_tris, ofs_frames,
            ofs_glcmds, ofs_end))
        for skin in self.skins:
            out += skin
        out += self.st.astype("<i2").tobytes()
        tris = np.concatenate([self.tri_xyz, self.tri_st], axis=1).astype("<u2")
        out += tris.tobytes()
        for hdr, verts in zip(self.frame_headers, self.frame_verts):
            out += hdr + verts.tobytes()
        out += struct.pack("<%di" % len(glcmds), *glcmds)
        return bytes(out)


def plane_quadrics(pos, tri):
    """Per-frame, per-vertex error quadrics from area-weighted triangle planes."""
    num_frames, num_verts = pos.shape[:2]
    p0, p1, p2 = pos[:, tri[:, 0]], pos[:, tri[:, 1]], pos[:, tri[:, 2]]
    n = np.cross(p1 - p0, p2 - p0)
    area = np.linalg.norm(n, axis=2, keepdims=True)
    n = n / np.maximum(area, 1e-12)
    plane = np.concatenate([n, -np.sum(n * p0, axis=2, keepdims=True)], axis=2)
    k = plane[:, :, :, None] * plane[:, :, None, :] * area[:, :, :, None]
    q = np.zeros((num_frames, num_verts, 4, 4))
    for corner in range(3):
        np.add.at(q, (slice(None), tri[:, corner]), k)
    return q


class Simplifier:
    def __init__(self, md2, min_normal_dot, seam_weight, uv_tolerance):
        self.md2 = md2
        self.seam_weight = seam_weight
        self.uv_tolerance = uv_tolerance
        self.pos = md2.positions()
        self.pos_h = np.concatenate([self.pos, np.ones(self.pos.shape[:2] + (1,))], axis=2)
        self.quadric = plane_quadrics(self.pos, md2.tri_xyz)
        self.min_normal_dot = min_normal_dot
        self.tri_xyz = md2.tri_xyz.copy()
        self.tri_st = md2.tri_st.copy()
        self.alive = np.ones(len(self.tri_xyz), bool)
        num_verts = self.pos.shape[1]
        self.vert_tris = [set() for _ in range(num_verts)]
        for t, (a, b, c) in enumerate(self.tri_xyz):
            for v in (a, b, c):
                self.vert_tris[v].add(t)
        self.locked = np.zeros(num_verts, bool)
        self.seam = np.array([len(self.wedges(v)) > 1 for v in range(num_verts)])
        self._lock_borders()

    def st_at(self, t, v):
        return int(self.tri_st[t][list(self.tri_xyz[t]).index(v)])

    def st_value(self, t, v):
        return tuple(self.md2.st[self.st_at(t, v)])

    def wedges(self, v):
        groups = {}
        for t in self.vert_tris[v]:
            groups.setdefault(self.st_value(t, v), set()).add(t)
        return groups

    def _lock_borders(self):
        edge_count = {}
        for a, b, c in self.tri_xyz:
            for e in ((a, b), (b, c), (c, a)):
                key = (min(e), max(e))
                edge_count[key] = edge_count.get(key, 0) + 1
        for (a, b), n in edge_count.items():
            if n != 2:
                self.locked[a] = self.locked[b] = True

    def neighbours(self, v):
        return {int(w) for t in self.vert_tris[v] for w in self.tri_xyz[t] if w != v}

    def cost(self, u, v):
        p = self.pos_h[:, v]
        c = float(np.einsum("fi,fij,fj->", p, self.quadric[:, u], p))
        return c * self.seam_weight if self.seam[u] else c

    def can_collapse(self, u, v):
        if self.locked[u]:
            return None
        shared = self.vert_tris[u] & self.vert_tris[v]
        if not shared:
            return None
        if self.neighbours(u) & self.neighbours(v) != {
                int(w) for t in shared for w in self.tri_xyz[t] if w != u and w != v}:
            return None
        new_st = {}
        wedges = self.wedges(u)
        for wedge, tris in wedges.items():
            sides = tris & shared
            if not sides or len({self.st_value(t, v) for t in sides}) != 1:
                return None
            new_st[wedge] = self.st_at(next(iter(sides)), v)
        moved = [t for t in self.vert_tris[u] if t not in shared]
        if not moved:
            return None
        tri = self.tri_xyz[moved]
        new_tri = np.where(tri == u, v, tri)
        if not self._orientation_kept(tri, new_tri):
            return None
        remap = {t: new_st[self.st_value(t, u)] for t in moved}
        for wedge, tris in wedges.items():
            if not self._skin_area_kept(tris, remap, u):
                return None
        return remap

    def _uv_area(self, t, replace_u=None):
        st = [self.md2.st[i].astype(np.float64) for i in self.tri_st[t]]
        if replace_u is not None:
            st[list(self.tri_xyz[t]).index(replace_u[0])] = self.md2.st[replace_u[1]]
        (a, b, c) = st
        return 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]))

    def _skin_area_kept(self, tris, remap, u):
        before = after = 0.0
        for t in tris:
            area = self._uv_area(t)
            before += area
            if t in remap:
                new_area = self._uv_area(t, (u, remap[t]))
                if area * new_area < 0 and abs(area) > 0.5:
                    return False
                after += new_area
        return abs(after - before) <= self.uv_tolerance

    def _orientation_kept(self, before, after):
        def normals(tri):
            p0, p1, p2 = (self.pos[:, tri[:, i]] for i in range(3))
            return np.cross(p1 - p0, p2 - p0)
        a, b = normals(before), normals(after)
        la, lb = np.linalg.norm(a, axis=2), np.linalg.norm(b, axis=2)
        if np.any(lb < 1e-9):
            return False
        dot = np.sum(a * b, axis=2) / np.maximum(la * lb, 1e-12)
        return bool(np.all((la < 1e-9) | (dot >= self.min_normal_dot)))

    def collapse(self, u, v, moved):
        for t in self.vert_tris[u] & self.vert_tris[v]:
            self.alive[t] = False
            for w in self.tri_xyz[t]:
                if w != u:
                    self.vert_tris[w].discard(t)
        for t, st in moved.items():
            i = list(self.tri_xyz[t]).index(u)
            self.tri_xyz[t][i] = v
            self.tri_st[t][i] = st
            self.vert_tris[v].add(t)
        self.vert_tris[u] = set()
        self.quadric[:, v] += self.quadric[:, u]

    def run(self, target_tris):
        heap = []
        version = np.zeros(self.pos.shape[1], np.int64)

        def push(u):
            for v in self.neighbours(u):
                if not self.locked[u]:
                    heapq.heappush(heap, (self.cost(u, v), u, v, version[u], version[v]))

        for u in range(self.pos.shape[1]):
            push(u)
        while heap and self.alive.sum() > target_tris:
            c, u, v, vu, vv = heapq.heappop(heap)
            if vu != version[u] or vv != version[v] or not self.vert_tris[u]:
                continue
            ok = self.can_collapse(u, v)
            if ok is None:
                continue
            affected = self.neighbours(u) | {v}
            self.collapse(u, v, ok)
            for w in affected:
                version[w] += 1
            for w in affected:
                push(w)
                for x in self.neighbours(w):
                    if x in affected:
                        continue
                    heapq.heappush(heap, (self.cost(x, w), x, w, version[x], version[w]))

    def result(self):
        tri_xyz = self.tri_xyz[self.alive]
        tri_st = self.tri_st[self.alive]
        used_xyz = np.unique(tri_xyz)
        used_st = np.unique(tri_st)
        remap_xyz = np.full(self.pos.shape[1], -1)
        remap_xyz[used_xyz] = np.arange(len(used_xyz))
        remap_st = np.full(len(self.md2.st), -1)
        remap_st[used_st] = np.arange(len(used_st))
        md2 = self.md2
        md2.tri_xyz = remap_xyz[tri_xyz]
        md2.tri_st = remap_st[tri_st]
        md2.st = md2.st[used_st]
        md2.frame_verts = md2.frame_verts[:, used_xyz]
        return md2


def read_pak(path):
    data = open(path, "rb").read()
    ident, ofs, size = struct.unpack_from("<4sii", data)
    if ident != b"PACK":
        raise ValueError(path + " is not a pak file")
    files = {}
    for i in range(size // 64):
        name, fofs, flen = struct.unpack_from("<56sii", data, ofs + i * 64)
        files[name.split(b"\0")[0].decode()] = data[fofs:fofs + flen]
    return files


def write_pak(path, files):
    body = bytearray()
    directory = bytearray()
    for name, data in files.items():
        directory += struct.pack("<56sii", name.encode(), 12 + len(body), len(data))
        body += data
    with open(path, "wb") as f:
        f.write(struct.pack("<4sii", b"PACK", 12 + len(body), len(directory)))
        f.write(body)
        f.write(directory)


def target_for(name, num_tris, args):
    for rule in args.rule:
        pattern, ratio = rule.split("=")
        if fnmatch.fnmatch(name, pattern):
            return max(args.min_tris, int(num_tris * float(ratio)))
    return max(args.min_tris, int(num_tris * args.ratio))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paks", nargs="+", help="source pak files, later ones override earlier ones")
    parser.add_argument("-o", "--output", required=True, help="output pak file")
    parser.add_argument("-r", "--ratio", type=float, default=0.5, help="fraction of triangles to keep")
    parser.add_argument("--rule", action="append", default=[],
                        help="per-model ratio, e.g. 'models/weapons/v_*=0.6' (first match wins)")
    parser.add_argument("--min-tris", type=int, default=48, help="leave models this small alone")
    parser.add_argument("--min-normal-dot", type=float, default=0.2,
                        help="reject collapses that turn a triangle further than this in any frame")
    parser.add_argument("--seam-weight", type=float, default=2.0,
                        help="cost multiplier for collapsing along texture seams")
    parser.add_argument("--uv-tolerance", type=float, default=2.0,
                        help="max change in skin texels covered around a removed vertex")
    parser.add_argument("--suffix", default="", help="append this to the output model names")
    parser.add_argument("--only", default="*.md2", help="glob of models to process")
    args = parser.parse_args()

    files = {}
    for pak in args.paks:
        files.update(read_pak(pak))

    out = {}
    total_before = total_after = 0
    for name in sorted(files):
        if not name.endswith(".md2") or not fnmatch.fnmatch(name, args.only):
            continue
        md2 = Md2(files[name])
        before = len(md2.tri_xyz)
        target = target_for(name, before, args)
        if before <= args.min_tris or target >= before:
            continue
        simplifier = Simplifier(md2, args.min_normal_dot, args.seam_weight, args.uv_tolerance)
        simplifier.run(target)
        small = simplifier.result()
        after = len(small.tri_xyz)
        total_before += before
        total_after += after
        out[name[:-4] + args.suffix + ".md2"] = small.serialize()
        print(f"{name}: {before} -> {after} triangles, "
              f"{simplifier.pos.shape[1]} -> {small.frame_verts.shape[1]} vertices", file=sys.stderr)

    write_pak(args.output, out)
    print(f"{len(out)} models, {total_before} -> {total_after} triangles", file=sys.stderr)


if __name__ == "__main__":
    main()
