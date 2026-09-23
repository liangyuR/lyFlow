//! 最小的 PCD 读取：CLI 的 `--input` 要把文件**原样**交给 core（m8-plan L18）。
//!
//! 为什么不借 core 的 `io.load_pcd` 再用 `lyflow_output_cloud` 取回来：那条取数通道（`lyflow_cloud_view`）
//! 只有 xyz / intensity / normals，没有 rgb —— 而 gap 的模型定位把传感器写在 rgb 的 R 上的强度当特征，
//! 丢了它，注入的云与从目录读的就不是同一份。给视图结构加字段是 ABI 破坏，所以在这里读。
//! 支持 `ascii` / `binary` / `binary_compressed`（LZF）三种 DATA；字段认 x y z、intensity、
//! normal_x/y/z、rgb / rgba，其余字段跳过。点的顺序与 NaN 槽原样保留。

use std::path::Path;

#[derive(Debug, Default, Clone, PartialEq)]
pub struct PcdCloud {
    pub xyz: Vec<f32>,
    pub intensity: Vec<f32>,
    pub normals: Vec<f32>,
    pub rgb: Vec<u8>,
}

#[derive(Debug, Clone)]
struct Field {
    name: String,
    size: usize,
    kind: char,
    count: usize,
}

impl Field {
    fn bytes(&self) -> usize {
        self.size * self.count
    }
}

/// 一个标量按声明的类型读成 f64（小端，PCD 规定）。
fn scalar(bytes: &[u8], kind: char, size: usize) -> f64 {
    let b = |n: usize| {
        let mut a = [0u8; 8];
        a[..n].copy_from_slice(&bytes[..n]);
        a
    };
    match (kind, size) {
        ('F', 4) => f64::from(f32::from_le_bytes(b(4)[..4].try_into().unwrap())),
        ('F', 8) => f64::from_le_bytes(b(8)),
        ('U', 1) => f64::from(bytes[0]),
        ('U', 2) => f64::from(u16::from_le_bytes([bytes[0], bytes[1]])),
        ('U', 4) => f64::from(u32::from_le_bytes(b(4)[..4].try_into().unwrap())),
        ('I', 1) => f64::from(bytes[0] as i8),
        ('I', 2) => f64::from(i16::from_le_bytes([bytes[0], bytes[1]])),
        ('I', 4) => f64::from(i32::from_le_bytes(b(4)[..4].try_into().unwrap())),
        _ => f64::NAN,
    }
}

fn unpack_rgb(packed: u32, out: &mut Vec<u8>) {
    out.push(((packed >> 16) & 0xFF) as u8);
    out.push(((packed >> 8) & 0xFF) as u8);
    out.push((packed & 0xFF) as u8);
}

/// LZF 解压（PCD 的 binary_compressed 用它）。
pub fn lzf_decompress(input: &[u8], out_len: usize) -> Result<Vec<u8>, String> {
    let mut out = Vec::with_capacity(out_len);
    let mut i = 0;
    let bad = || "binary_compressed 数据损坏（LZF 越界）".to_string();
    while i < input.len() {
        let ctrl = input[i] as usize;
        i += 1;
        if ctrl < 32 {
            let len = ctrl + 1;
            let chunk = input.get(i..i + len).ok_or_else(bad)?;
            out.extend_from_slice(chunk);
            i += len;
        } else {
            let mut len = ctrl >> 5;
            if len == 7 {
                len += *input.get(i).ok_or_else(bad)? as usize;
                i += 1;
            }
            let back = ((ctrl & 0x1F) << 8) + *input.get(i).ok_or_else(bad)? as usize + 1;
            i += 1;
            len += 2;
            if back > out.len() {
                return Err(bad());
            }
            let start = out.len() - back;
            for k in 0..len {
                let v = out[start + k];
                out.push(v);
            }
        }
    }
    if out.len() != out_len {
        return Err(format!("binary_compressed 解压出 {} 字节，头里写的是 {out_len}", out.len()));
    }
    Ok(out)
}

pub fn read_pcd(path: &Path) -> Result<PcdCloud, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("读取 {} 失败: {e}", path.display()))?;
    parse_pcd(&bytes).map_err(|e| format!("{}: {e}", path.display()))
}

pub fn parse_pcd(bytes: &[u8]) -> Result<PcdCloud, String> {
    // -- 头：逐行读到 DATA 为止
    let mut pos = 0;
    let mut names: Vec<String> = Vec::new();
    let mut sizes: Vec<usize> = Vec::new();
    let mut kinds: Vec<char> = Vec::new();
    let mut counts: Vec<usize> = Vec::new();
    let (mut width, mut height, mut points) = (0usize, 1usize, None::<usize>);
    let data;
    loop {
        let end = bytes[pos..]
            .iter()
            .position(|&b| b == b'\n')
            .map(|p| pos + p)
            .ok_or("头没有 DATA 行")?;
        let line = std::str::from_utf8(&bytes[pos..end]).map_err(|_| "头不是文本")?.trim();
        pos = end + 1;
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut it = line.split_whitespace();
        let key = it.next().unwrap_or("").to_ascii_uppercase();
        let rest: Vec<&str> = it.collect();
        let nums = |v: &[&str]| -> Result<Vec<usize>, String> {
            v.iter().map(|s| s.parse::<usize>().map_err(|_| format!("{key} 里有非数字 {s}"))).collect()
        };
        match key.as_str() {
            "FIELDS" => names = rest.iter().map(|s| s.to_string()).collect(),
            "SIZE" => sizes = nums(&rest)?,
            "TYPE" => kinds = rest.iter().map(|s| s.chars().next().unwrap_or('F')).collect(),
            "COUNT" => counts = nums(&rest)?,
            "WIDTH" => width = nums(&rest)?.first().copied().unwrap_or(0),
            "HEIGHT" => height = nums(&rest)?.first().copied().unwrap_or(1),
            "POINTS" => points = nums(&rest)?.first().copied(),
            "DATA" => {
                data = rest.first().map(|s| s.to_ascii_lowercase()).unwrap_or_default();
                break;
            }
            _ => {}
        }
    }
    if counts.is_empty() {
        counts = vec![1; names.len()];
    }
    if sizes.len() != names.len() || kinds.len() != names.len() || counts.len() != names.len() {
        return Err("FIELDS / SIZE / TYPE / COUNT 的个数对不上".into());
    }
    let fields: Vec<Field> = (0..names.len())
        .map(|i| Field { name: names[i].clone(), size: sizes[i], kind: kinds[i], count: counts[i] })
        .collect();
    let n = points.unwrap_or(width * height);
    let find = |name: &str| fields.iter().position(|f| f.name == name);
    let (fx, fy, fz) = match (find("x"), find("y"), find("z")) {
        (Some(x), Some(y), Some(z)) => (x, y, z),
        _ => return Err("文件里没有 x / y / z 字段".into()),
    };
    let fi = find("intensity");
    let normals = match (find("normal_x"), find("normal_y"), find("normal_z")) {
        (Some(a), Some(b), Some(c)) => Some([a, b, c]),
        _ => None,
    };
    let frgb = find("rgb").or_else(|| find("rgba"));

    let mut cloud = PcdCloud {
        xyz: Vec::with_capacity(n * 3),
        ..PcdCloud::default()
    };
    let body = &bytes[pos..];

    if data == "ascii" {
        let text = std::str::from_utf8(body).map_err(|_| "ascii 数据不是文本")?;
        for line in text.lines().map(str::trim).filter(|l| !l.is_empty()).take(n) {
            let tokens: Vec<&str> = line.split_whitespace().collect();
            // 每个字段的第一个分量在这一行里的下标
            let mut at = Vec::with_capacity(fields.len());
            let mut k = 0;
            for f in &fields {
                at.push(k);
                k += f.count;
            }
            // 直接按 f32 解析：先解成 f64 再截成 f32 是两次舍入，偶尔差最后一位
            let num = |f: usize| -> f32 {
                tokens.get(at[f]).and_then(|t| t.parse::<f32>().ok()).unwrap_or(f32::NAN)
            };
            cloud.xyz.extend([num(fx), num(fy), num(fz)]);
            if let Some(f) = fi {
                cloud.intensity.push(num(f));
            }
            if let Some(ns) = normals {
                cloud.normals.extend(ns.map(num));
            }
            if let Some(f) = frgb {
                let token = tokens.get(at[f]).copied().unwrap_or("0");
                // F 类型的 rgb 是把 32 位打包值按 float 写出来的（PCL 的写法）
                let packed = if fields[f].kind == 'F' {
                    token.parse::<f32>().map(f32::to_bits).unwrap_or(0)
                } else {
                    token.parse::<f64>().map(|v| v as u32).unwrap_or(0)
                };
                unpack_rgb(packed, &mut cloud.rgb);
            }
        }
        if cloud.xyz.len() != n * 3 {
            return Err(format!("ascii 数据只有 {} 个点，头里写的是 {n}", cloud.xyz.len() / 3));
        }
        return Ok(cloud);
    }

    // 两种二进制布局：binary 是逐点排（AoS），binary_compressed 解压之后是逐字段排（SoA）
    let (buffer, soa) = match data.as_str() {
        "binary" => (body.to_vec(), false),
        "binary_compressed" => {
            if body.len() < 8 {
                return Err("binary_compressed 缺长度头".into());
            }
            let packed = u32::from_le_bytes(body[0..4].try_into().unwrap()) as usize;
            let raw = u32::from_le_bytes(body[4..8].try_into().unwrap()) as usize;
            let chunk = body.get(8..8 + packed).ok_or("binary_compressed 数据被截断")?;
            (lzf_decompress(chunk, raw)?, true)
        }
        other => return Err(format!("不认识的 DATA 格式 {other}")),
    };
    let step: usize = fields.iter().map(Field::bytes).sum();
    let mut starts = Vec::with_capacity(fields.len());
    let mut acc = 0;
    for f in &fields {
        starts.push(acc);
        acc += if soa { f.bytes() * n } else { f.bytes() };
    }
    if buffer.len() < step * n {
        return Err(format!("二进制数据 {} 字节，{n} 个点要 {} 字节", buffer.len(), step * n));
    }
    let at = |f: usize, i: usize| -> &[u8] {
        let off = if soa { starts[f] + i * fields[f].bytes() } else { i * step + starts[f] };
        &buffer[off..off + fields[f].size]
    };
    let num = |f: usize, i: usize| scalar(at(f, i), fields[f].kind, fields[f].size);
    for i in 0..n {
        cloud.xyz.extend([num(fx, i) as f32, num(fy, i) as f32, num(fz, i) as f32]);
        if let Some(f) = fi {
            cloud.intensity.push(num(f, i) as f32);
        }
        if let Some(ns) = normals {
            cloud.normals.extend(ns.map(|f| num(f, i) as f32));
        }
        if let Some(f) = frgb {
            let b = at(f, i);
            let packed = if b.len() >= 4 { u32::from_le_bytes(b[..4].try_into().unwrap()) } else { 0 };
            unpack_rgb(packed, &mut cloud.rgb);
        }
    }
    Ok(cloud)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lzf_literal_then_backreference() {
        // "abc" 字面量 + 「往回 3 个字节、复制 6 个」
        let packed = [2u8, b'a', b'b', b'c', 128, 2];
        assert_eq!(lzf_decompress(&packed, 9).unwrap(), b"abcabcabc");
        assert!(lzf_decompress(&packed, 10).is_err());
    }

    #[test]
    fn ascii_keeps_nan_slots_and_float_packed_rgb() {
        let rgb = f32::from_bits(0x00_10_20_30);
        let text = format!(
            "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z rgb\nSIZE 4 4 4 4\nTYPE F F F F\nCOUNT 1 1 1 1\n\
             WIDTH 2\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 2\nDATA ascii\n1 2 3 {rgb:e}\nnan 0 nan {rgb:e}\n"
        );
        let c = parse_pcd(text.as_bytes()).unwrap();
        assert_eq!(&c.xyz[..3], &[1.0, 2.0, 3.0]);
        assert!(c.xyz[3].is_nan() && c.xyz[5].is_nan());
        assert_eq!(c.rgb, vec![0x10, 0x20, 0x30, 0x10, 0x20, 0x30]);
    }

    #[test]
    fn binary_reads_every_field_in_point_order() {
        let mut body = Vec::new();
        for (x, v) in [(0.5f32, 0x00_01_02_03u32), (-1.25, 0x00_ff_00_7f)] {
            body.extend(x.to_le_bytes());
            body.extend(0f32.to_le_bytes());
            body.extend((x * 2.0).to_le_bytes());
            body.extend(v.to_le_bytes());
        }
        let mut file = b"VERSION 0.7\nFIELDS x y z rgb\nSIZE 4 4 4 4\nTYPE F F F U\nCOUNT 1 1 1 1\n\
WIDTH 2\nHEIGHT 1\nPOINTS 2\nDATA binary\n"
            .to_vec();
        file.extend(body);
        let c = parse_pcd(&file).unwrap();
        assert_eq!(c.xyz, vec![0.5, 0.0, 1.0, -1.25, 0.0, -2.5]);
        assert_eq!(c.rgb, vec![1, 2, 3, 0xff, 0, 0x7f]);
        assert!(c.intensity.is_empty());
    }
}
