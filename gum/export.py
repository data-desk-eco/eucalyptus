"""gum.bin from orbio's sentinel-2 model: the frozen forward pass as a list
of ops, then the retrieval lookup table. a convolution carries what
follows it alone: a batch norm, as eager torch computes it in inference
(x · a + b, a = w / sqrt(var + eps), b = bias - mean · a), and then an
activation. op i's output is value i + 1; value 0 is the input. ints are
int32, weights float32, the table float64.
usage: export.py MODEL.pth LUT.csv OUT"""
import sys, numpy as np, torch

m = torch.load(sys.argv[1], weights_only=False, map_location='cpu').eval()
with torch.no_grad():
    g = torch.jit.freeze(torch.jit.trace(m, torch.rand(1, 24, 64, 64)),
                         optimize_numerics=False).graph
K = {'aten::_convolution': 'C', 'aten::sigmoid': 'S', 'aten::mul': 'M',
     'aten::mul_': 'M', 'aten::add': 'A', 'aten::mean': 'G',
     'aten::upsample_nearest2d': 'U', 'aten::cat': 'K', 'aten::relu_': 'R',
     'aten::batch_norm': 'B'}
ir, src = [], {}   # entries [kind, inputs, conv params]; value -> entry
for n in g.nodes():
    k, a = K.get(n.kind()), list(n.inputs())
    if not k:
        assert n.kind() in ('prim::Constant', 'prim::ListConstruct'), n
        continue
    c = a[1:9]
    if k == 'C':
        w, b, s, p, d, t, _, gr = (v.toIValue() for v in c)
        assert s[0] == s[1] and p[0] == p[1] and d == [1, 1] and not t
        z = torch.zeros(w.shape[0])
        c = [w, torch.ones_like(z), z if b is None else b, s[0], p[0], gr, 0]
    assert k != 'A' or a[2].toIValue() == 1
    assert k != 'U' or a[2].toIValue() == [2.0, 2.0]
    assert k != 'G' or a[1].toIValue() == [2, 3]
    a = list(a[0].node().inputs()) if k == 'K' else a[:2 if k in 'AM' else 1]
    x = src.get(a[0])
    if k == 'B':
        assert x[0] == 'C' and len(a[0].uses()) == 1 and not x[2][-1]
        bw, bb, mu, var, _, _, eps, _ = (v.toIValue() for v in c)
        x[2][1] = bw * (1 / torch.sqrt(var + eps))
        x[2][2] = bb - mu * x[2][1] + x[2][2] * x[2][1]
        src[n.output()] = x
        continue
    if k == 'M' and src.get(a[1], [0])[0] == 'S' and src[a[1]][1][0] is a[0] \
            and len(a[1].uses()) == 1:   # x * sigmoid(x): silu
        src[a[1]][0], k, a = None, 'W', a[:1]
    # an activation that is a convolution's only use joins it
    if k in 'RWS' and x and x[0] == 'C' and \
            len(a[0].uses()) == 1 + (k == 'W'):
        x[2][6] = 'RWS'.index(k) + 1
        src[n.output()] = x
        continue
    src[n.output()] = e = [k, a, c]
    ir.append(e)
ir = [e for e in ir if e[0]]
idx = {id(e): i + 1 for i, e in enumerate(ir)}
val = lambda v: 0 if v.node().kind() == 'prim::Param' else idx[id(src[v])]
with open(sys.argv[3], 'wb') as f:
    i32 = lambda *v: f.write(np.array(v, '<i4').tobytes())
    f32 = lambda t: f.write(t.numpy().astype('<f4').tobytes())
    f.write(b'gum1'), i32(len(ir))
    for k, a, c in ir:
        i32(ord(k), len(a), *map(val, a))
        if k == 'C':
            w, sc, sh, s, p, gr, act = c
            i32(*w.shape, s, p, gr, act), f32(w), f32(sc), f32(sh)
    t = np.loadtxt(sys.argv[2], delimiter=',', skiprows=1, usecols=(0, 1))
    i32(len(t)), f.write((-t[:, 0]).astype('<f8').tobytes())
    f.write(t[:, 1].astype('<f8').tobytes())
print(len(ir), 'ops:', *sorted({e[0] + str(sum(f[0] == e[0] for f in ir))
                                 for e in ir}))
