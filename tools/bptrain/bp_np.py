"""NumPy re-implementation of the stock Basic Pitch ONNX (front end + head) - reference for the C++ port."""
import numpy as np, onnx
from onnx import numpy_helper, helper

def consts(path):
    m = onnx.load(path); init = {i.name: numpy_helper.to_array(i) for i in m.graph.initializer}
    convs = [n for n in m.graph.node if n.op_type == 'Conv']
    k36 = [init[n.input[1]] for n in convs if init[n.input[1]].shape == (36, 1, 1, 256)]
    b36 = [init[n.input[2]] for n in convs if init[n.input[1]].shape == (36, 1, 1, 256)]
    lp = [init[n.input[1]] for n in convs if init[n.input[1]].shape == (1, 1, 1, 256)]
    scale = [v for v in init.values() if v.shape == (309, 1, 1)][0]
    head = {}
    for n in convs:
        W = init[n.input[1]]
        if W.ndim == 4 and W.shape[2] > 1:
            at = {a.name: helper.get_attribute_value(a) for a in n.attribute}
            head[W.shape] = (W, init[n.input[2]], at['pads'], at['strides'])
    return dict(kA=k36[0][:, 0, 0], kB=k36[1][:, 0, 0], bA=b36[0], bB=b36[1], lp=lp[0][0, 0, 0],
                same_lp=all(np.array_equal(l, lp[0]) for l in lp),
                same_k=all(np.array_equal(k36[i], k36[i % 2]) for i in range(len(k36))),
                scale=scale[:, 0, 0], head=head)

def conv1d(x, k, stride):         # valid correlation, x (L,), k (C, K) -> (C, T)
    K = k.shape[-1]; T = (len(x) - K) // stride + 1
    idx = np.arange(T)[:, None] * stride + np.arange(K)[None]
    return (x[idx] @ k.T).T if k.ndim == 2 else x[idx] @ k

def cqt_mag(x, c):
    """x (43844,) -> (172, 309) magnitude, bins low -> high."""
    octs = []; cur = x.astype(np.float64)
    for o in range(9):
        stride = 256 >> o
        xp = np.pad(cur, 128, mode='reflect')
        re = conv1d(xp, c['kA'].astype(np.float64), stride)[:, :172] + c['bA'][:, None]
        im = conv1d(xp, c['kB'].astype(np.float64), stride)[:, :172] + c['bB'][:, None]
        octs.append((re.T, im.T))                                   # (172, 36) each
        if o < 8: cur = conv1d(np.pad(cur, 127), c['lp'].astype(np.float64), 2)
    re = np.concatenate([o[0] for o in octs[::-1]], 1)[:, -309:]; im = np.concatenate([o[1] for o in octs[::-1]], 1)[:, -309:]
    re = re * c['scale']; im = im * c['scale']
    return np.sqrt(re ** 2 + im ** 2)

def features(x, c):
    mag = cqt_mag(x, c)
    db = 10 * np.log10(mag ** 2 + 1e-10)
    db = db - db.min(); mx = db.max(); db = db / mx if mx != 0 else db
    f = db * 2.480741024017334 - 0.8769183158874512                   # (172, 309)
    out = []
    for s in [-36, 0, 36, 57, 72, 84, 93, 101]:
        g = np.zeros_like(f)
        if s < 0: g[:, -s:] = f[:, :s]
        elif s == 0: g = f.copy()
        else: g[:, :-s] = f[:, s:]
        out.append(g[:, :264])
    return np.stack(out, 0)                                          # (8, 172, 264)

def conv2d(x, W, b, pads, strides):   # x (Cin, T, F), W (Cout, Cin, kt, kf), pads [t, l, b, r]
    t, l, bo, r = pads; st, sf = strides
    xp = np.pad(x, ((0, 0), (t, bo), (l, r)))
    Co, Ci, kt, kf = W.shape; T = (xp.shape[1] - kt) // st + 1; F = (xp.shape[2] - kf) // sf + 1
    out = np.zeros((Co, T, F)) + b[:, None, None]
    for i in range(kt):
        for j in range(kf):
            patch = xp[:, i:i + st * T:st, j:j + sf * F:sf]               # (Ci, T, F)
            out += np.einsum('oc,ctf->otf', W[:, :, i, j], patch)
    return out

sig = lambda z: 1 / (1 + np.exp(-z))
def run(x, c):
    f = features(x, c); h = c['head']; g = lambda s: h[s]
    on1 = np.maximum(conv2d(f, *g((32, 8, 5, 5))), 0)
    cc = np.maximum(conv2d(f, *g((8, 8, 3, 39))), 0)
    contour = sig(conv2d(cc, *g((1, 8, 5, 5))))
    n1 = np.maximum(conv2d(contour, *g((32, 1, 7, 7))), 0)
    note = sig(conv2d(n1, *g((1, 32, 7, 3))))
    onset = sig(conv2d(np.concatenate([note, on1], 0), *g((1, 33, 3, 3))))
    return note[0], onset[0], contour[0]
