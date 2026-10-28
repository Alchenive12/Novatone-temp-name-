"""Basic Pitch split for fine-tuning: frozen front end (ONNX: CQT + harmonic stacking) -> trainable
PyTorch head (the 6 conv layers, BN folded), weights loaded from / written back to the stock ONNX."""
import os, numpy as np, onnx, torch, torch.nn as nn, torch.nn.functional as F
from onnx import numpy_helper, helper
import onnxruntime as ort

HEAD = {  # ONNX conv name prefix -> role
    'model_1/re_lu_3/Relu':        'onset1',   # 32x8x5x5, stride (1,3), on stacked CQT
    'model_1/re_lu_1/Relu':        'cont1',    # 8x8x3x39, on stacked CQT
    'model_1/contours-reduced':    'cont2',    # 1x8x5x5 -> sigmoid = contour
    'model_1/re_lu_2/Relu':        'note1',    # 32x1x7x7, stride (1,3), on contour
    'model_1/conv2d_3/BiasAdd':    'note2',    # 1x32x7x3 -> sigmoid = note
    'model_1/conv2d_5/BiasAdd':    'onset2',   # 1x33x3x3 on concat(note, onset1) -> sigmoid = onset
}

def _head_convs(m):
    init = {i.name: i for i in m.graph.initializer}
    out = {}
    for n in m.graph.node:
        if n.op_type != 'Conv': continue
        for pre, role in HEAD.items():
            if n.name.startswith(pre):
                at = {a.name: helper.get_attribute_value(a) for a in n.attribute}
                out[role] = dict(node=n, w=n.input[1], b=n.input[2], pads=list(at.get('pads', [0, 0, 0, 0])),
                                 strides=list(at.get('strides', [1, 1])), W=numpy_helper.to_array(init[n.input[1]]),
                                 B=numpy_helper.to_array(init[n.input[2]]))
    assert len(out) == 6, out.keys()
    return out

class Head(nn.Module):
    def __init__(self, convs):
        super().__init__()
        self.cfg = {}
        for role, c in convs.items():
            conv = nn.Conv2d(c['W'].shape[1], c['W'].shape[0], c['W'].shape[2:], stride=tuple(c['strides']))
            conv.weight.data = torch.from_numpy(c['W'].copy()); conv.bias.data = torch.from_numpy(c['B'].copy())
            setattr(self, role, conv)
            t, l, b, r = c['pads']; self.cfg[role] = (l, r, t, b)
    def _c(self, role, x): return getattr(self, role)(F.pad(x, self.cfg[role]))
    def forward(self, x, logits=False):                 # x: (B, 8, T, 264) stacked CQT
        on1 = F.relu(self._c('onset1', x))
        c = F.relu(self._c('cont1', x))
        c_l = self._c('cont2', c); contour = torch.sigmoid(c_l)
        n1 = F.relu(self._c('note1', contour))
        n_l = self._c('note2', n1); note = torch.sigmoid(n_l)
        o_l = self._c('onset2', torch.cat([note, on1], 1))
        if logits: return n_l[:, 0], o_l[:, 0], c_l[:, 0]
        return note[:, 0], torch.sigmoid(o_l)[:, 0], contour[:, 0]

def load(onnx_path):
    m = onnx.load(onnx_path)
    convs = _head_convs(m)
    feat_name = convs['onset1']['node'].input[0]
    assert convs['cont1']['node'].input[0] == feat_name
    # front end = same graph, output = stacked CQT tensor
    fm = onnx.load(onnx_path)
    del fm.graph.output[:]
    fm.graph.output.append(helper.make_tensor_value_info(feat_name, onnx.TensorProto.FLOAT, None))
    front = ort.InferenceSession(fm.SerializeToString(), providers=['CPUExecutionProvider'])
    return front, Head(convs), m, convs

def features(front, audio_windows):                      # (N, 43844) float32 at 22050 Hz
    return front.run(None, {front.get_inputs()[0].name: audio_windows[:, :, None].astype(np.float32)})[0]

def save_onnx(head, model, convs, path):
    """Write the fine-tuned head weights into a copy of the stock ONNX graph."""
    m = onnx.ModelProto(); m.CopyFrom(model)
    byname = {i.name: i for i in m.graph.initializer}
    for role, c in convs.items():
        conv = getattr(head, role)
        byname[c['w']].CopyFrom(numpy_helper.from_array(conv.weight.detach().cpu().numpy().astype(np.float32), c['w']))
        byname[c['b']].CopyFrom(numpy_helper.from_array(conv.bias.detach().cpu().numpy().astype(np.float32), c['b']))
    onnx.save(m, path)

if __name__ == '__main__':   # self-check: head reproduces the stock model exactly
    import basic_pitch
    p = os.path.join(os.path.dirname(basic_pitch.__file__), 'saved_models', 'icassp_2022', 'nmp.onnx')
    front, head, model, convs = load(p)
    x = (np.random.default_rng(0).standard_normal((2, 43844)) * 0.1).astype(np.float32)
    ref = ort.InferenceSession(p).run(None, {'serving_default_input_2:0': x[:, :, None]})
    feat = features(front, x); print('features', feat.shape)
    with torch.no_grad(): note, onset, contour = head(torch.from_numpy(feat))
    for r in ref:
        mine = {88: note, 264: contour}.get(r.shape[-1])
        if r.shape[-1] == 88:
            print(r.shape, 'vs note', float(np.abs(r - note.numpy()).max()), 'vs onset', float(np.abs(r - onset.numpy()).max()))
        else:
            print(r.shape, 'vs contour', float(np.abs(r - contour.numpy()).max()))
    save_onnx(head, model, convs, '/tmp/claude-0/rt.onnx'); r2 = ort.InferenceSession('/tmp/claude-0/rt.onnx').run(None, {'serving_default_input_2:0': x[:, :, None]})
    print('round-trip export identical:', all(np.allclose(a, b, atol=1e-6) for a, b in zip(ref, r2)))
