"""导出 v5 推理测试用的模型。

这个模型是测试夹具，存在的唯一目的是让 examples/cuda_pipeline/v5_onnx_inference.cu
能验证「推理结果算对了没」。它不追求任何模型意义上的合理性。

设计要点：

1. 权重写成固定的整数，不用随机初始化。
   随机权重会导致每次导出结果都不同，测试就无法复现——今天 PASS 明天 FAIL。
   整数权重让 float32 上的算术是精确的，C++ 里可以直接用相等比较（留个极小容差）。

2. 权重全为正数，且输入非负，于是 Wx+b 恒为正，Relu 永远不会触发。
   这样 C++ 侧可以独立算出精确的期望值来比对，不需要参考表、也不需要读权重文件。
   （Relu 保留在模型里，只是为了让它长得像个正常的神经网络。）

3. 两行权重刻意取不同的和（10 和 4），这样两个输出的斜率不同。
   万一程序把两个输出搞反了，斜率对不上，测试能发现。

对应关系（C++ 侧的校验用同样的公式）：

    W = [[1, 2, 3, 4],        b = [1, 2]
         [1, 1, 1, 1]]
    输入 task_id = t 时，input[i] = t + i（i = 0..3）

    output[0] = 1*t + 2(t+1) + 3(t+2) + 4(t+3) + 1 = 10t + 21
    output[1] = 1*t + 1(t+1) + 1(t+2) + 1(t+3) + 2 =  4t +  8

改了这里的权重，v5_onnx_inference.cu 里的 EXPECT_W / EXPECT_B 必须同步改，
否则测试会 FAIL——这是刻意的，避免两边悄悄不一致。
"""

import os
import sys

# 中文 Windows 控制台默认 GBK 编码，而新导出器的日志里有 ✅ 之类的字符，
# 不改这里会直接 UnicodeEncodeError 崩溃。
sys.stdout.reconfigure(encoding="utf-8")

import torch
import torch.nn as nn


class SimpleModel(nn.Module):
    def __init__(self):
        super(SimpleModel, self).__init__()
        self.fc = nn.Linear(4, 2)  # 输入4维，输出2维

    def forward(self, x):
        return torch.relu(self.fc(x))


model = SimpleModel()
model.eval()

# 固定权重，覆盖掉 nn.Linear 的随机初始化
with torch.no_grad():
    model.fc.weight.copy_(torch.tensor([[1.0, 2.0, 3.0, 4.0],
                                        [1.0, 1.0, 1.0, 1.0]]))
    model.fc.bias.copy_(torch.tensor([1.0, 2.0]))

# 假输入形状 [1, 4]：batch 维会被固定成 1（不是动态维），
# 所以推理时必须一条一条 Run，不能一次喂多条
dummy_input = torch.randn(1, 4)

# 导出到本脚本所在目录，避免依赖调用者的当前工作目录
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "model.onnx")

# 注意：torch 2.9 起默认使用新的 torch.export 导出器，它的最低 opset 是 18，
# 传 opset_version=11 会被静默忽略。若确实需要 opset 11，加 dynamo=False
# 走旧的 TorchScript 导出器（会有 DeprecationWarning）。
torch.onnx.export(
    model,
    dummy_input,
    out_path,
    input_names=["input"],
    output_names=["output"],
    opset_version=18,
)

print("Model exported to " + out_path)

# 自检：确认权重确实被写进去了，且 Relu 对测试输入不触发
import numpy as np
import onnxruntime as ort

sess = ort.InferenceSession(out_path, providers=["CPUExecutionProvider"])
name = sess.get_inputs()[0].name
ok = True
for t in (0, 1, 50, 99):
    x = np.array([[float(t + i) for i in range(4)]], dtype=np.float32)
    got = sess.run(None, {name: x})[0][0]
    want = np.array([10.0 * t + 21.0, 4.0 * t + 8.0], dtype=np.float32)
    match = np.allclose(got, want, atol=1e-5)
    ok = ok and match
    print("  task %-3d expect=%-18s got=%-18s %s"
          % (t, want, got, "OK" if match else "MISMATCH"))

if not ok:
    print("导出结果和预期公式不符，检查权重是否写进去了")
    sys.exit(1)
print("自检通过：输出与 Wx+b 公式一致")
