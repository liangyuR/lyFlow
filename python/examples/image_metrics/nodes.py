import numpy as np

from lyflow_python import Image, Measurement, Record, node


@node(
    id="py.example.image_metrics", version="1.0.0", label="Python 图像亮度",
    inputs={"image": "Image"}, outputs={"image": "Image", "mean": "Measurement", "info": "Record"},
    params={"gain": {"type": "float", "default": 1.0, "min": 0, "max": 8}},
)
def image_metrics(inputs, params, ctx):
    image, gain = inputs["image"], params["gain"]
    ctx.check_cancelled()
    pixels = image.pixels.astype(np.float32) * np.float32(gain)
    ctx.progress(1, "已计算亮度")
    return {
        "image": Image(pixels),
        "mean": Measurement(float(pixels.mean()), unit="level"),
        "info": Record({"shape": list(pixels.shape), "gain": gain}, record_type="py.image.metrics"),
    }
