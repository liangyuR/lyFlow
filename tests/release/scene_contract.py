import argparse
import json
import math
from pathlib import Path

from p4_contract import execute, require


def write_pcd(path, points):
    header = (
        "VERSION 0.7\nFIELDS x y z rgb\nSIZE 4 4 4 4\nTYPE F F F U\nCOUNT 1 1 1 1\n"
        f"WIDTH {len(points)}\nHEIGHT 1\nPOINTS {len(points)}\nDATA ascii\n"
    )
    path.write_text(header + "".join(f"{x:.9g} {y:.9g} {z:.9g} 6579300\n" for x, y, z in points),
                    encoding="ascii")


def gap_inputs(directory):
    directory.mkdir(exist_ok=True)
    template = directory / "StandardGap"
    template.mkdir(exist_ok=True)
    path = directory / "StandardGap.yml"
    config = {
        "common_settings": {
            "seg_mode": "ROI", "overall_roi": [-30, 150, 30, 180],
            "overall_roi_mode": "fixed", "using_camera": "Both",
            "line_fit_distance": 0.05, "circle_fit_distance": 0.03,
        },
        "flush": {
            "base_type": "fit line", "ref_type": "line end", "base_side": "left",
            "base_roi": [-18, 164.5, -8, 165.5], "ref_roi": [8, 163.5, 18, 164.5],
            "segment_points": 0, "offset": 0,
            "tolerances": {"nominal": -1, "up_deviation": 0.2, "low_deviation": -0.2},
        },
        "gap": {
            "left_type": "circle", "right_type": "circle", "definition": "B",
            "left_roi": [-3.2, 164.8, -1.8, 166.2], "right_roi": [1.8, 163.8, 3.2, 165.2],
            "offset": 0,
            "radius": {
                "left_circle_radius_min": 0.8, "left_circle_radius_max": 1.2,
                "right_circle_radius_min": 0.8, "right_circle_radius_max": 1.2,
            },
            "tolerances": {"nominal": 4, "up_deviation": 0.2, "low_deviation": -0.2},
        },
        "align": {
            "align_cloud": True,
            "ICP": {"num_neighbor": 8, "min_score": 20, "max_matching_dist": 2,
                    "max_fitness_dist": 0.5, "max_iteration_num": 100},
            "template_candidates": [
                {"id": "survey", "left": "left_template.pcd", "right": "right_template.pcd"},
            ],
        },
    }
    path.write_text(json.dumps(config), encoding="utf-8")
    positions = [i / 199 for i in range(200)]
    left = [(-20 + 17 * t, 165) for t in positions]
    left += [(-3 + math.sin(t * math.pi / 2), 166 - math.cos(t * math.pi / 2)) for t in positions]
    right = [(3 - math.sin(t * math.pi / 2), 165 - math.cos(t * math.pi / 2))
             for t in reversed(positions)]
    right += [(3 + 17 * t, 164) for t in positions]
    for side, points in (("left", left), ("right", right)):
        write_pcd(template / f"{side}_template.pcd", [(x / 1000, z / 1000, 0) for x, z in points])
    sensor = [(x / 1000, 0, z / 1000) for x, z in left + right]
    for name in ("LaserProfile_L0_Master_x.pcd", "LaserProfile_R1_Slave_x.pcd"):
        write_pcd(directory / name, sensor)
    return path


def dts_input(path):
    sections = [
        (-36, -24, lambda x: 4 + (x + 24) * math.tan(math.radians(35))),
        (-21, -12, lambda x: 1 - (x + 12) * math.tan(math.radians(30))),
        (-8, 8, lambda x: math.sqrt(100 - x*x)),
    ]
    with path.open("w", encoding="ascii", newline="\n") as csv:
        csv.write("x,z,intensity\n")
        for first, last, height in sections:
            for step in range(first * 10, last * 10 + 1):
                x = step / 10
                csv.write(f"{x:.1f},{height(x):.8f},1\n")


def glue_input(path):
    width, height = 1280, 1024
    background = bytes([184]) * 705 + bytes([95]) * (width - 705)
    strip = background[:624] + bytes([54]) * 32 + background[656:]
    path.write_bytes(f"P5\n{width} {height}\n255\n".encode("ascii")
                     + background * 32 + strip * 518 + background * (height - 550))


def runtime_outputs(cli, graph, output_dir, options=()):
    lines = execute(cli, ["run", str(graph), "--outputs", "--summary", *options],
                    output_dir / graph.stem.replace(".lyflow", ""))
    summary = lines[-1]
    require(summary["kind"] == "run_summary" and summary["status"] == "ok"
            and not summary["contractViolations"], f"{graph.name}: unexpected run state")
    require(all(entry["state"] == "value" for entry in summary["outputs"].values()),
            f"{graph.name}: missing named output")
    return summary["outputs"]


def file_input(node, path):
    return ("--set", f'{node}.source="file"', "--set", f"{node}.path=" + json.dumps(str(path)))


def main():
    parser = argparse.ArgumentParser(description="Run installed CLI workflows for all three solutions")
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--examples", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    cli = args.cli.resolve()
    examples = args.examples.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    gap = gap_inputs(output / "gap")
    graph = gap.parent / "standardgap.lyflow.json"
    execute(cli, ["import", str(gap), "--kind", "StandardGap.yml:template:fine", "-o", str(graph)],
            output / "import")
    outputs = runtime_outputs(cli, graph, output)
    for name in ("gap", "flush"):
        result = outputs[name]["value"]
        require(result["kind"] == "Measurement" and result["ok"] and result["unit"] == "mm",
                f"StandardGap {name}: expected a valid millimeter measurement")

    profile = output / "seal.csv"
    dts_input(profile)
    for graph in ("default-profile", "adjacent-profile"):
        outputs = runtime_outputs(cli, examples / "dts" / f"{graph}.lyflow.json", output,
                                  file_input("n_in", profile))
        result = outputs["flush"]["value"]
        require(result["kind"] == "Measurement" and result["ok"] and result["unit"] == "mm",
                f"{graph}: expected a valid millimeter measurement")
        require(outputs["bundle"]["value"]["type"] == "DtsProfileBundle.v3",
                f"{graph}: missing result bundle")

    image = output / "bead.pgm"
    glue_input(image)
    outputs = runtime_outputs(cli, examples / "glue/graphs/measure.lyflow.json", output,
                              (*file_input("n_load", image), "--param", "sector=[-120,-60]"))
    require(outputs["runs0"]["value"]["type"] == "glue.LateralRuns",
            "Glue: missing the caliper observation record")
    print(json.dumps({"scenes": ["gap", "dts-default", "dts-adjacent", "glue"], "status": "passed"}))


if __name__ == "__main__":
    main()
