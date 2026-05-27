"""Generate a tiny deterministic ONNX model for TractBackend tests.

The model ignores input and always outputs two detections:
  [0.1, 0.2, 0.5, 0.6, 0.9, 0.0]  -> Person at (0.1,0.2)-(0.5,0.6), conf 0.9
  [0.3, 0.4, 0.7, 0.8, 0.3, 1.0]  -> Vehicle at (0.3,0.4)-(0.7,0.8), conf 0.3

With default threshold 0.5, only the first detection should pass.
"""

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

# Fixed output: 2 detections, 6 values each [x1, y1, x2, y2, conf, class]
output_data = np.array(
    [
        [
            [0.1, 0.2, 0.5, 0.6, 0.9, 0.0],
            [0.3, 0.4, 0.7, 0.8, 0.3, 1.0],
        ]
    ],
    dtype=np.float32,
)

constant_node = helper.make_node(
    "Constant",
    inputs=[],
    outputs=["detections"],
    value=numpy_helper.from_array(output_data, name="const_detections"),
)

# Identity node to consume the input (required by tract to accept the input fact)
input_identity = helper.make_node(
    "Shape", inputs=["input"], outputs=["input_shape"]
)

graph = helper.make_graph(
    [input_identity, constant_node],
    "test_detector",
    inputs=[
        helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, 4, 4]),
    ],
    outputs=[
        helper.make_tensor_value_info(
            "detections", TensorProto.FLOAT, [1, 2, 6]
        ),
    ],
)

model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, "tests/fixtures/test_detector.onnx")
print("Created tests/fixtures/test_detector.onnx")
