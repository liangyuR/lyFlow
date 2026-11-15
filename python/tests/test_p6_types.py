import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lyflow_python import Box2D, Circle2D, Image, Line2D, NodeCancelled, NodeError, Plane, Point2D, Profile, Record, Tensor, Transform, geometry3d, node, pose2d, pose3d, poses, scalar, value_list
from lyflow_python.codec import DataCodec


class NativeTypeContracts(unittest.TestCase):
    def test_profile_preserves_slots_metadata_channels_and_readonly_copy(self):
        @node(id="py.test.profile", version="1.0.0", label="轮廓", inputs={"profile": "Profile"}, outputs={"profile": "Profile"})
        def echo(inputs, params, ctx):
            return {"profile": inputs["profile"]}

        self.assertEqual(echo.__lyflow_node__["inputs"][0]["type"], "Profile")
        descriptor = dict(schemaVersion=1, frame="激光头", unit="mm", plane="XZ", handedness="reflected",
                          sampleOrder="acquisition", sourceId="scan-1280", sourceCount=1280)
        xyz = np.arange(3840, dtype=np.float32).reshape(1280, 3)
        xyz[100] = np.nan
        validity = np.ones(1280, dtype=np.uint8)
        validity[100] = 0
        profile = Profile(xyz, descriptor, validity, np.arange(1280, dtype=np.uint32),
                          np.arange(1280, dtype=np.float32), np.ones((1280, 3), dtype=np.float32),
                          np.zeros((1280, 3), dtype=np.uint8))
        with tempfile.TemporaryDirectory() as temporary:
            writer, reader = DataCodec(temporary), DataCodec(temporary)
            try:
                dto = writer.encode(profile, "Profile")
                with self.assertRaises(NodeError):
                    writer.encode(profile, "PointCloud")
                self.assertEqual(dto["sourceIndices"]["dtype"], "u32")
                self.assertNotIn("data", dto)
                decoded = reader.decode(dto, "Profile")
                self.assertEqual(dict(decoded.descriptor), descriptor)
                np.testing.assert_array_equal(decoded.xyz, xyz)
                np.testing.assert_array_equal(decoded.source_indices, np.arange(1280, dtype=np.uint32))
                self.assertFalse(decoded.xyz.flags.writeable)
                self.assertFalse(decoded.source_indices.flags.writeable)
                with self.assertRaises(TypeError):
                    decoded.descriptor["unit"] = "m"
                with self.assertRaises(NodeError):
                    decoded.validity = validity
                copied = decoded.copy()
                copied.xyz[0] += 1
                self.assertFalse(np.array_equal(copied.xyz[0], decoded.xyz[0]))
                self.assertEqual(copied.descriptor["unit"], "mm")
            finally:
                reader.close()
                writer.close()
            self.assertEqual(list(Path(temporary).iterdir()), [])
        empty = Profile(np.empty((0, 3), np.float32), {**descriptor, "sourceCount": 0},
                        np.empty(0, np.uint8), np.empty(0, np.uint32))
        self.assertEqual(len(empty.xyz), 0)

    def test_profile_rejects_implicit_units_bad_order_masks_and_invalid_coordinates(self):
        descriptor = dict(schemaVersion=1, frame="sensor", unit="m", plane="XY", handedness="right",
                          sampleOrder="acquisition", sourceId="scan", sourceCount=4)
        xyz = np.zeros((2, 3), np.float32)
        for patch in [{"sourceCount": True}, {"sourceCount": 4294967296}, {"sourceCount": "4"},
                      {"schemaVersion": 1.0}, {"frame": ""}, {"sourceId": "文" * 342},
                      {"unit": "px"}, {"plane": "ZX"}, {"handedness": "auto"}, {"extra": 1}]:
            with self.subTest(patch=patch), self.assertRaises(NodeError):
                Profile(xyz, {**descriptor, **patch}, np.ones(2, np.uint8), np.arange(2, dtype=np.uint32))
        for indices in [np.array([1, 0], np.uint32), np.array([1, 1], np.uint32), np.array([0, 4], np.uint32)]:
            with self.subTest(indices=indices), self.assertRaises(NodeError):
                Profile(xyz, descriptor, np.ones(2, np.uint8), indices)
        unknown = {**descriptor, "sampleOrder": "unknown"}
        Profile(xyz, unknown, np.ones(2, np.uint8), np.array([1, 0], np.uint32))
        with self.assertRaises(NodeError):
            Profile(xyz, unknown, np.ones(2, np.uint8), np.array([1, 1], np.uint32))
        for mask in [np.array([1, 2], np.uint8), np.ones(1, np.uint8), np.ones(2, np.float32)]:
            with self.subTest(mask=mask), self.assertRaises(NodeError):
                Profile(xyz, descriptor, mask, np.arange(2, dtype=np.uint32))
        for value in [float("inf"), float("nan")]:
            bad = xyz.copy()
            bad[0, 0] = value
            with self.subTest(value=value), self.assertRaises(NodeError):
                Profile(bad, descriptor, np.ones(2, np.uint8), np.arange(2, dtype=np.uint32))
        bad = xyz.copy()
        bad[0, 0] = float("inf")
        with self.assertRaises(NodeError):
            Profile(bad, descriptor, np.zeros(2, np.uint8), np.arange(2, dtype=np.uint32))

    def test_profile_cancelled_write_cleans_all_array_files(self):
        descriptor = dict(schemaVersion=1, frame="sensor", unit="m", plane="XY", handedness="right",
                          sampleOrder="acquisition", sourceId="scan", sourceCount=2)
        profile = Profile(np.zeros((2, 3), np.float32), descriptor, np.ones(2, np.uint8), np.arange(2, dtype=np.uint32))
        calls = 0

        def cancel():
            nonlocal calls
            calls += 1
            if calls == 5:
                raise NodeCancelled("cancelled")

        with tempfile.TemporaryDirectory() as temporary:
            codec = DataCodec(temporary, cancel)
            try:
                with self.assertRaises(NodeCancelled):
                    codec.encode(profile, "Profile")
            finally:
                codec.close()
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_native_geometry_tensor_and_preview_metadata_roundtrip_readonly(self):
        matrix = np.eye(4, dtype=np.float32)
        matrix[:3, 3] = [.125, -.25, 2]
        values = [
            ("Transform", Transform(matrix)),
            ("Plane", Plane([0, 0, 1], -.125)),
            ("Point2D", Point2D([1.25, 2.5], "px")),
            ("Point2D", Point2D([.125, -.25], "m")),
            ("Line2D", Line2D([1, 2], [0, 1], "px", [1, 1], [1, 4])),
            ("Line2D", Line2D([.1, .2], [1, 0], "m")),
            ("Circle2D", Circle2D([3, 4], .125, "m")),
            ("Box2D", Box2D([0, 1], [4, 5], "px")),
            ("Image", Image(np.arange(24, dtype=np.uint16).reshape(2, 3, 4), scale=2)),
            ("Tensor", Tensor(np.arange(24, dtype=np.float32).reshape(2, 3, 4)[:, ::-1], scale=3)),
            ("Tensor", Tensor(np.empty((2, 0, 4), dtype=np.float32), scale=2)),
            ("Tensor", Tensor(np.empty((0,), dtype=np.float32), shape=[])),
            ("Tensor", Tensor(np.ones((1,) * 8, dtype=np.float32))),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            for kind, value in values:
                with self.subTest(kind=kind, values=vars(value)):
                    writer, reader = DataCodec(temporary), DataCodec(temporary)
                    try:
                        dto = writer.encode(value, kind)
                        decoded = reader.decode(dto, kind)
                        copied = decoded.copy()
                        for key, original in vars(value).items():
                            actual = getattr(decoded, key)
                            if isinstance(original, np.ndarray):
                                np.testing.assert_array_equal(actual, original)
                                self.assertFalse(actual.flags.writeable)
                                if actual.size:
                                    with self.assertRaises(ValueError):
                                        actual.flat[0] = 7
                                    getattr(copied, key).flat[0] = 7
                                    self.assertEqual(getattr(decoded, key).flat[0], original.flat[0])
                            else:
                                self.assertEqual(actual, original)
                        with self.assertRaisesRegex(NodeError, "read-only"):
                            decoded.unexpected = 1
                        if kind == "Tensor":
                            self.assertEqual(Path(dto["values"]["file"]).stat().st_size, value.values.nbytes)
                            self.assertNotIn("data", dto)
                    finally:
                        reader.close()
                        writer.close()
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_invalid_native_metadata_and_tensor_shapes_are_rejected(self):
        actions = [
            lambda: Transform(np.eye(3)),
            lambda: Transform(np.full((4, 4), float("nan"))),
            lambda: Transform([[True, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]),
            lambda: Plane([0, 0, 2], 0),
            lambda: Plane([0, 0, 1], float("inf")),
            lambda: Point2D([1, 2], "mm"),
            lambda: Point2D([1e100, 2]),
            lambda: Line2D([0, 0], [0, 0]),
            lambda: Line2D([0, 0], [1, 0], start=[0, 0]),
            lambda: Line2D([0, 0], [1, 0], start=[1, 2], end=[1, 2]),
            lambda: Circle2D([0, 0], 0),
            lambda: Box2D([2, 0], [1, 2]),
            lambda: Tensor(np.ones(3, dtype=np.float64)),
            lambda: Tensor(np.ones(3, dtype=np.float32), shape=[2, 2]),
            lambda: Tensor(np.ones((1,) * 9, dtype=np.float32)),
            lambda: Tensor(np.ones(3, dtype=np.float32), scale=True),
            lambda: Image(np.ones((1, 1), dtype=np.uint8), scale=0),
            lambda: Tensor(np.broadcast_to(np.zeros(1, dtype=np.float32), (268435457,))),
        ]
        for index, action in enumerate(actions):
            with self.subTest(index=index), self.assertRaises(NodeError):
                action()
        with tempfile.TemporaryDirectory() as temporary:
            codec = DataCodec(temporary)
            for dto in [
                {"type": "Point2D", "position": [1, 2]},
                {"type": "Point2D", "position": [1, 2], "unit": "px", "unknown": 1},
                {"type": "Transform", "matrix": [True] + [0] * 15},
                {"type": "Image", "pixels": {}},
                {"type": "Tensor", "values": {}, "scale": 1},
            ]:
                with self.subTest(dto=dto), self.assertRaises(NodeError):
                    codec.decode(dto, dto["type"])
            codec.close()

    def test_existing_standard_records_preserve_units_identity_and_version(self):
        a = pose2d("a", [1, 2], [3, 4], angle=3 * np.pi, frame="image", unit="px", attributes={"optional": None})
        self.assertEqual(a.data["angle"], -np.pi)
        b = pose3d("b", [.1, .2, .3], [.01, .02, .03], rotation=[0, 0, 0, -1], frame="camera", pickable=False)
        self.assertEqual(b.data["rotation"], [0, 0, 0, 1])
        values = [scalar(1.25, "m"), scalar(True), scalar("中文"), value_list([1, 2, 3], unit="m"), a, b,
                  poses([a], 2), poses([b], 3), poses([], 3),
                  geometry3d([{"id": "p", "type": "point", "position": [1, 2, 3]},
                              {"type": "line", "position": [0, 0, 0], "direction": [1, 0, 0]},
                              {"type": "circle", "position": [0, 0, 0], "normal": [0, 0, 1], "radius": .5},
                              {"type": "plane", "normal": [0, 0, 1], "d": -2},
                              {"type": "sphere", "position": [0, 0, 0], "radius": 1}], frame="world"),
                  Record({"frame": "world", "unit": "m", "sourceFrame": "image",
                          "items": [{"type": "point", "position": [1, 2, 3]}]}, "geometry.Features3D.v1")]
        with tempfile.TemporaryDirectory() as temporary:
            codec = DataCodec(temporary)
            try:
                for value in values:
                    decoded = codec.decode(codec.encode(value, "Record"), "Record")
                    self.assertEqual(decoded.copy().data, value.data)
                    self.assertEqual(decoded.record_type, value.record_type)
                self.assertEqual(poses([a], 2).data["items"][0]["id"], "a")
                self.assertEqual(poses([b], 3).data["unit"], "m")
            finally:
                codec.close()
        for action in [
            lambda: scalar(2 ** 53 + 1),
            lambda: scalar(True, "m"),
            lambda: value_list([1, "two"]),
            lambda: poses([a, a], 2),
            lambda: poses([a], 3),
            lambda: poses([b], 3, frame="different"),
            lambda: Record(b.data, "vision.Pose3D.v2"),
            lambda: Record({**b.data, "rotation": [0, 0, 0, 2]}, b.record_type),
            lambda: Record({"array": [0] * 4097}),
            lambda: Record({"text": "x" * 65537}),
            lambda: geometry3d({"type": "circle", "position": [0, 0, 0], "normal": [0, 0, 1], "radius": -1}),
            lambda: geometry3d({"type": "line", "position": [0, 0, 0], "direction": [1, 0, 0], "hasSegment": 1}),
            lambda: geometry3d({"type": "point", "position": [0, 0, 0], "unit": "mm"}),
        ]:
            with self.subTest(action=action), self.assertRaises(NodeError):
                action()


if __name__ == "__main__":
    unittest.main()
