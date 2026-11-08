"""Fits the LTC table for Rect / Disc Lights: run `python tools/ltc_fit.py` from the repo root.

LTC (linearly transformed cosines, Heitz et al. 2016): for each roughness and view angle a 3x3 matrix M
bends a clamped cosine into GGX's specular lobe (Fresnel 1; env_brdf scales it in the shader). The shader
keeps M's inverse as (m00, m02, m11, m20) with m22 = 1, rows by roughness 0..1, columns by sqrt(1 - n.v).
The script rewrites the table between the `ltc table` markers in engine/rhi/shaders/mesh.hlsl and the GLSL
mirror in engine/rhi/backends/opengl/opengl_device.cpp, then checks a few rects against Monte Carlo.
"""
import os
import re
import numpy as np

N = 16            # table size (N x N)
SAMPLES = 4096    # per distribution and error estimate
ALPHA_MIN = 0.01  # shortcut: smoother rows than roughness 0.1 reuse its lobe, fit sharper ones if mirrors need exact panels
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
rng = np.random.default_rng(7)
U_GGX, U_LTC = rng.random((SAMPLES, 2)), rng.random((SAMPLES, 2))


def unit(x):
    return x / np.linalg.norm(x, axis=-1, keepdims=True)


def smith_lambda(c, a2):
    c = np.clip(c, 1e-6, 1.0)
    return (-1.0 + np.sqrt(1.0 + a2 * (1.0 - c * c) / (c * c))) * 0.5


def ggx_d(ndh, a2):
    return a2 / (np.pi * (ndh * ndh * (a2 - 1.0) + 1.0) ** 2)


def ggx_eval(v, l, alpha):  # specular BRDF times n.l (F = 1, height-correlated Smith)
    a2 = alpha * alpha
    h = unit(v + l)
    val = ggx_d(h[:, 2], a2) / (1.0 + smith_lambda(v[2], a2) + smith_lambda(l[:, 2], a2)) / (4.0 * v[2])
    return np.where(l[:, 2] > 0.0, val, 0.0)


def ggx_sample(v, alpha, u):
    phi = 2.0 * np.pi * u[:, 0]
    t2 = alpha * alpha * u[:, 1] / (1.0 - u[:, 1])
    c = 1.0 / np.sqrt(1.0 + t2)
    s = np.sqrt(1.0 - c * c)
    h = np.stack([s * np.cos(phi), s * np.sin(phi), c], 1)
    return 2.0 * (h @ v)[:, None] * h - v


def ggx_pdf(v, l, alpha):
    h = unit(v + l)
    return ggx_d(h[:, 2], alpha * alpha) * h[:, 2] / (4.0 * np.abs(h @ v))


def matrix(p):
    return np.array([[p[0], 0.0, p[1]], [0.0, p[2], 0.0], [p[3], 0.0, 1.0]])


def ltc_eval(minv, l):  # the clamped cosine taken through M: D_o(M^-1 l) |det M^-1| / |M^-1 l|^3
    lo = l @ minv.T
    length = np.linalg.norm(lo, axis=1)
    return np.maximum(lo[:, 2] / length, 0.0) / np.pi * abs(np.linalg.det(minv)) / length ** 3


def ltc_sample(m, u):
    r, phi = np.sqrt(u[:, 0]), 2.0 * np.pi * u[:, 1]
    o = np.stack([r * np.cos(phi), r * np.sin(phi), np.sqrt(1.0 - u[:, 0])], 1)
    return unit(o @ m.T)


class Cell:
    def __init__(self, ndv, alpha):
        self.v = np.array([np.sqrt(1.0 - ndv * ndv), 0.0, ndv])
        self.alpha = alpha
        self.l_ggx = ggx_sample(self.v, alpha, U_GGX)
        self.p_ggx = ggx_pdf(self.v, self.l_ggx, alpha)
        self.f_ggx = ggx_eval(self.v, self.l_ggx, alpha)
        self.norm = np.mean(self.f_ggx / self.p_ggx)  # the lobe's integral: its share kept by the table is 1

    def error(self, p):  # L3 between the normalized GGX lobe and the LTC, both sample sets (balance heuristic)
        m = matrix(p)
        if abs(np.linalg.det(m)) < 1e-9 or p[0] <= 0.0 or p[2] <= 0.0:
            return 1e30
        minv = np.linalg.inv(m)
        l_ltc = ltc_sample(m, U_LTC)
        total = 0.0
        for l, p_ggx, f in ((self.l_ggx, self.p_ggx, self.f_ggx), (l_ltc, None, None)):
            if p_ggx is None:
                p_ggx = ggx_pdf(self.v, l, self.alpha)
                f = ggx_eval(self.v, l, self.alpha)
            g = ltc_eval(minv, l)
            f = f / self.norm
            total += np.sum(np.abs(f - g) ** 3 / (p_ggx + g + 1e-30))
        return total / SAMPLES


def nelder_mead(fn, x0, step, iters=600):
    pts = [np.array(x0, float)] + [np.array(x0, float) + np.eye(len(x0))[i] * step[i] for i in range(len(x0))]
    vals = [fn(p) for p in pts]
    for _ in range(iters):
        order = np.argsort(vals)
        pts, vals = [pts[i] for i in order], [vals[i] for i in order]
        centre = np.mean(pts[:-1], 0)
        r = centre + (centre - pts[-1])
        fr = fn(r)
        if fr < vals[0]:
            e = centre + 2.0 * (centre - pts[-1])
            fe = fn(e)
            pts[-1], vals[-1] = (e, fe) if fe < fr else (r, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = r, fr
        else:
            c = centre + 0.5 * (pts[-1] - centre)
            fc = fn(c)
            if fc < vals[-1]:
                pts[-1], vals[-1] = c, fc
            else:
                pts = [pts[0] + 0.5 * (p - pts[0]) for p in pts]
                vals = [fn(p) for p in pts]
    best = int(np.argmin(vals))
    return pts[best], vals[best]


def cell_params(row, col):
    roughness = row / (N - 1)
    u = col / (N - 1)
    return max(1.0 - u * u, 0.02), max(roughness * roughness, ALPHA_MIN)


def fit():
    table = np.zeros((N, N, 4))
    above = None
    for row in range(N - 1, -1, -1):  # rough to smooth, each from its neighbour's fit
        prev = None
        for col in range(N):
            ndv, alpha = cell_params(row, col)
            cell = Cell(ndv, alpha)
            if prev is not None:
                x0 = prev
            elif above is not None:
                x0 = above * np.array([alpha / above_alpha, 1.0, alpha / above_alpha, 1.0])
            else:
                x0 = np.array([alpha, 0.0, alpha, 0.0])
            step = np.array([0.1, 0.1, 0.1, 0.1]) * max(alpha, 0.05)
            p, _ = nelder_mead(cell.error, x0, step)
            if col == 0:  # straight on the lobe is round
                p = np.array([(p[0] + p[2]) * 0.5, 0.0, (p[0] + p[2]) * 0.5, 0.0])
                above, above_alpha = p.copy(), alpha
            prev = p
            minv = np.linalg.inv(matrix(p))
            minv /= minv[2, 2]
            table[row, col] = (minv[0, 0], minv[0, 2], minv[1, 1], minv[2, 0])
        print('row %d / %d' % (N - row, N), flush=True)
    return table


def polygon_share(p):  # the shader's polygon_share: clip to z >= 0, Lambert's edge sum
    c = []
    for e in range(len(p)):
        a, b = p[e], p[(e + 1) % len(p)]
        if a[2] >= 0:
            c.append(a)
        if (a[2] >= 0) != (b[2] >= 0):
            c.append(a + (b - a) * (a[2] / (a[2] - b[2])))
    total = 0.0
    for k in range(len(c)):
        a, b = c[k] / np.linalg.norm(c[k]), c[(k + 1) % len(c)] / np.linalg.norm(c[(k + 1) % len(c)])
        x = np.clip(a @ b, -0.9999, 0.9999)
        total += np.cross(a, b)[2] * np.arccos(x) / np.sqrt(1.0 - x * x)
    return abs(total) / (2.0 * np.pi)


def lookup(table, roughness, ndv):  # the shader's ltc_inverse (bilinear)
    p = np.array([np.sqrt(1.0 - ndv), roughness]) * (N - 1)
    c = np.minimum(np.floor(p), N - 2).astype(int)
    f = p - c
    t = table
    return (t[c[1], c[0]] * (1 - f[0]) + t[c[1], c[0] + 1] * f[0]) * (1 - f[1]) + (t[c[1] + 1, c[0]] * (1 - f[0]) + t[c[1] + 1, c[0] + 1] * f[0]) * f[1]


def check(table):  # a rect's share of the lobe: table + polygon vs Monte Carlo over GGX samples
    worst = 0.0
    big = np.random.default_rng(3).random((200000, 2))
    for roughness, ndv, offset, size in ((0.5, 0.8, (0.3, 0.2), 1.5), (0.3, 0.5, (0.0, 0.0), 0.6), (0.8, 0.9, (-0.5, 0.4), 2.0),
                                         (0.25, 0.3, (0.1, -0.1), 0.5), (0.15, 0.7, (0.0, 0.0), 0.3), (0.6, 0.2, (0.2, 0.0), 1.5)):
        alpha = max(roughness * roughness, ALPHA_MIN)
        v = np.array([np.sqrt(1.0 - ndv * ndv), 0.0, ndv])
        r = np.array([-v[0], 0.0, v[2]])  # a rect facing down, 2 m along the mirror direction (in the lobe)
        c = r * 2.0 + np.array([offset[0], offset[1], 0.0])
        corners = [c + np.array([dx, dy, 0.0]) * size * 0.5 for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        m = lookup(table, roughness, ndv)
        bent = [np.array([m[0] * q[0] + m[1] * q[2], m[2] * q[1], m[3] * q[0] + q[2]]) for q in corners]
        ltc = polygon_share(bent)
        l = ggx_sample(v, alpha, big)
        f, pdf = ggx_eval(v, l, alpha), ggx_pdf(v, l, alpha)
        t = np.where(l[:, 2] > 1e-6, c[2] / np.maximum(l[:, 2], 1e-6), -1.0)  # the rect lies in the plane z = c.z
        hit = (t > 0) & (np.abs(l[:, 0] * t - c[0]) <= size * 0.5) & (np.abs(l[:, 1] * t - c[1]) <= size * 0.5)
        mc = np.mean(np.where(hit, f / pdf, 0.0)) / np.mean(f / pdf)
        print('roughness %.2f n.v %.2f: LTC %.4f, Monte Carlo %.4f' % (roughness, ndv, ltc, mc))
        worst = max(worst, abs(ltc - mc) / (mc + 0.02))  # relative, absolute near 0
    print('worst relative error %.1f%%' % (worst * 100.0))
    return worst


def write(table):
    flat = table.reshape(-1, 4)
    hlsl = 'static const float4 ltc_table[%d] = {\n' % len(flat) + ''.join(
        '    float4(%.5f, %.5f, %.5f, %.5f),\n' % tuple(r) for r in flat) + '};\n'
    glsl = 'const vec4 ltc_table[%d] = vec4[](\n' % len(flat) + ',\n'.join(
        '    vec4(%.5f, %.5f, %.5f, %.5f)' % tuple(r) for r in flat) + ');\n'
    for path, body in (('engine/rhi/shaders/mesh.hlsl', hlsl), ('engine/rhi/backends/opengl/opengl_device.cpp', glsl)):
        full = os.path.join(ROOT, path)
        text = open(full, encoding='utf-8').read()
        text, count = re.subn(r'(// ltc table begin[^\n]*\n).*?(// ltc table end)', lambda m: m.group(1) + body + m.group(2), text, flags=re.S)
        assert count == 1, path
        open(full, 'w', encoding='utf-8', newline='').write(text)


def read():  # the table as written in mesh.hlsl
    text = open(os.path.join(ROOT, 'engine/rhi/shaders/mesh.hlsl'), encoding='utf-8').read()
    body = re.search(r'ltc_table\[\d+\] = \{(.*?)\};', text, re.S).group(1)
    return np.array([float(x) for x in re.findall(r'-?\d+\.\d+', body)]).reshape(N, N, 4)


if __name__ == '__main__':  # `--check`: test the table in the shaders without fitting
    import sys
    t = read() if '--check' in sys.argv else fit()
    worst = check(t)
    assert worst < 0.15, 'fit too far from GGX'
    if '--check' not in sys.argv:
        write(t)
