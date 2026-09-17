import torch
import torch.nn as nn
import torchvision.models as models

# 1. 加载预训练的 MobileNetV2
#    pretrained=True 会下载在 ImageNet 上训练好的权重
model = models.mobilenet_v2(pretrained=True)

# 2. 设为推理模式
#    这会影响 Dropout 和 BatchNorm 的行为，导出时必须做
model.eval()

# 3. 创建假输入，形状为 [1, 3, 224, 224]
#    含义：[batch_size=1, channels=3, height=224, width=224]
dummy_input = torch.randn(1, 3, 224, 224)

# 4. 先跑一次 PyTorch 推理，保存参考输出
#    之后可以用来验证 ONNX 导出的正确性
with torch.no_grad():
    pytorch_output = model(dummy_input)

print("PyTorch output shape:", pytorch_output.shape)
print("PyTorch output[:5]:", pytorch_output[0, :5])

# 5. 导出为 ONNX
torch.onnx.export(
    model,
    dummy_input,
    "mobilenet.onnx",
    input_names=["input"],
    output_names=["output"],
    opset_version=11,
    dynamo=False    # 用旧版导出器，兼容性更好
)

print("Model exported to mobilenet.onnx")

# 6. 保存一份参考输出，之后 C++ 端可以用来对比
import numpy as np
np.save("mobilenet_reference.npy", pytorch_output.detach().numpy())
print("Reference output saved to mobilenet_reference.npy")