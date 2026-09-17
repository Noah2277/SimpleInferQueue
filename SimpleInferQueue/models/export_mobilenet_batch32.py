import torch
import torch.nn as nn
import torchvision.models as models

# 加载预训练 MobileNetV2
model = models.mobilenet_v2(pretrained=True)
model.eval()

# 关键变化：batch size 从 1 改成 32
dummy_input = torch.randn(32, 3, 224, 224)

# 跑一次 PyTorch 推理，保存参考输出
with torch.no_grad():
    pytorch_output = model(dummy_input)

print("PyTorch output shape:", pytorch_output.shape)
print("PyTorch output[:5]:", pytorch_output[0, :5])

# 导出为 ONNX
torch.onnx.export(
    model,
    dummy_input,
    "mobilenet_batch32.onnx",
    input_names=["input"],
    output_names=["output"],
    opset_version=11,
    dynamo=False
)

print("Model exported to mobilenet_batch32.onnx")