# Library for groundloop_solve.py (no command line): grid rasteriser and sparse DC conductance model of one net.
"""DC copper resistance model of one net (GND) on an N-layer board.

Cells: square grid of pitch P (mm). Each layer is rasterised at P/SUB and block-averaged to a copper
coverage f in [0,1]. In-plane conductance between neighbouring cells = (t/rho) * harmonic_mean(f1, f2)
(exact for a strip that fills a row/column partially). THT pads (soldered leads) are one equipotential
node across all layers; SMD pads and gold fingers are equipotential on their own layer; vias link the
layers with a barrel resistance rho*L/(pi*(d+tp)*tp).
"""
import numpy as np
import scipy.sparse as sp
from PIL import Image, ImageDraw
from shapely.geometry import Point, Polygon, MultiPolygon, box
import shapely

RHO = 1.72e-8  # ohm*m, copper at 20 C (IEC 60028 annealed: 1.7241e-8)


class Grid:
    def __init__(self, bounds, P, SUB=4):
        x0, y0, x1, y1 = bounds
        self.P, self.SUB = P, SUB
        self.x0, self.y0 = x0 - P, y0 - P
        self.nx = int(np.ceil((x1 - self.x0) / P)) + 2
        self.ny = int(np.ceil((y1 - self.y0) / P)) + 2

    def cell_of(self, x, y):
        return int((y - self.y0) / self.P), int((x - self.x0) / self.P)

    def centers(self):
        xs = self.x0 + (np.arange(self.nx) + 0.5) * self.P
        ys = self.y0 + (np.arange(self.ny) + 0.5) * self.P
        return xs, ys

    def coverage(self, geom):
        """block-averaged coverage of a shapely (Multi)Polygon"""
        S = self.SUB
        W, H = self.nx * S, self.ny * S
        img = Image.new('L', (W, H), 0)
        dr = ImageDraw.Draw(img)
        sc = S / self.P
        polys = []
        if geom.is_empty:
            return np.zeros((self.ny, self.nx))
        if isinstance(geom, Polygon):
            polys = [geom]
        else:
            polys = [g for g in geom.geoms if isinstance(g, Polygon)]
        polys.sort(key=lambda g: -g.area)

        def tr(coords):
            return [((x - self.x0) * sc, (y - self.y0) * sc) for x, y in coords]
        for g in polys:
            dr.polygon(tr(g.exterior.coords), fill=255)
            for h in g.interiors:
                dr.polygon(tr(h.coords), fill=0)
        a = np.asarray(img, dtype=np.float32) / 255.0
        return a.reshape(self.ny, S, self.nx, S).mean(axis=(1, 3))

    def mask(self, geom, thr=0.5):
        return self.coverage(geom) >= thr

    def coverage_local(self, geom):
        """coverage on the window of cells around geom: returns (i0, j0, cov)"""
        S, P = self.SUB, self.P
        bx0, by0, bx1, by1 = geom.bounds
        j0 = max(int((bx0 - self.x0) / P) - 1, 0); i0 = max(int((by0 - self.y0) / P) - 1, 0)
        j1 = min(int((bx1 - self.x0) / P) + 2, self.nx); i1 = min(int((by1 - self.y0) / P) + 2, self.ny)
        W, H = (j1 - j0) * S, (i1 - i0) * S
        img = Image.new('L', (W, H), 0)
        dr = ImageDraw.Draw(img)
        sc = S / P
        ox, oy = self.x0 + j0 * P, self.y0 + i0 * P
        polys = [geom] if isinstance(geom, Polygon) else [g for g in geom.geoms if isinstance(g, Polygon)]
        polys.sort(key=lambda g: -g.area)
        for g in polys:
            dr.polygon([((x - ox) * sc, (y - oy) * sc) for x, y in g.exterior.coords], fill=255)
            for h in g.interiors:
                dr.polygon([((x - ox) * sc, (y - oy) * sc) for x, y in h.coords], fill=0)
        a = np.asarray(img, dtype=np.float32) / 255.0
        return i0, j0, a.reshape(i1 - i0, S, j1 - j0, S).mean(axis=(1, 3))


class Model:
    def __init__(self, grid, layers):
        """layers: list of dicts {name, cov (ny,nx), t (m)}"""
        self.g = grid
        self.layers = layers
        nl, ny, nx = len(layers), grid.ny, grid.nx
        self.nl = nl
        self.cell_id = -np.ones((nl, ny, nx), dtype=np.int64)
        cu = np.stack([L['cov'] > 0.02 for L in layers])
        self.cell_id[cu] = np.arange(cu.sum())
        self.ncell = int(cu.sum())
        self.parent = np.arange(self.ncell)
        self.extra_edges = []  # (a, b, G)
        self.named = {}

    # union-find for equipotential merges
    def find(self, a):
        p = self.parent
        r = a
        while p[r] != r:
            r = p[r]
        while p[a] != r:
            p[a], a = r, p[a]
        return r

    def merge(self, ids):
        ids = [int(i) for i in ids if i >= 0]
        if not ids:
            return None
        r = self.find(ids[0])
        for i in ids[1:]:
            ri = self.find(i)
            if ri != r:
                self.parent[ri] = r
        return r

    def cells_in(self, li, geom, thr=0.3):
        i0, j0, cov = self.g.coverage_local(geom)
        ids = self.cell_id[li, i0:i0 + cov.shape[0], j0:j0 + cov.shape[1]]
        m = (cov >= thr) & (ids >= 0)
        if m.any():
            return ids[m]
        if True:  # tiny geometry: take nearest copper cell to centroid
            c = geom.centroid
            i, j = self.g.cell_of(c.x, c.y)
            win = self.cell_id[li, max(i - 3, 0):i + 4, max(j - 3, 0):j + 4]
            return win[win >= 0].ravel()

    def link(self, a, b, G):
        self.extra_edges.append((int(a), int(b), float(G)))

    def assemble(self):
        nx, ny = self.g.nx, self.g.ny
        rows, cols, vals = [], [], []
        for li, L in enumerate(self.layers):
            cov = L['cov']
            gs = L['t'] / RHO
            ids = self.cell_id[li]
            for ax in (0, 1):
                if ax == 1:
                    a, b = ids[:, :-1], ids[:, 1:]
                    fa, fb = cov[:, :-1], cov[:, 1:]
                else:
                    a, b = ids[:-1, :], ids[1:, :]
                    fa, fb = cov[:-1, :], cov[1:, :]
                m = (a >= 0) & (b >= 0)
                hm = 2 * fa[m] * fb[m] / (fa[m] + fb[m])
                rows.append(a[m]); cols.append(b[m]); vals.append(gs * hm)
        for a, b, G in self.extra_edges:
            rows.append(np.array([a])); cols.append(np.array([b])); vals.append(np.array([G]))
        r = np.concatenate(rows); c = np.concatenate(cols); v = np.concatenate(vals)
        # map through union-find
        par = self.parent
        root = np.array([self.find(i) for i in range(self.ncell)]) if self.ncell < 0 else None
        # vectorised path compression
        root = par.copy()
        while True:
            nr = root[root]
            if np.array_equal(nr, root):
                break
            root = nr
        self.root = root
        uniq, inv = np.unique(root, return_inverse=True)
        self.nnode = len(uniq)
        self.node_of_cell = inv
        r, c = inv[r], inv[c]
        keep = r != c
        r, c, v = r[keep], c[keep], v[keep]
        n = self.nnode
        G = sp.coo_matrix((np.concatenate([-v, -v]), (np.concatenate([r, c]), np.concatenate([c, r]))), shape=(n, n)).tocsr()
        d = -np.asarray(G.sum(axis=1)).ravel()
        self.G = (G + sp.diags(d)).tocsr()
        return self.G

    def node(self, cell):
        return self.node_of_cell[int(cell)]

    def solve(self, gnd_node, injections, tol=1e-11):
        """injections: list of dicts {node: current_A}; returns list of potential arrays (V)"""
        import pyamg
        n = self.nnode
        keep = np.ones(n, bool); keep[gnd_node] = False
        idx = np.flatnonzero(keep)
        A = self.G[idx][:, idx].tocsr()
        # drop isolated islands (no path to ground): detect via connected components
        ncomp, lab = sp.csgraph.connected_components(self.G, directed=False)
        glab = lab[gnd_node]
        conn = lab[idx] == glab
        idx2 = idx[conn]
        A = self.G[idx2][:, idx2].tocsr()
        ml = pyamg.smoothed_aggregation_solver(A, symmetry='symmetric', max_coarse=500)
        out = []
        pos = -np.ones(n, dtype=np.int64); pos[idx2] = np.arange(len(idx2))
        for inj in injections:
            bvec = np.zeros(len(idx2))
            for nd, I in inj.items():
                if pos[nd] >= 0:
                    bvec[pos[nd]] += I
                else:
                    raise ValueError('injection node not connected to ground')
            res = []
            x = ml.solve(bvec, tol=tol, accel='cg', residuals=res, maxiter=3000)
            rel = np.linalg.norm(A @ x - bvec) / max(np.linalg.norm(bvec), 1e-30)
            if not (rel < 1e-6) and len(idx2) < 400000:
                import scipy.sparse.linalg as spl
                x = spl.spsolve(A.tocsc(), bvec)
                rel = np.linalg.norm(A @ x - bvec) / max(np.linalg.norm(bvec), 1e-30)
            if not (rel < 1e-8):
                # fx-fb3 patch: AMG-CG can break down (seen once at rel 1e+30); retry with AMG-preconditioned scipy CG,
                # then AMG-GMRES, keep the best
                import scipy.sparse.linalg as spl
                print('  solve retry, rel', rel, flush=True)
                cands = [(rel, x)]
                try:
                    x2, info = spl.cg(A, bvec, M=ml.aspreconditioner(cycle='V'), tol=1e-12, maxiter=4000)
                    cands.append((np.linalg.norm(A @ x2 - bvec) / max(np.linalg.norm(bvec), 1e-30), x2))
                except Exception as e:
                    print('  cg failed', e)
                if not (min(c[0] for c in cands if np.isfinite(c[0])) < 1e-8):
                    x3 = ml.solve(bvec, tol=tol, accel='gmres', maxiter=400)
                    cands.append((np.linalg.norm(A @ x3 - bvec) / max(np.linalg.norm(bvec), 1e-30), x3))
                cands = [c for c in cands if np.isfinite(c[0])]
                rel, x = min(cands, key=lambda c: c[0])
                print('  solve retry -> rel', rel, flush=True)
            self.max_rel_residual = max(getattr(self, 'max_rel_residual', 0.0), float(rel))
            V = np.full(n, np.nan); V[gnd_node] = 0.0; V[idx2] = x
            out.append(V)
        self.island_nodes = int((~conn).sum())
        return out
