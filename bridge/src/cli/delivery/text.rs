use serde_json::{json, Value};
use std::f64::consts::PI;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Kind {
    String,
    Poses2d,
    Poses3d,
}

#[derive(Clone, Debug)]
pub(super) struct Options {
    pub kind: Kind,
    pub first: bool,
    pub pose_format: String,
    pub length_unit: Option<String>,
    pub angle_degrees: bool,
}

fn number(value: &Value, field: &str) -> Result<f64, String> {
    if value
        .as_i64()
        .is_some_and(|v| !(-9_007_199_254_740_992..=9_007_199_254_740_992).contains(&v))
        || value.as_u64().is_some_and(|v| v > 9_007_199_254_740_992)
    {
        return Err(format!("{field}: integer exceeds exact JSON range"));
    }
    value
        .as_f64()
        .filter(|v| v.is_finite())
        .ok_or_else(|| format!("{field}: expected a finite number"))
}

fn numbers<const N: usize>(value: &Value, field: &str) -> Result<[f64; N], String> {
    let array = value
        .as_array()
        .filter(|a| a.len() == N)
        .ok_or_else(|| format!("{field}: expected {N} components"))?;
    let mut output = [0.; N];
    for (i, item) in array.iter().enumerate() {
        output[i] = number(item, field)?;
    }
    Ok(output)
}

fn text<'a>(value: &'a Value, field: &str) -> Result<&'a str, String> {
    value
        .as_str()
        .filter(|s| !s.is_empty())
        .ok_or_else(|| format!("{field}: expected nonempty text"))
}

fn quaternion(value: &Value) -> Result<[f64; 4], String> {
    let mut q = numbers::<4>(value, "rotation")?;
    let scale = q.iter().fold(0f64, |a, b| a.max(b.abs()));
    if scale == 0. {
        return Err("rotation: zero quaternion".into());
    }
    let norm = q.iter().map(|v| (v / scale).powi(2)).sum::<f64>().sqrt() * scale;
    if !norm.is_finite() || (norm - 1.).abs() > 1e-6 {
        return Err("rotation: quaternion must be normalized".into());
    }
    for v in &mut q {
        *v /= norm;
    }
    if q[3] < 0.
        || (q[3] == 0.
            && q.iter()
                .take(3)
                .find(|v| **v != 0.)
                .is_some_and(|v| *v < 0.))
    {
        for v in &mut q {
            *v = -*v;
        }
    }
    Ok(q)
}

fn matrix(q: [f64; 4]) -> [[f64; 3]; 3] {
    let [x, y, z, w] = q;
    [
        [
            1. - 2. * (y * y + z * z),
            2. * (x * y - z * w),
            2. * (x * z + y * w),
        ],
        [
            2. * (x * y + z * w),
            1. - 2. * (x * x + z * z),
            2. * (y * z - x * w),
        ],
        [
            2. * (x * z - y * w),
            2. * (y * z + x * w),
            1. - 2. * (x * x + y * y),
        ],
    ]
}

fn wrap(value: f64) -> f64 {
    (value + PI).rem_euclid(2. * PI) - PI
}

pub(super) fn euler(rotation: [f64; 4], sequence: &str) -> Result<([f64; 3], bool), String> {
    let bytes = sequence.as_bytes();
    if bytes.len() != 3
        || !(bytes.iter().all(u8::is_ascii_uppercase) || bytes.iter().all(u8::is_ascii_lowercase))
        || !bytes
            .iter()
            .all(|v| matches!(v.to_ascii_lowercase(), b'x' | b'y' | b'z'))
        || bytes[0] == bytes[1]
        || bytes[1] == bytes[2]
    {
        return Err(
            "pose-format: expected quat or a valid three-axis intrinsic/extrinsic sequence".into(),
        );
    }
    let intrinsic = bytes[0].is_ascii_uppercase();
    let mut axes = bytes
        .iter()
        .map(|v| (v.to_ascii_lowercase() - b'x') as usize)
        .collect::<Vec<_>>();
    if intrinsic {
        axes.reverse();
    }
    let i = axes[0];
    let j = axes[1];
    let k = 3 - i - j;
    let repeated = axes[2] == i;
    if !repeated && axes[2] != k {
        return Err("pose-format: invalid Euler sequence".into());
    }
    let odd = j != (i + 1) % 3;
    let r = matrix(rotation);
    let (mut angles, singular) = if repeated {
        let support = r[i][j].hypot(r[i][k]);
        if support > 1e-12 {
            (
                [
                    r[i][j].atan2(r[i][k]),
                    support.atan2(r[i][i]),
                    r[j][i].atan2(-r[k][i]),
                ],
                false,
            )
        } else {
            (
                [(-r[j][k]).atan2(r[j][j]), support.atan2(r[i][i]), 0.],
                true,
            )
        }
    } else {
        let support = r[i][i].hypot(r[j][i]);
        if support > 1e-12 {
            (
                [
                    r[k][j].atan2(r[k][k]),
                    (-r[k][i]).atan2(support),
                    r[j][i].atan2(r[i][i]),
                ],
                false,
            )
        } else {
            (
                [(-r[j][k]).atan2(r[j][j]), (-r[k][i]).atan2(support), 0.],
                true,
            )
        }
    };
    if odd {
        for angle in &mut angles {
            *angle = -*angle;
        }
    }
    if repeated && angles[1] < 0. {
        angles = [angles[0] + PI, -angles[1], angles[2] - PI];
    }
    angles[0] = wrap(angles[0]);
    angles[2] = wrap(angles[2]);
    if intrinsic {
        angles.swap(0, 2);
    }
    Ok((angles, singular))
}

fn fixed(value: f64, precision: usize) -> Result<String, String> {
    if !value.is_finite() {
        return Err("formatted value overflowed".into());
    }
    let rounded = if value.abs() < 0.5 * 10f64.powi(-(precision as i32)) {
        0.
    } else {
        value
    };
    Ok(format!("{rounded:.precision$}"))
}

pub(super) fn render(record: &Value, options: &Options) -> Result<Vec<Value>, String> {
    if serde_json::to_vec(record).map_err(|e| e.to_string())?.len() > 1024 * 1024 {
        return Err("log payload exceeds 1 MiB".into());
    }
    if options.kind == Kind::String {
        if record["type"] != "std.Value.v1"
            || record["data"]["kind"] != "string"
            || record["data"]["unit"] != ""
        {
            return Err("string log requires std.Value.v1/string".into());
        }
        let value = record["data"]["value"]
            .as_str()
            .ok_or("string log requires a string value")?;
        return Ok(vec![json!({"type":"string", "value":value})]);
    }
    let dimension = if options.kind == Kind::Poses2d { 2 } else { 3 };
    let single = format!("vision.Pose{dimension}D.v1");
    let collection = format!("vision.Poses{dimension}D.v1");
    let ty = record["type"]
        .as_str()
        .ok_or("log requires a typed Record")?;
    let data = record.get("data").ok_or("log requires Record data")?;
    let frame = text(&data["frame"], "frame")?;
    let unit = text(&data["unit"], "unit")?;
    if (dimension == 3 && unit != "m") || (dimension == 2 && !["m", "px"].contains(&unit)) {
        return Err("pose units must be m (3D), or m/px (2D)".into());
    }
    let items: Vec<&Value> = if ty == single {
        vec![data]
    } else if ty == collection {
        data["items"]
            .as_array()
            .ok_or("pose collection needs items")?
            .iter()
            .collect()
    } else {
        return Err(format!("expected {single} or {collection}"));
    };
    if items.len() > 4096 {
        return Err("pose collection exceeds 4096 items".into());
    }
    let output_unit = options.length_unit.as_deref().unwrap_or(unit);
    let (scale, location_precision) = match (unit, output_unit) {
        ("m", "m") => (1., 5),
        ("m", "mm") => (1000., 2),
        ("px", "px") => (1., 4),
        _ => return Err("pixel and physical units cannot be converted without calibration".into()),
    };
    if dimension == 3 && options.pose_format != "quat" {
        euler([0., 0., 0., 1.], &options.pose_format)?;
    }
    let mut result = Vec::new();
    let mut ids = std::collections::HashSet::new();
    for item in items.iter() {
        let id = text(&item["id"], "id")?;
        if !ids.insert(id) || item["frame"] != frame || item["unit"] != unit {
            return Err(
                "pose IDs must be unique and units/frames must agree with the collection".into(),
            );
        }
        let position = item["position"]
            .as_array()
            .filter(|p| p.len() == dimension)
            .ok_or("pose position has invalid dimension")?;
        let size = item["size"]
            .as_array()
            .filter(|p| p.len() == dimension)
            .ok_or("pose size has invalid dimension")?;
        for value in size {
            if number(value, "size")? <= 0. {
                return Err("pose size must be positive".into());
            }
        }
        let mut values = position
            .iter()
            .map(|v| fixed(number(v, "position")? * scale, location_precision))
            .collect::<Result<Vec<_>, String>>()?;
        let (rotation, singular) = if dimension == 2 {
            (vec![number(&item["angle"], "angle")?], false)
        } else {
            let q = quaternion(&item["rotation"])?;
            if options.pose_format == "quat" {
                (q.to_vec(), false)
            } else {
                let (angles, singular) = euler(q, &options.pose_format)?;
                (angles.to_vec(), singular)
            }
        };
        let angles = dimension == 2 || options.pose_format != "quat";
        for value in rotation {
            values.push(fixed(
                if angles && options.angle_degrees {
                    value * 180. / PI
                } else {
                    value
                },
                if angles && options.angle_degrees {
                    3
                } else {
                    5
                },
            )?);
        }
        result.push(json!({"type":if dimension==2 {"pose2d"} else {"pose3d"}, "id":id,"frame":frame,"unit":output_unit,
            "angleUnit":if angles {if options.angle_degrees {"deg"}else{"rad"}}else{"unit_quaternion"},
            "rotationFormat":if dimension==2 {"angle"}else{options.pose_format.as_str()},"gimbalLock":singular,"values":values,"original":item}));
    }
    if options.first {
        result.truncate(1);
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn multiply(a: [[f64; 3]; 3], b: [[f64; 3]; 3]) -> [[f64; 3]; 3] {
        let mut r = [[0.; 3]; 3];
        for i in 0..3 {
            for j in 0..3 {
                for k in 0..3 {
                    r[i][j] += a[i][k] * b[k][j];
                }
            }
        }
        r
    }

    fn axes_matrix(sequence: &str, angles: [f64; 3]) -> [[f64; 3]; 3] {
        let mut r = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]];
        for (axis, angle) in sequence.bytes().zip(angles) {
            let mut m = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]];
            let i = (axis.to_ascii_lowercase() - b'x') as usize;
            let j = (i + 1) % 3;
            let k = (i + 2) % 3;
            m[j][j] = angle.cos();
            m[k][k] = angle.cos();
            m[j][k] = -angle.sin();
            m[k][j] = angle.sin();
            r = if axis.is_ascii_uppercase() {
                multiply(r, m)
            } else {
                multiply(m, r)
            };
        }
        r
    }

    #[test]
    fn all_intrinsic_and_extrinsic_euler_sequences_reconstruct_nonsymmetric_rotations_and_singularities(
    ) {
        for sequence in [
            "xyz", "xzy", "yxz", "yzx", "zxy", "zyx", "xyx", "xzx", "yxy", "yzy", "zxz", "zyz",
        ] {
            for name in [sequence.to_string(), sequence.to_ascii_uppercase()] {
                for q in [
                    [0., 0., 0., 1.],
                    [0.2, -0.3, 0.4, 0.8426149773176358],
                    [0.5, 0.5, -0.5, 0.5],
                    [
                        0.,
                        std::f64::consts::FRAC_1_SQRT_2,
                        0.,
                        std::f64::consts::FRAC_1_SQRT_2,
                    ],
                ] {
                    let q = quaternion(&json!(q)).unwrap();
                    let (angles, _) = euler(q, &name).unwrap();
                    let actual = axes_matrix(&name, angles);
                    let expected = matrix(q);
                    for i in 0..3 {
                        for j in 0..3 {
                            assert!(
                                (actual[i][j] - expected[i][j]).abs() < 1e-10,
                                "{name} {q:?} {angles:?} {actual:?} != {expected:?}"
                            );
                        }
                    }
                }
            }
        }
        for bad in ["xyZ", "xxz", "xxx", "xy", "quaternion", "xyq"] {
            assert!(euler([0., 0., 0., 1.], bad).is_err());
        }
    }

    #[test]
    fn pose_text_conversion_is_explicit_about_units_identity_fixed_precision_and_empty_collections()
    {
        let one = json!({"id":"部件A","frame":"world","unit":"m","position":[0.00125,-0.002,0.003],"size":[1,2,3],"rotation":[0,0,0,1],"attributes":{"source":"camera"}});
        let mut data = json!({"type":"vision.Poses3D.v1","data":{"frame":"world","unit":"m","items":[one.clone()]}});
        let mut options = Options {
            kind: Kind::Poses3d,
            first: false,
            pose_format: "ZYX".into(),
            length_unit: Some("mm".into()),
            angle_degrees: true,
        };
        let output = render(&data, &options).unwrap();
        assert_eq!(
            output[0]["values"],
            json!(["1.25", "-2.00", "3.00", "0.000", "0.000", "0.000"])
        );
        assert_eq!(output[0]["original"], one);
        options.pose_format = "quat".into();
        assert_eq!(
            render(&data, &options).unwrap()[0]["angleUnit"],
            "unit_quaternion"
        );
        data["data"]["items"] = json!([]);
        assert!(render(&data, &options).unwrap().is_empty());
        options.kind = Kind::Poses2d;
        options.length_unit = Some("m".into());
        let pixel = json!({"type":"vision.Pose2D.v1","data":{"id":"pixel","frame":"image","unit":"px","position":[1,2],"size":[3,4],"angle":0}});
        assert!(render(&pixel, &options).is_err());
        options.length_unit = None;
        assert_eq!(render(&pixel, &options).unwrap()[0]["unit"], "px");
        options.kind = Kind::String;
        let text = json!({"type":"std.Value.v1","data":{"kind":"string","unit":"","value":"a\nb"}});
        assert_eq!(render(&text, &options).unwrap()[0]["value"], "a\nb");
    }
}
