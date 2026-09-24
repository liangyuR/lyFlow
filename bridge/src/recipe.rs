//! 参数配方（param-recipe P4.1）的 Rust 实现：读配方文件、配方目录、规格摘要（specDigest）、
//! 四类失配与修复建议，外加 `lyflow recipes` 子命令与 `--recipe` 的应用。
//!
//! 规则写在 docs/recipe.md。编辑器的 `packages/editor/src/lib/recipes.ts` 是另一份实现，两边对着
//! 同一组夹具（`schema/fixtures/recipes/`）验：摘要逐字节相同，每一条失配的类别、参数、建议、文案都相同。
//! 改规则时两边与夹具一起改。
//!
//! 配方 = 顶层图参数的稀疏覆盖（K1），有效值 = default ← 配方（K4）。core 不知道配方的存在（K3）：
//! CLI 把配方里的值写成对应图参数的 default（与 `--param` 同一条路，所以 `--param` 排在后面、优先），
//! 宿主自己合成 `params_json`（docs/embedding.md「按名字切换配方」）。

use std::path::{Path, PathBuf};

use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};

use crate::cli::{fail, json_line, line, Parsed, Sink, EXIT_INVALID, EXIT_OK, EXIT_USAGE};
use crate::graph::GraphDoc;

pub const RECIPE_SCHEMA_VERSION: u64 = 1;
pub const RECIPE_EXT: &str = ".lyflow-recipe.json";
/// 目录下可选的索引：`{ default?: 名字, order?: [名字…] }`。
pub const RECIPE_INDEX: &str = "index.json";
/// 编辑器 30 s 自动备份写的那一份，不是配方文件。
pub const RECIPE_AUTOSAVE: &str = "autosave~.json";

// ------------------------------------------------------------ 与 JS 一致的文本

/// ECMAScript 的 Number::toString（`JSON.stringify` 与模板字符串里的数都是它）：最短往返的有效数字，
/// 指数在 [-7, 21) 之外才写成 `1e+21` / `1e-7`。Rust 的 `{:e}` 给的正是最短往返的那组数字。
pub fn js_number(x: f64) -> String {
    if x.is_nan() {
        return "NaN".to_string();
    }
    if x.is_infinite() {
        return if x > 0.0 { "Infinity" } else { "-Infinity" }.to_string();
    }
    if x == 0.0 {
        return "0".to_string();
    }
    let sci = format!("{:e}", x.abs());
    let (mant, exp) = sci.split_once('e').unwrap_or((sci.as_str(), "0"));
    let digits: String = mant.chars().filter(|c| *c != '.').collect();
    let k = digits.len() as i32;
    let n = exp.parse::<i32>().unwrap_or(0) + 1;
    let body = if k <= n && n <= 21 {
        format!("{digits}{}", "0".repeat((n - k) as usize))
    } else if 0 < n && n <= 21 {
        format!("{}.{}", &digits[..n as usize], &digits[n as usize..])
    } else if -6 < n && n <= 0 {
        format!("0.{}{digits}", "0".repeat((-n) as usize))
    } else {
        let e = n - 1;
        let sign = if e >= 0 { '+' } else { '-' };
        let (d0, rest) = digits.split_at(1);
        if rest.is_empty() {
            format!("{d0}e{sign}{}", e.abs())
        } else {
            format!("{d0}.{rest}e{sign}{}", e.abs())
        }
    };
    if x < 0.0 {
        format!("-{body}")
    } else {
        body
    }
}

/// JS 对象里「数组下标」那样的键（0 .. 2^32-2 的规范十进制）排在其它键前面。
fn index_key(k: &str) -> Option<u64> {
    if k.is_empty() || (k.len() > 1 && k.starts_with('0')) || !k.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    k.parse::<u64>().ok().filter(|n| *n < 4_294_967_295)
}

/// `JSON.stringify(v)`：无空白，数按 [`js_number`]，字符串的转义与 serde_json 相同（控制字符
/// `\u00xx` 小写、非 ASCII 原样），对象的键序与 JS 一致（下标键在前、按数值升序，其余按插入序）。
pub fn js_json(v: &Value) -> String {
    let mut out = String::new();
    write_js_json(v, &mut out);
    out
}

fn write_js_json(v: &Value, out: &mut String) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => out.push_str(&js_number(n.as_f64().unwrap_or(0.0))),
        Value::String(s) => out.push_str(&serde_json::to_string(s).unwrap_or_default()),
        Value::Array(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_js_json(x, out);
            }
            out.push(']');
        }
        Value::Object(o) => {
            let mut keys: Vec<&String> = o.keys().collect();
            let mut indexed: Vec<(u64, &String)> =
                keys.iter().filter_map(|k| index_key(k).map(|n| (n, *k))).collect();
            indexed.sort();
            keys.retain(|k| index_key(k).is_none());
            out.push('{');
            for (i, k) in indexed.iter().map(|(_, k)| *k).chain(keys).enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push_str(&serde_json::to_string(k).unwrap_or_default());
                out.push(':');
                write_js_json(&o[k.as_str()], out);
            }
            out.push('}');
        }
    }
}

/// `String(v)`（模板字符串、`Array.prototype.join` 用的那个）。
fn js_string(v: &Value) -> String {
    match v {
        Value::Null => "null".to_string(),
        Value::Bool(b) => b.to_string(),
        Value::Number(n) => js_number(n.as_f64().unwrap_or(0.0)),
        Value::String(s) => s.clone(),
        Value::Array(a) => a
            .iter()
            .map(|x| if x.is_null() { String::new() } else { js_string(x) })
            .collect::<Vec<_>>()
            .join(","),
        Value::Object(_) => "[object Object]".to_string(),
    }
}

/// 编辑器的 `short`：JSON 文本，超过 48 个 UTF-16 码元时截成 45 个加「…」。
fn short(v: &Value) -> String {
    let s = js_json(v);
    if s.encode_utf16().count() <= 48 {
        return s;
    }
    let mut units = 0;
    let mut cut = String::new();
    for c in s.chars() {
        units += c.len_utf16();
        if units > 45 {
            break;
        }
        cut.push(c);
    }
    format!("{cut}…")
}

/// JS 的一个数落成 JSON 值：整数写成整数（`5` 而不是 `5.0`），与 `JSON.stringify` 的文本一致。
fn num_value(x: f64) -> Value {
    if x == x.trunc() && x.abs() < 9_007_199_254_740_992.0 {
        json!(x as i64)
    } else {
        json!(x)
    }
}

fn num(v: &Value) -> Option<f64> {
    v.as_f64()
}

/// JS 的 `String.prototype.trim`：Unicode 空白外加 BOM。
fn js_trim(s: &str) -> &str {
    s.trim_matches(|c: char| c.is_whitespace() || c == '\u{feff}')
}

/// 编辑器 `asNumber` 的字符串那一半：`^\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?\s*$`，再 `Number(v)`。
fn decimal_string(s: &str) -> Option<f64> {
    let t = js_trim(s);
    let b = t.as_bytes();
    let mut i = 0;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let int_start = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    let int_digits = i - int_start;
    if i < b.len() && b[i] == b'.' {
        i += 1;
        let frac_start = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        if int_digits == 0 && i == frac_start {
            return None;
        }
    } else if int_digits == 0 {
        return None;
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        i += 1;
        if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
            i += 1;
        }
        let exp_start = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        if i == exp_start {
            return None;
        }
    }
    if i != b.len() {
        return None;
    }
    t.parse::<f64>().ok().filter(|x| x.is_finite())
}

/// 能不能当成一个数：数，或写成十进制数的字符串。
fn as_number(v: &Value) -> Option<f64> {
    match v {
        Value::Number(_) => num(v),
        Value::String(s) => decimal_string(s),
        _ => None,
    }
}

/// 四舍五入，.5 远离 0（编辑器的 roundHalfAway 与它同一个口径）。
fn round_half_away(x: f64) -> f64 {
    x.round()
}

// ------------------------------------------------------------ 规格摘要（specDigest）

fn finite_or_null(v: Option<&Value>) -> Value {
    match v {
        Some(n @ Value::Number(_)) => n.clone(),
        _ => Value::Null,
    }
}

fn options_canonical(options: Option<&Value>) -> Value {
    let Some(list) = options.and_then(Value::as_array).filter(|a| !a.is_empty()) else {
        return Value::Null;
    };
    let mut values: Vec<(String, Value)> = list
        .iter()
        .map(|o| {
            let v = o.get("value").cloned().unwrap_or(Value::Null);
            (js_json(&v), v)
        })
        .collect();
    // 按各自 JSON 文本的码点序（Rust 字符串按 UTF-8 字节比，与码点序相同）；sort_by 是稳定的
    values.sort_by(|a, b| a.0.cmp(&b.0));
    Value::Array(values.into_iter().map(|(_, v)| v).collect())
}

/// 规格摘要的规范形（docs/recipe.md §3）：每个图参数一个五元组 `[名字, type, min, max, options]`，
/// 按名字的码点序排；没有 type 的老格式只留名字；options 只取 value、按 JSON 文本排序、空的算 null。
pub fn spec_canonical(params: &Map<String, Value>) -> String {
    let mut rows: Vec<(&String, String)> = params
        .iter()
        .map(|(name, gp)| {
            let row = match gp.get("type").and_then(Value::as_str) {
                Some(t) => json!([
                    name,
                    t,
                    finite_or_null(gp.get("min")),
                    finite_or_null(gp.get("max")),
                    options_canonical(gp.get("options")),
                ]),
                None => json!([name, null, null, null, null]),
            };
            (name, js_json(&row))
        })
        .collect();
    rows.sort_by(|a, b| a.0.cmp(b.0));
    let joined: Vec<&str> = rows.iter().map(|(_, r)| r.as_str()).collect();
    format!("[{}]", joined.join(","))
}

/// `sha256:<64 位小写十六进制>`，对 [`spec_canonical`] 的 UTF-8 字节。
pub fn spec_digest(params: &Map<String, Value>) -> String {
    let digest = Sha256::digest(spec_canonical(params).as_bytes());
    let hex: String = digest.iter().map(|b| format!("{b:02x}")).collect();
    format!("sha256:{hex}")
}

// ------------------------------------------------------------ 读文件

#[derive(Clone, Debug, PartialEq)]
pub struct GraphRef {
    pub id: String,
    pub spec_digest: String,
}

#[derive(Clone, Debug)]
pub struct RecipeFile {
    /// 配方名 = 文件名去掉 `.lyflow-recipe.json`（与文件里的 name 不一致时以文件名为准）。
    pub name: String,
    pub path: PathBuf,
    pub values: Map<String, Value>,
    pub note: Option<String>,
    /// 文件里记着的图 id 与规格摘要（失配 ④ 拿它比）。没写就是 None。
    pub graph: Option<GraphRef>,
    pub updated_at: Option<String>,
    /// 读得出来、但不完全合格的地方（缺字段、多字段）。不拦。
    pub notes: Vec<String>,
}

/// `A.lyflow-recipe.json` → `A`（扩展名不分大小写）；不是配方文件返回 None。
pub fn recipe_stem(file_name: &str) -> Option<&str> {
    let n = file_name.len();
    let ext = RECIPE_EXT.len();
    (n > ext && file_name.is_char_boundary(n - ext) && file_name[n - ext..].eq_ignore_ascii_case(RECIPE_EXT))
        .then(|| &file_name[..n - ext])
}

/// 宽松地读一个配方（与编辑器的 parseRecipeText 同一套）：缺的字段补上并记一笔；真读不出来
/// （不是 JSON、顶层不是对象、schemaVersion 比这个 lyflow 新）才是 Err。
pub fn parse_recipe_text(text: &str, name: &str, path: &Path) -> Result<RecipeFile, String> {
    let raw: Value = serde_json::from_str(text.trim_start_matches('\u{feff}'))
        .map_err(|e| format!("不是合法的 JSON：{e}"))?;
    let Some(o) = raw.as_object() else {
        return Err("顶层应当是一个对象".to_string());
    };
    let mut notes = Vec::new();
    match o.get("schemaVersion") {
        Some(Value::Number(v)) if v.as_f64().unwrap_or(0.0) > RECIPE_SCHEMA_VERSION as f64 => {
            return Err(format!(
                "schemaVersion {} 比这个 lyflow 认识的（{RECIPE_SCHEMA_VERSION}）新",
                js_string(&Value::Number(v.clone()))
            ));
        }
        None => notes.push("缺 schemaVersion".to_string()),
        _ => {}
    }
    if !o.get("name").is_some_and(Value::is_string) {
        notes.push("缺 name，用了文件名".to_string());
    }
    let values = match o.get("values") {
        Some(Value::Object(v)) => v.clone(),
        _ => {
            notes.push("values 不是对象，当作空配方".to_string());
            Map::new()
        }
    };
    let graph = o.get("graph").and_then(|g| {
        Some(GraphRef {
            id: g.get("id")?.as_str()?.to_string(),
            spec_digest: g.get("specDigest")?.as_str()?.to_string(),
        })
    });
    const KNOWN: &[&str] = &["schemaVersion", "name", "graph", "values", "note", "updatedAt"];
    let extra: Vec<&str> = o.keys().map(String::as_str).filter(|k| !KNOWN.contains(k)).collect();
    if !extra.is_empty() {
        notes.push(format!("不认识的字段 {}", extra.join("、")));
    }
    Ok(RecipeFile {
        name: name.to_string(),
        path: path.to_path_buf(),
        values,
        note: o.get("note").and_then(Value::as_str).map(str::to_owned),
        graph,
        updated_at: o.get("updatedAt").and_then(Value::as_str).map(str::to_owned),
        notes,
    })
}

pub fn read_recipe_file(path: &Path) -> Result<RecipeFile, String> {
    let file_name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let name = recipe_stem(&file_name).unwrap_or(&file_name).to_string();
    let text = std::fs::read_to_string(path).map_err(|e| format!("读取 {} 失败：{e}", path.display()))?;
    parse_recipe_text(&text, &name, path)
}

// ------------------------------------------------------------ 目录约定

/// 图文件同目录的 `<图文件名去扩展名>.recipes/`：`.lyflow.json` 整个去掉，其它文件去最后一个扩展名。
pub fn recipe_dir_of(graph: &Path) -> PathBuf {
    let file = graph.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    const GRAPH_EXT: &str = ".lyflow.json";
    let n = file.len();
    let stem = if n >= GRAPH_EXT.len()
        && file.is_char_boundary(n - GRAPH_EXT.len())
        && file[n - GRAPH_EXT.len()..].eq_ignore_ascii_case(GRAPH_EXT)
    {
        &file[..n - GRAPH_EXT.len()]
    } else {
        match file.rfind('.') {
            Some(dot) if dot > 0 => &file[..dot],
            _ => file.as_str(),
        }
    };
    graph.with_file_name(format!("{stem}.recipes"))
}

#[derive(Clone, Debug, Default)]
pub struct RecipeIndex {
    pub default: Option<String>,
    pub order: Vec<String>,
}

pub fn parse_index(text: &str) -> RecipeIndex {
    let Ok(v) = serde_json::from_str::<Value>(text.trim_start_matches('\u{feff}')) else {
        return RecipeIndex::default();
    };
    RecipeIndex {
        default: v.get("default").and_then(Value::as_str).map(str::to_owned),
        order: v
            .get("order")
            .and_then(Value::as_array)
            .map(|a| a.iter().filter_map(Value::as_str).map(str::to_owned).collect())
            .unwrap_or_default(),
    }
}

pub struct RecipeDir {
    pub dir: PathBuf,
    pub exists: bool,
    /// 按 index.json 的 order 排；没列到的按名字（码点序）接在后面。
    pub recipes: Vec<RecipeFile>,
    /// index.json 的 default，且目录里真有这个配方。
    pub default: Option<String>,
    pub order: Vec<String>,
    /// 读不出来的文件：(文件名, 原因)。
    pub problems: Vec<(String, String)>,
}

pub fn list_recipe_dir(dir: &Path) -> RecipeDir {
    let mut out = RecipeDir {
        dir: dir.to_path_buf(),
        exists: dir.is_dir(),
        recipes: Vec::new(),
        default: None,
        order: Vec::new(),
        problems: Vec::new(),
    };
    let Ok(entries) = std::fs::read_dir(dir) else {
        return out;
    };
    let mut index = RecipeIndex::default();
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let file = entry.file_name().to_string_lossy().into_owned();
        if file.eq_ignore_ascii_case(RECIPE_INDEX) {
            index = std::fs::read_to_string(&path).map(|t| parse_index(&t)).unwrap_or_default();
            continue;
        }
        let Some(stem) = recipe_stem(&file) else { continue };
        match read_recipe_file(&path) {
            Ok(mut r) => {
                r.name = stem.to_string();
                out.recipes.push(r);
            }
            Err(e) => out.problems.push((file, e)),
        }
    }
    let rank = |name: &str| index.order.iter().position(|n| n == name);
    out.recipes.sort_by(|a, b| match (rank(&a.name), rank(&b.name)) {
        (Some(x), Some(y)) => x.cmp(&y),
        (Some(_), None) => std::cmp::Ordering::Less,
        (None, Some(_)) => std::cmp::Ordering::Greater,
        (None, None) => a.name.cmp(&b.name),
    });
    out.problems.sort();
    out.default = index.default.filter(|d| out.recipes.iter().any(|r| &r.name == d));
    out.order = index.order;
    out
}

// ------------------------------------------------------------ 四类失配

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// ① 多出：配方里的名字在图参数里不存在。
    Extra,
    /// ② 类型不符。
    Type,
    /// ③ 越界 / 不在 options 里。
    Range,
    /// ④ 规格变了（整个配方的事，只提示）。
    Spec,
}

impl Kind {
    pub fn code(self) -> &'static str {
        match self {
            Kind::Extra => "extra",
            Kind::Type => "type",
            Kind::Range => "range",
            Kind::Spec => "spec",
        }
    }
    /// 与编辑器的 KIND_LABEL 同一套用语。
    pub fn label(self) -> &'static str {
        match self {
            Kind::Extra => "多出",
            Kind::Type => "类型不符",
            Kind::Range => "越界",
            Kind::Spec => "规格变了",
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Fix {
    Delete,
    Set(Value),
    /// ④ 的建议：把文件里记的图 id 与摘要换成当前图的。
    Rebase,
}

impl Fix {
    pub fn to_json(&self) -> Value {
        match self {
            Fix::Delete => json!({ "action": "delete" }),
            Fix::Set(v) => json!({ "action": "set", "value": v }),
            Fix::Rebase => json!({ "action": "rebase" }),
        }
    }
}

#[derive(Clone, Debug)]
pub struct Mismatch {
    pub kind: Kind,
    /// ①–③ 是图参数名；④ 是 None。
    pub param: Option<String>,
    pub message: String,
    pub fix: Fix,
    pub fix_label: String,
}

impl Mismatch {
    pub fn to_json(&self) -> Value {
        json!({
            "kind": self.kind.code(),
            "param": self.param,
            "message": self.message,
            "fix": self.fix.to_json(),
            "fixLabel": self.fix_label,
        })
    }

    /// stderr 上的一行：`[越界] cutMax：不能大于 5 → 夹到限位：5`。
    pub fn describe(&self) -> String {
        match &self.param {
            Some(p) => format!("[{}] {p}：{} → {}", self.kind.label(), self.message, self.fix_label),
            None => format!("[{}] {} → {}", self.kind.label(), self.message, self.fix_label),
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Report {
    pub items: Vec<Mismatch>,
    /// ①–③ 的条数：不为 0 时这个配方不能运行（P3.7）。④ 只提示。
    pub blocking: usize,
}

impl Report {
    pub fn spec(&self) -> Option<&Mismatch> {
        self.items.iter().find(|m| m.kind == Kind::Spec)
    }
}

struct Check {
    kind: Kind,
    message: String,
    fix: Fix,
    fix_label: String,
}

/// 失配报告（P3.7）。顺序：④ 在前，然后按参数名的码点序；每个参数至多一条，取第一个不满足的：
/// ① → ② → ③。没有 type 的老格式图参数只查 ①（core 也不按它校验，P1.2）。
pub fn recipe_report(
    doc_id: &str,
    params: &Map<String, Value>,
    values: &Map<String, Value>,
    graph: Option<&GraphRef>,
) -> Report {
    let mut items = Vec::new();
    let current = spec_digest(params);
    match graph {
        None => items.push(Mismatch {
            kind: Kind::Spec,
            param: None,
            message: "文件里没有记录图的 id 与规格摘要，不知道它是对着哪张图写的".to_string(),
            fix: Fix::Rebase,
            fix_label: "记成当前图".to_string(),
        }),
        Some(g) if g.id != doc_id || g.spec_digest != current => {
            let mut parts = Vec::new();
            if g.id != doc_id {
                parts.push(format!("图 id 不同（配方记的是 {}）", g.id));
            }
            if g.spec_digest != current {
                parts.push("图参数的规格（名字、类型、限位、options）与写配方时不同".to_string());
            }
            items.push(Mismatch {
                kind: Kind::Spec,
                param: None,
                message: format!("{}。不阻止运行，值照常逐个检查", parts.join("；")),
                fix: Fix::Rebase,
                fix_label: "按当前图更新记录".to_string(),
            });
        }
        Some(_) => {}
    }
    let mut names: Vec<&String> = values.keys().collect();
    names.sort();
    let mut blocking = 0;
    for name in names {
        let value = &values[name.as_str()];
        let check = match params.get(name.as_str()).filter(|gp| !gp.is_null()) {
            None => Some(Check {
                kind: Kind::Extra,
                message: format!("图里没有图参数 {name}（改名或删掉了？）"),
                fix: Fix::Delete,
                fix_label: "删除这个值".to_string(),
            }),
            Some(gp) => check_value(gp, value),
        };
        if let Some(c) = check {
            blocking += 1;
            items.push(Mismatch {
                kind: c.kind,
                param: Some(name.clone()),
                message: c.message,
                fix: c.fix,
                fix_label: c.fix_label,
            });
        }
    }
    Report { items, blocking }
}

pub fn report_of(doc: &GraphDoc, recipe: &RecipeFile) -> Report {
    recipe_report(&doc.id, &doc.params, &recipe.values, recipe.graph.as_ref())
}

fn clamp_num(x: f64, min: Option<f64>, max: Option<f64>, integer: bool) -> f64 {
    let lo = if integer { min.map(f64::ceil) } else { min };
    let hi = if integer { max.map(f64::floor) } else { max };
    if let Some(lo) = lo {
        if x < lo {
            return lo;
        }
    }
    if let Some(hi) = hi {
        if x > hi {
            return hi;
        }
    }
    x
}

/// 数（或每个分量）越过 min / max 的第一处；都在界内返回 None。文案与 core 的 checkRange 一致。
fn range_problem(xs: &[f64], min: Option<f64>, max: Option<f64>) -> Option<String> {
    for (i, x) in xs.iter().enumerate() {
        let at = if xs.len() > 1 { format!("第 {} 个分量", i + 1) } else { String::new() };
        if let Some(lo) = min {
            if *x < lo {
                return Some(format!("{at}不能小于 {}", js_number(lo)));
            }
        }
        if let Some(hi) = max {
            if *x > hi {
                return Some(format!("{at}不能大于 {}", js_number(hi)));
            }
        }
    }
    None
}

fn type_check(message: String, converted: Option<Value>) -> Option<Check> {
    Some(match converted {
        None => Check {
            kind: Kind::Type,
            message,
            fix: Fix::Delete,
            fix_label: "删除这个值（用基础）".to_string(),
        },
        Some(v) => Check {
            kind: Kind::Type,
            message,
            fix_label: format!("改为 {}", short(&v)),
            fix: Fix::Set(v),
        },
    })
}

fn range_check(message: String, clamped: Value) -> Option<Check> {
    Some(Check {
        kind: Kind::Range,
        message,
        fix_label: format!("夹到限位：{}", short(&clamped)),
        fix: Fix::Set(clamped),
    })
}

fn all_numbers(a: &[Value]) -> Option<Vec<f64>> {
    a.iter().map(|x| if x.is_number() { num(x) } else { None }).collect()
}

/// 同 core 的 checkCurveValue（编辑器 lib/curve.ts 的 curveProblem）：合法返回 None，否则一句原因。
fn curve_problem(value: &Value, min: Option<f64>, max: Option<f64>) -> Option<String> {
    let Some(o) = value.as_object() else {
        return Some("应当是 {\"points\": [[x, y], …], \"interp\": …} 这样的对象".to_string());
    };
    for k in o.keys() {
        if k != "points" && k != "interp" {
            return Some(format!("不认识的字段 '{k}'（只有 points 与 interp）"));
        }
    }
    if let Some(interp) = o.get("interp") {
        if interp != "linear" && interp != "smooth" {
            return Some("interp 只能是 \"linear\" 或 \"smooth\"".to_string());
        }
    }
    let Some(points) = o.get("points").and_then(Value::as_array).filter(|p| p.len() >= 2) else {
        return Some("points 至少要有两个控制点".to_string());
    };
    let mut prev = 0.0;
    for (i, pt) in points.iter().enumerate() {
        let at = format!("第 {} 个控制点", i + 1);
        let xy = pt
            .as_array()
            .filter(|p| p.len() == 2 && p[0].is_number() && p[1].is_number())
            .and_then(|p| Some((num(&p[0])?, num(&p[1])?)));
        let Some((x, y)) = xy else {
            return Some(format!("{at}应当是 [x, y] 两个数"));
        };
        if !(0.0..=1.0).contains(&x) {
            return Some(format!("{at}的 x 必须在 [0, 1] 内，实际是 {}", js_number(x)));
        }
        if i > 0 && !(x > prev) {
            return Some(format!("{at}的 x 必须大于前一个点（{}）", js_number(prev)));
        }
        if let Some(lo) = min {
            if y < lo {
                return Some(format!("{at}的 y 不能小于 {}", js_number(lo)));
            }
        }
        if let Some(hi) = max {
            if y > hi {
                return Some(format!("{at}的 y 不能大于 {}", js_number(hi)));
            }
        }
        prev = x;
    }
    None
}

/// `#rrggbb` / `#rrggbbaa` → [r, g, b(, a)]，各分量 = 字节 / 255。
fn hex_color(s: &str) -> Option<Vec<f64>> {
    let t = js_trim(s);
    let hex = t.strip_prefix('#')?;
    if !(hex.len() == 6 || hex.len() == 8) || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    (0..hex.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).ok().map(|b| f64::from(b) / 255.0))
        .collect()
}

/// 一个值对着图参数规格查 ②（类型不符）与 ③（越界 / 不在 options 里），并给出修复建议。
/// 判据与 core 的 coerceParam + checkRange 同一套（P1.2），建议与编辑器的 checkValue 逐条相同：
/// 能转换就转换（再夹进限位），不能就删；越界夹到限位；不在 options 里改为默认。
fn check_value(gp: &Value, v: &Value) -> Option<Check> {
    let min = gp.get("min").filter(|m| m.is_number()).and_then(num);
    let max = gp.get("max").filter(|m| m.is_number()).and_then(num);
    let ty = gp.get("type").and_then(Value::as_str)?;
    match ty {
        "bool" => {
            if v.is_boolean() {
                return None;
            }
            let b = match v {
                Value::Number(_) if num(v) == Some(0.0) || num(v) == Some(1.0) => Some(num(v) == Some(1.0)),
                Value::String(s) => {
                    let t = js_trim(s).to_ascii_lowercase();
                    (t == "true" || t == "false").then(|| t == "true")
                }
                _ => None,
            };
            type_check(format!("应当是 true / false，实际是 {}", short(v)), b.map(Value::Bool))
        }
        "int" | "flags" => {
            if let Some(x) = v.is_number().then(|| num(v)).flatten().filter(|x| x.fract() == 0.0) {
                return range_problem(&[x], min, max)
                    .and_then(|p| range_check(p, num_value(clamp_num(x, min, max, true))));
            }
            let n = if v.is_boolean() { None } else { as_number(v) };
            let what = if ty == "flags" { "整数位掩码" } else { "整数" };
            type_check(
                format!("应当是{what}，实际是 {}", short(v)),
                n.map(|n| num_value(clamp_num(round_half_away(n), min, max, true))),
            )
        }
        "float" => {
            if let Some(x) = v.is_number().then(|| num(v)).flatten() {
                return range_problem(&[x], min, max)
                    .and_then(|p| range_check(p, num_value(clamp_num(x, min, max, false))));
            }
            let n = if v.is_boolean() { None } else { as_number(v) };
            type_check(
                format!("应当是数字，实际是 {}", short(v)),
                n.map(|n| num_value(clamp_num(n, min, max, false))),
            )
        }
        "vec2f" | "vec3f" | "vec4f" | "transform" | "color" => {
            let color = ty == "color";
            let n = match ty {
                "vec2f" => 2,
                "vec3f" | "color" => 3,
                "vec4f" => 4,
                _ => 16,
            };
            let ok_len = |len: usize| len == n || (color && len == 4);
            let clamp_all = |xs: &[f64]| Value::Array(xs.iter().map(|x| num_value(clamp_num(*x, min, max, false))).collect());
            if let Some(a) = v.as_array() {
                if let Some(xs) = all_numbers(a).filter(|xs| ok_len(xs.len())) {
                    return range_problem(&xs, min, max).and_then(|p| range_check(p, clamp_all(&xs)));
                }
            }
            let keep = if color { 4 } else { n };
            let conv: Option<Vec<f64>> = if v.is_number() && ty != "transform" {
                num(v).map(|x| vec![x; n])
            } else if let Some(xs) = v.as_array().and_then(|a| all_numbers(a)).filter(|xs| xs.len() > keep) {
                Some(xs[..keep].to_vec())
            } else if let (true, Some(s)) = (color, v.as_str()) {
                hex_color(s)
            } else {
                None
            };
            let want = if color { "3 或 4 个数（RGB / RGBA）".to_string() } else { format!("{n} 个数") };
            type_check(format!("应当是 {want}的数组，实际是 {}", short(v)), conv.map(|xs| clamp_all(&xs)))
        }
        "enum" => {
            let options: Vec<Option<&Value>> = gp
                .get("options")
                .and_then(Value::as_array)
                .map(|a| a.iter().map(|o| o.get("value")).collect())
                .unwrap_or_default();
            let includes = |s: &str| options.iter().any(|o| o.and_then(Value::as_str) == Some(s));
            if let Some(s) = v.as_str() {
                if options.is_empty() || includes(s) {
                    return None;
                }
                let listed: Vec<String> = options
                    .iter()
                    .map(|o| o.filter(|x| !x.is_null()).map(js_string).unwrap_or_default())
                    .collect();
                return Some(Check {
                    kind: Kind::Range,
                    message: format!("'{s}' 不在选项里（{}）", listed.join(" / ")),
                    fix: Fix::Delete,
                    fix_label: "改为默认（删除这条覆盖）".to_string(),
                });
            }
            let s = (v.is_number() || v.is_boolean()).then(|| js_string(v));
            type_check(
                format!("应当是选项里的字符串，实际是 {}", short(v)),
                s.filter(|s| options.is_empty() || includes(s)).map(Value::String),
            )
        }
        "string" | "text" | "path" => {
            if v.is_string() {
                return None;
            }
            let s = (v.is_number() || v.is_boolean()).then(|| js_string(v));
            type_check(format!("应当是字符串，实际是 {}", short(v)), s.map(Value::String))
        }
        "curve" => {
            if let Some(shape) = curve_problem(v, None, None) {
                return type_check(format!("曲线的格式不对：{shape}"), None);
            }
            let p = curve_problem(v, min, max)?;
            let points: Vec<Value> = v["points"]
                .as_array()
                .map(|pts| {
                    pts.iter()
                        .map(|pt| {
                            let x = num(&pt[0]).unwrap_or(0.0);
                            let y = num(&pt[1]).unwrap_or(0.0);
                            json!([num_value(x), num_value(clamp_num(y, min, max, false))])
                        })
                        .collect()
                })
                .unwrap_or_default();
            let mut clamped = Map::new();
            clamped.insert("points".to_string(), Value::Array(points));
            if let Some(interp) = v.get("interp") {
                clamped.insert("interp".to_string(), interp.clone());
            }
            range_check(p, Value::Object(clamped))
        }
        _ => None,
    }
}

// ------------------------------------------------------------ 取值

/// 运行时交给 core 的 `{名字: 值}`：图里声明着的每个图参数取「配方的值，没有就 default」（K4）。
/// 配方里多出来的名字不进来（那是失配 ①）。宿主拼 `params_json` 就是这一步。
pub fn effective_params(params: &Map<String, Value>, values: &Map<String, Value>) -> Map<String, Value> {
    params
        .iter()
        .map(|(name, gp)| {
            let v = values.get(name).cloned().unwrap_or_else(|| gp.get("default").cloned().unwrap_or(Value::Null));
            (name.clone(), v)
        })
        .collect()
}

/// 把配方里的值写成对应图参数的 default（只写图里声明着的名字）。返回真正改了的那些名字。
pub fn write_as_defaults(doc: &mut GraphDoc, values: &Map<String, Value>) -> Vec<String> {
    let mut changed = Vec::new();
    for (name, value) in values {
        let Some(decl) = doc.params.get_mut(name).and_then(Value::as_object_mut) else {
            continue;
        };
        if decl.get("default") == Some(value) {
            continue;
        }
        decl.insert("default".to_string(), value.clone());
        changed.push(name.clone());
    }
    changed
}

/// ①–③ 拦下时的整段文字。第一行带 `recipe_mismatch:`，load_exit 靠它给退出码 4。
pub fn blocking_text(recipe: &RecipeFile, report: &Report) -> String {
    let mut lines = vec![format!(
        "recipe_mismatch: 配方「{}」有 {} 处失配，不能运行（{}）：",
        recipe.name,
        report.blocking,
        recipe.path.display()
    )];
    for m in &report.items {
        lines.push(format!("  {}", m.describe()));
    }
    lines.push(
        "  有失配时这个配方不能运行；其余配方不受影响。到编辑器的「配方管理」按建议修复，或者改配方文件".to_string(),
    );
    lines.join("\n")
}

/// `--recipe <文件>`（run / validate / plan / params / eval，以及同样经 load_graph 的 dump / sweep /
/// perturb）：读配方、对着这张图查失配，①–③ 返回 Err（退出码 4，整段报告），④ 只在 stderr 提示；
/// 然后把配方里的值写成对应图参数的 default。之后 load_graph 才应用 `--param`，所以 `--param` 优先。
pub(crate) fn apply_recipe_option(doc: &mut GraphDoc, parsed: &Parsed, err: &Sink) -> Result<(), String> {
    let specs = parsed.many("recipe");
    let Some(file) = specs.last() else {
        return Ok(());
    };
    if specs.len() > 1 {
        return Err(format!("bad_recipe: --recipe 只能给一个（收到 {} 个）：配方之间不叠加（K4）", specs.len()));
    }
    let recipe = load_checked(doc, Path::new(file))?;
    let changed = write_as_defaults(doc, &recipe.values);
    line(
        err,
        &format!(
            "配方「{}」：{} 个值，{} 个与基础不同{}",
            recipe.name,
            recipe.values.len(),
            changed.len(),
            if changed.is_empty() { String::new() } else { format!("（{}）", changed.join("、")) }
        ),
    );
    if let Some(spec) = report_of(doc, &recipe).spec() {
        line(err, &spec_hint(&recipe, spec));
    }
    Ok(())
}

/// 读配方并查 ①–③。读不出来是 `bad_recipe:`，有 ①–③ 是 `recipe_mismatch:`（两者都是退出码 4）。
pub(crate) fn load_checked(doc: &GraphDoc, file: &Path) -> Result<RecipeFile, String> {
    let recipe = read_recipe_file(file).map_err(|e| format!("bad_recipe: 读不出配方文件 {}：{e}", file.display()))?;
    let report = report_of(doc, &recipe);
    if report.blocking > 0 {
        return Err(blocking_text(&recipe, &report));
    }
    Ok(recipe)
}

/// ④ 的提示（不改退出码）。
pub(crate) fn spec_hint(recipe: &RecipeFile, spec: &Mismatch) -> String {
    format!(
        "提示：配方「{}」[{}] {}（编辑器「配方管理」里可以{}）",
        recipe.name,
        spec.kind.label(),
        spec.message,
        spec.fix_label
    )
}

// ------------------------------------------------------------ lyflow recipes

fn recipe_json(doc: &GraphDoc, r: &RecipeFile, default: Option<&str>, with_params: bool) -> Value {
    let report = report_of(doc, r);
    let mut row = json!({
        "kind": "recipe",
        "name": r.name,
        "file": r.path.to_string_lossy(),
        "default": default == Some(r.name.as_str()),
        "values": r.values.len(),
        "blocking": report.blocking,
        "items": report.items.iter().map(Mismatch::to_json).collect::<Vec<_>>(),
    });
    let obj = row.as_object_mut().expect("对象");
    if let Some(note) = &r.note {
        obj.insert("note".to_string(), json!(note));
    }
    if let Some(at) = &r.updated_at {
        obj.insert("updatedAt".to_string(), json!(at));
    }
    if !r.notes.is_empty() {
        obj.insert("notes".to_string(), json!(r.notes));
    }
    if with_params {
        obj.insert("params".to_string(), Value::Object(effective_params(&doc.params, &r.values)));
    }
    row
}

/// `lyflow recipes <graph> [--recipe <文件>]... [--json]`：图旁配方目录里的配方（或只看给的那几个文件）、
/// 值个数、四类失配与建议、默认配方。只读；失配是报告的内容，不是这条命令的失败（退出码 0）。
/// 给了 --recipe 时每个配方另带 `params`（合成好的图参数取值，MCP 的 run_graph 拿它交给后端）。
pub(crate) fn cmd_recipes(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow recipes <graph> [--recipe <文件>]... [--json]");
        return EXIT_USAGE;
    };
    let text = match std::fs::read_to_string(&path) {
        Ok(t) => t,
        Err(e) => return fail(err, &format!("读取 {path} 失败: {e}"), EXIT_INVALID),
    };
    let doc: GraphDoc = match serde_json::from_str(&text) {
        Ok(d) => d,
        Err(e) => return fail(err, &format!("{path} 不是合法的 GraphDoc: {e}"), EXIT_INVALID),
    };
    let dir = recipe_dir_of(&std::path::absolute(&path).unwrap_or_else(|_| PathBuf::from(&path)));
    let listed = list_recipe_dir(&dir);
    let files = parsed.many("recipe");
    let chosen: Vec<RecipeFile> = if files.is_empty() {
        listed.recipes.clone()
    } else {
        let mut v = Vec::new();
        for f in files {
            match read_recipe_file(Path::new(f)) {
                Ok(r) => v.push(r),
                Err(e) => return fail(err, &format!("bad_recipe: 读不出配方文件 {f}：{e}"), EXIT_USAGE),
            }
        }
        v
    };
    let default = listed.default.as_deref();
    let rows: Vec<Value> = chosen.iter().map(|r| recipe_json(&doc, r, default, !files.is_empty())).collect();
    let summary = json!({
        "kind": "recipe_dir",
        "graph": path,
        "graphId": doc.id,
        "specDigest": spec_digest(&doc.params),
        "dir": dir.to_string_lossy(),
        "exists": listed.exists,
        "default": listed.default,
        "order": listed.order,
        "count": listed.recipes.len(),
        "problems": listed.problems.iter().map(|(f, m)| json!({ "file": f, "message": m })).collect::<Vec<_>>(),
    });
    if parsed.has("json") {
        for row in &rows {
            json_line(out, row);
        }
        json_line(out, &summary);
    } else {
        for row in &rows {
            let star = if row["default"] == true { " ★默认" } else { "" };
            let blocking = row["blocking"].as_u64().unwrap_or(0);
            let state = if blocking > 0 { format!("{blocking} 处失配，不能运行") } else { "可以运行".to_string() };
            line(
                out,
                &format!("{}{star}  {} 个值  {state}", row["name"].as_str().unwrap_or_default(), row["values"]),
            );
            for item in row["items"].as_array().into_iter().flatten() {
                let label = Kind::from_code(item["kind"].as_str().unwrap_or_default()).map(Kind::label).unwrap_or("?");
                let param = item["param"].as_str().map(|p| format!("{p}：")).unwrap_or_default();
                line(
                    out,
                    &format!(
                        "    [{label}] {param}{} → {}",
                        item["message"].as_str().unwrap_or_default(),
                        item["fixLabel"].as_str().unwrap_or_default()
                    ),
                );
            }
        }
    }
    for (file, message) in &listed.problems {
        line(err, &format!("读不出来的文件 {file}：{message}"));
    }
    line(
        err,
        &format!(
            "{} 个配方（{}）{}",
            rows.len(),
            if listed.exists { dir.display().to_string() } else { format!("{} 不存在", dir.display()) },
            listed.default.as_ref().map(|d| format!("，默认「{d}」")).unwrap_or_default()
        ),
    );
    EXIT_OK
}

impl Kind {
    fn from_code(code: &str) -> Option<Kind> {
        match code {
            "extra" => Some(Kind::Extra),
            "type" => Some(Kind::Type),
            "range" => Some(Kind::Range),
            "spec" => Some(Kind::Spec),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixtures() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../schema/fixtures/recipes")
    }

    fn read_json(path: &Path) -> Value {
        serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
    }

    fn fixture_doc() -> GraphDoc {
        serde_json::from_value(read_json(&fixtures().join("graph.lyflow.json"))).unwrap()
    }

    #[test]
    fn js_number_matches_ecmascript_number_to_string() {
        for (x, want) in [
            (0.0, "0"),
            (-0.0, "0"),
            (1.0, "1"),
            (5.0, "5"),
            (-2.5, "-2.5"),
            (0.1, "0.1"),
            (0.0001, "0.0001"),
            (0.00001, "0.00001"),
            (0.000001, "0.000001"),
            (1e-7, "1e-7"),
            (1.5e-7, "1.5e-7"),
            (20000000.0, "20000000"),
            (1e20, "100000000000000000000"),
            (1e21, "1e+21"),
            (1.2345e22, "1.2345e+22"),
            (123.456, "123.456"),
            (128.0 / 255.0, "0.5019607843137255"),
            (0.1 + 0.2, "0.30000000000000004"),
            (f64::MAX, "1.7976931348623157e+308"),
            (5e-324, "5e-324"),
        ] {
            assert_eq!(js_number(x), want, "{x:e}");
        }
    }

    #[test]
    fn js_json_puts_index_keys_first_like_json_stringify() {
        let v: Value = serde_json::from_str(r#"{"b":1,"10":2,"a":[1.0,"é\u0001"],"2":null,"01":3}"#).unwrap();
        assert_eq!(js_json(&v), r#"{"2":null,"10":2,"b":1,"a":[1,"é\u0001"],"01":3}"#);
    }

    /// 与编辑器同一个数：expected.json 里的规范形与摘要是编辑器单测断言过的。
    #[test]
    fn spec_digest_matches_the_shared_fixture_byte_for_byte() {
        let expected = read_json(&fixtures().join("expected.json"));
        let doc = fixture_doc();
        assert_eq!(spec_canonical(&doc.params), expected["specCanonical"].as_str().unwrap());
        assert_eq!(spec_digest(&doc.params), expected["specDigest"].as_str().unwrap());
        // 没有图参数也有确定的摘要（空数组的 SHA-256）
        assert_eq!(spec_canonical(&Map::new()), "[]");
    }

    #[test]
    fn spec_digest_ignores_cosmetics_and_orders_by_code_point() {
        let params = |v: Value| v.as_object().unwrap().clone();
        let a = params(json!({ "x": {"type": "float", "min": 0, "max": 1, "default": 0.5, "binds": ["n.a"]},
                              "y": {"type": "int", "default": 1, "binds": ["n.b"]} }));
        let b = params(json!({ "y": {"type": "int", "default": 9, "binds": ["n.b"], "advanced": true},
                              "x": {"type": "float", "min": 0, "max": 1, "label": "别的", "unit": "mm",
                                    "softMin": 0.2, "softMax": 0.8, "step": 0.1, "group": "g", "doc": "…",
                                    "default": 0.7, "binds": ["m.b"]} }));
        assert_eq!(spec_digest(&a), spec_digest(&b));
        let e1 = params(json!({ "m": {"type": "enum", "options": [{"value": "a", "label": "A"}, {"value": "b"}], "binds": []} }));
        let e2 = params(json!({ "m": {"type": "enum", "options": [{"value": "b", "label": "乙"}, {"value": "a"}], "binds": []} }));
        assert_eq!(spec_digest(&e1), spec_digest(&e2));
        // 老格式：只有名字算数
        assert_eq!(
            spec_digest(&params(json!({ "x": {"default": 1, "max": 5} }))),
            spec_digest(&params(json!({ "x": {"default": 2} })))
        );
        // BMP 外的字符按码点排在 U+FF5E 后面（UTF-16 码元序正好相反）；数用 ECMAScript 的格式
        let c = spec_canonical(&params(json!({ "\u{1F600}": {"type": "float"}, "\u{FF5E}": {"type": "float", "min": 0.1, "max": 1e21} })));
        assert!(c.starts_with("[[\"\u{FF5E}\",\"float\",0.1,1e+21,null]"), "{c}");
    }

    /// P4.1 的一致性断言：夹具目录里每个配方文件的报告与 expected.json 逐条相同 ——
    /// 类别、参数、建议（fix，按 JSON 文本比）、文案（message、fixLabel），以及 blocking 的条数。
    #[test]
    fn reports_match_the_shared_fixture_item_for_item() {
        let expected = read_json(&fixtures().join("expected.json"));
        let doc = fixture_doc();
        let listed = list_recipe_dir(&fixtures().join("graph.recipes"));
        assert!(listed.problems.is_empty(), "{:?}", listed.problems);
        let mut files: Vec<String> = listed
            .recipes
            .iter()
            .map(|r| r.path.file_name().unwrap().to_string_lossy().into_owned())
            .collect();
        files.sort();
        let mut want_files: Vec<String> = expected["recipes"].as_object().unwrap().keys().cloned().collect();
        want_files.sort();
        assert_eq!(files, want_files, "夹具目录与 expected.json 列的文件对不上");
        for r in &listed.recipes {
            let file = r.path.file_name().unwrap().to_string_lossy().into_owned();
            let want = &expected["recipes"][file.as_str()];
            let report = report_of(&doc, r);
            assert_eq!(report.blocking as u64, want["blocking"].as_u64().unwrap(), "{file} 的 blocking");
            let got: Vec<Value> = report.items.iter().map(Mismatch::to_json).collect();
            let want_items = want["items"].as_array().unwrap();
            assert_eq!(got.len(), want_items.len(), "{file}：{}", Value::Array(got.clone()));
            for (g, w) in got.iter().zip(want_items) {
                for key in ["kind", "param", "message", "fixLabel"] {
                    assert_eq!(g[key], w[key], "{file} {} 的 {key}", w["param"]);
                }
                assert_eq!(js_json(&g["fix"]), js_json(&w["fix"]), "{file} {} 的 fix", w["param"]);
            }
        }
        // index.json：默认是 ok，顺序照 order
        assert_eq!(listed.default.as_deref(), Some("ok"));
        let names: Vec<&str> = listed.recipes.iter().map(|r| r.name.as_str()).collect();
        assert_eq!(names, ["ok", "extra", "type", "range", "spec", "nograph"]);
    }

    /// 按建议修完，①–③ 清零（② 的转换已经夹进限位，所以修一次就干净），报告里没有的值一个不动。
    #[test]
    fn applying_every_suggestion_leaves_no_blocking_item() {
        let doc = fixture_doc();
        for r in list_recipe_dir(&fixtures().join("graph.recipes")).recipes {
            let before = report_of(&doc, &r);
            let mut values = r.values.clone();
            for m in &before.items {
                let Some(p) = &m.param else { continue };
                match &m.fix {
                    Fix::Delete => {
                        values.remove(p);
                    }
                    Fix::Set(v) => {
                        values.insert(p.clone(), v.clone());
                    }
                    Fix::Rebase => {}
                }
            }
            let graph = GraphRef { id: doc.id.clone(), spec_digest: spec_digest(&doc.params) };
            let after = recipe_report(&doc.id, &doc.params, &values, Some(&graph));
            assert!(after.items.is_empty(), "{}：{:?}", r.name, after.items);
            let touched: Vec<&str> = before.items.iter().filter_map(|m| m.param.as_deref()).collect();
            for (k, v) in &r.values {
                if !touched.contains(&k.as_str()) {
                    assert_eq!(values.get(k), Some(v), "{}：{k} 不在报告里却被改了", r.name);
                }
            }
        }
    }

    #[test]
    fn names_that_look_like_object_builtins_are_still_extra() {
        let doc = fixture_doc();
        let values = json!({ "constructor": 1, "toString": 2, "__proto__": 3 }).as_object().unwrap().clone();
        let r = recipe_report(&doc.id, &doc.params, &values, None);
        let kinds: Vec<(&str, Option<&str>)> = r.items.iter().map(|m| (m.kind.code(), m.param.as_deref())).collect();
        assert_eq!(
            kinds,
            [("spec", None), ("extra", Some("__proto__")), ("extra", Some("constructor")), ("extra", Some("toString"))]
        );
    }

    #[test]
    fn recipe_dir_follows_the_graph_file_name() {
        let base = Path::new("D:/work");
        assert_eq!(recipe_dir_of(&base.join("车门缝隙.lyflow.json")), base.join("车门缝隙.recipes"));
        assert_eq!(recipe_dir_of(&base.join("demo.LYFLOW.JSON")), base.join("demo.recipes"));
        assert_eq!(recipe_dir_of(&base.join("demo.json")), base.join("demo.recipes"));
        assert_eq!(recipe_dir_of(&base.join(".hidden")), base.join(".hidden.recipes"));
        assert_eq!(recipe_stem("A.lyflow-recipe.json"), Some("A"));
        assert_eq!(recipe_stem("车型A.LYFLOW-RECIPE.JSON"), Some("车型A"));
        assert_eq!(recipe_stem(".lyflow-recipe.json"), None);
        assert_eq!(recipe_stem("index.json"), None);
    }

    #[test]
    fn lenient_reading_and_the_hard_errors() {
        let p = Path::new("x.lyflow-recipe.json");
        let r = parse_recipe_text(r#"{"values": {"a": 1}, "extraField": true}"#, "x", p).unwrap();
        assert_eq!(r.name, "x");
        assert!(r.graph.is_none());
        assert_eq!(r.notes.len(), 3, "{:?}", r.notes);
        assert!(parse_recipe_text("[]", "x", p).unwrap_err().contains("对象"));
        assert!(parse_recipe_text("{", "x", p).unwrap_err().contains("JSON"));
        assert!(parse_recipe_text(r#"{"schemaVersion": 2}"#, "x", p).unwrap_err().contains("新"));
    }

    #[test]
    fn effective_params_are_default_under_the_recipe() {
        let doc = fixture_doc();
        let values = json!({ "cutMax": 2.5, "voxelSize": 1 }).as_object().unwrap().clone();
        let p = effective_params(&doc.params, &values);
        assert_eq!(p.len(), doc.params.len());
        assert_eq!(p["cutMax"], json!(2.5));
        assert_eq!(p["pointCount"], json!(20000));
        assert!(!p.contains_key("voxelSize"));
    }
}
