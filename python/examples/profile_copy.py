import numpy as np

from lyflow_python import Context, Profile, node


@node(
    id="py.profile_copy.offset_z", version="1.0.0", label="Python 轮廓有效采样偏移",
    inputs={"profile": "Profile"}, outputs={"profile": "Profile"},
    params={"offset_m": {"type": "float", "default": 0.001, "unit": "m", "label": "Z 偏移"}},
    keywords=["轮廓", "有效采样", "偏移", "profile", "validity", "offset"],
)
def offset_z(inputs, params, ctx):
    result = inputs["profile"].copy()
    offset = params["offset_m"] * (1000 if result.descriptor["unit"] == "mm" else 1)
    for start in range(0, len(result.xyz), 4096):
        ctx.check_cancelled()
        end = min(start + 4096, len(result.xyz))
        valid = result.validity[start:end] == 1
        z = result.xyz[start:end, 2]
        z[valid] += offset
        ctx.progress(end / len(result.xyz))
    return {"profile": result}


def main():
    count = 1280
    xyz = np.zeros((count, 3), dtype=np.float32)
    xyz[:, 0] = np.arange(count, dtype=np.float32)
    xyz[120] = np.nan
    xyz[121] = [121, 0, 9]
    validity = np.ones(count, dtype=np.uint8)
    validity[120:122] = 0
    source = Profile(
        xyz,
        dict(schemaVersion=1, frame="scanner", unit="mm", plane="XZ", handedness="right",
             sampleOrder="acquisition", sourceId="demo-scan", sourceCount=count),
        validity, np.arange(count, dtype=np.uint32),
    )
    result = offset_z({"profile": source}, {"offset_m": 0.001}, Context())["profile"]
    assert result.xyz.shape == (count, 3)
    assert np.isnan(result.xyz[120]).all()
    np.testing.assert_array_equal(result.xyz[121], source.xyz[121])
    np.testing.assert_array_equal(result.validity, source.validity)
    np.testing.assert_array_equal(result.source_indices, source.source_indices)
    assert dict(result.descriptor) == dict(source.descriptor)
    assert result.xyz[0, 2] == 1 and source.xyz[0, 2] == 0
    print("Profile example OK: 1280 slots, 1278 valid; offset 0.001 m = 1 mm; input unchanged")


if __name__ == "__main__":
    main()
