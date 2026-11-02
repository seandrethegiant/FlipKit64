"""Writes a tiny ONNX model with HTDemucs' tensor signature, for testing StemSeparator's plumbing.

  python3 make_stub_model.py wave out.onnx   every stem = 0.25 * mix through the waveform branch
  python3 make_stub_model.py spec out.onnx   every stem = 0.25 * mix through the spectral branch

The stubs separate nothing; they only prove that normalisation, STFT / inverse STFT, chunk
stitching and file writing are right. The real model must be tested by ear.
"""
import sys
import numpy as np
import onnx
from onnx import helper, TensorProto as TP, numpy_helper as nh

mode, out = sys.argv[1], sys.argv[2]
N, T, F = 343980, 336, 2048
mix = helper.make_tensor_value_info("mix", TP.FLOAT, [1, 2, N])
spec = helper.make_tensor_value_info("spec", TP.FLOAT, [1, T, 4, F])
out_spec = helper.make_tensor_value_info("out_spec", TP.FLOAT, [1, T, 4, 4, F])
out_wave = helper.make_tensor_value_info("out_wave", TP.FLOAT, [1, 4, 2, N])

def i64(name, vals): return nh.from_array(np.array(vals, dtype=np.int64), name)
def f32(name, v): return nh.from_array(np.array(v, dtype=np.float32), name)
init = [i64("ax1", [1]), i64("ax2", [2]), i64("shape_wave", [1, 4, 2, N]), i64("shape_spec", [1, T, 4, 4, F]),
        f32("quarter", 0.25), f32("zero", 0.0)]
nodes = [
    helper.make_node("Unsqueeze", ["mix", "ax1"], ["mix4"]),
    helper.make_node("Expand", ["mix4", "shape_wave"], ["mix_exp"]),
    helper.make_node("Unsqueeze", ["spec", "ax2"], ["spec5"]),
    helper.make_node("Expand", ["spec5", "shape_spec"], ["spec_exp"]),
]
if mode == "wave":
    nodes += [helper.make_node("Mul", ["mix_exp", "quarter"], ["out_wave"]),
              helper.make_node("Mul", ["spec_exp", "zero"], ["out_spec"])]
else:
    nodes += [helper.make_node("Mul", ["mix_exp", "zero"], ["out_wave"]),
              helper.make_node("Mul", ["spec_exp", "quarter"], ["out_spec"])]
g = helper.make_graph(nodes, "stub", [mix, spec], [out_spec, out_wave], init)
m = helper.make_model(g, opset_imports=[helper.make_opsetid("", 17)])
m.ir_version = 8
onnx.checker.check_model(m)
onnx.save(m, out)
