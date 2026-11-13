use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::{Path, PathBuf};

fn default_timeout() -> u64 {
    60_000
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(default, rename_all = "camelCase")]
pub struct PythonRuntimeConfig {
    pub executable: String,
    pub package_paths: Vec<String>,
    pub timeout_ms: u64,
}

impl Default for PythonRuntimeConfig {
    fn default() -> Self {
        Self {
            executable: String::new(),
            package_paths: Vec::new(),
            timeout_ms: default_timeout(),
        }
    }
}

impl PythonRuntimeConfig {
    pub fn normalized(mut self) -> Result<Self, String> {
        self.executable = self.executable.trim().to_owned();
        if self.timeout_ms == 0 || self.timeout_ms > 86_400_000 {
            return Err("Python 超时必须在 1 到 86400000 毫秒之间".into());
        }
        let mut paths: Vec<String> = Vec::new();
        for path in self.package_paths {
            let path = path.trim().to_owned();
            if path.is_empty() {
                continue;
            }
            let key = |value: &str| {
                let value = value.replace('\\', "/");
                if cfg!(windows) {
                    value.to_lowercase()
                } else {
                    value
                }
            };
            if !paths.iter().any(|existing| key(existing) == key(&path)) {
                paths.push(path);
            }
        }
        self.package_paths = paths;
        Ok(self)
    }

    pub fn core_json(&self) -> Value {
        let mut value = serde_json::to_value(self).expect("Python config 可序列化");
        let sdk = std::env::var_os("LYFLOW_PYTHON_SDK_DIR")
            .map(PathBuf::from)
            .or_else(|| {
                if cfg!(debug_assertions) {
                    let source = Path::new(env!("CARGO_MANIFEST_DIR"))
                        .parent()?
                        .join("python");
                    source
                        .join("lyflow_python/worker.py")
                        .is_file()
                        .then_some(source)
                } else {
                    None
                }
            });
        if let Some(path) = sdk {
            value["sdkDir"] = Value::String(path.to_string_lossy().into_owned());
        }
        value
    }
}

pub fn settings_file() -> PathBuf {
    if let Some(path) = std::env::var_os("LYFLOW_PYTHON_CONFIG") {
        return PathBuf::from(path);
    }
    let base = std::env::var_os("APPDATA")
        .or_else(|| std::env::var_os("HOME"))
        .map(PathBuf::from)
        .unwrap_or_default();
    base.join("com.lyflow.app").join("python-nodes.json")
}

pub fn read_from(file: &Path) -> Result<PythonRuntimeConfig, String> {
    match std::fs::read_to_string(file) {
        Ok(text) => serde_json::from_str::<PythonRuntimeConfig>(&text)
            .map_err(|e| format!("Python 配置 {} 不是合法配置: {e}", file.display()))?
            .normalized(),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            Ok(PythonRuntimeConfig::default())
        }
        Err(error) => Err(format!("读取 Python 配置 {} 失败: {error}", file.display())),
    }
}

pub fn read() -> Result<PythonRuntimeConfig, String> {
    read_from(&settings_file())
}

pub fn write_to(file: &Path, config: PythonRuntimeConfig) -> Result<PythonRuntimeConfig, String> {
    let config = config.normalized()?;
    if let Some(parent) = file.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent).map_err(|e| format!("创建 Python 配置目录失败: {e}"))?;
    }
    let temporary = file.with_extension(format!("{}.tmp", std::process::id()));
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())? + "\n";
    std::fs::write(&temporary, text).map_err(|e| format!("写入 Python 配置失败: {e}"))?;
    if let Err(error) = std::fs::rename(&temporary, file) {
        let _ = std::fs::remove_file(&temporary);
        return Err(format!("替换 Python 配置失败: {error}"));
    }
    Ok(config)
}

pub fn write(config: PythonRuntimeConfig) -> Result<PythonRuntimeConfig, String> {
    write_to(&settings_file(), config)
}

pub fn apply(config: &PythonRuntimeConfig) -> Result<Value, String> {
    let core = crate::core_ffi::core()?;
    if !core.supports_python() {
        return Ok(unavailable_status(config));
    }
    let raw = core
        .configure_python(&config.core_json().to_string())
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("Python 状态无效: {e}"))
}

pub fn unavailable_status(config: &PythonRuntimeConfig) -> Value {
    serde_json::json!({ "configured": false, "ready": false, "operatorCount": 0, "problems": [],
        "executable": config.executable, "packagePaths": config.package_paths, "timeoutMs": config.timeout_ms })
}

pub fn status() -> Result<Value, String> {
    let core = crate::core_ffi::core()?;
    if !core.supports_python() {
        return Ok(unavailable_status(&PythonRuntimeConfig::default()));
    }
    serde_json::from_str(&core.python_status().map_err(|e| e.to_string())?)
        .map_err(|e| format!("Python 状态无效: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn configuration_round_trip_validation_and_corruption() {
        let root =
            std::env::temp_dir().join(format!("lyflow-python-config-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let file = root.join("配置.json");
        let _ = std::fs::remove_file(&file);
        assert_eq!(read_from(&file).unwrap(), PythonRuntimeConfig::default());
        let config = PythonRuntimeConfig {
            executable: " C:/Python/python.exe ".into(),
            package_paths: vec![" C:/节点.py ".into(), "".into(), "C:/节点.py".into()],
            timeout_ms: 1200,
        };
        let saved = write_to(&file, config).unwrap();
        assert_eq!(saved.package_paths, vec!["C:/节点.py"]);
        assert_eq!(saved.executable, "C:/Python/python.exe");
        assert_eq!(read_from(&file).unwrap(), saved);
        let updated = write_to(
            &file,
            PythonRuntimeConfig {
                timeout_ms: 2400,
                ..saved.clone()
            },
        )
        .unwrap();
        assert_eq!(read_from(&file).unwrap(), updated);
        assert!(write_to(
            &file,
            PythonRuntimeConfig {
                timeout_ms: 0,
                ..saved.clone()
            }
        )
        .is_err());
        assert_eq!(read_from(&file).unwrap(), updated);
        assert!(write_to(
            &file,
            PythonRuntimeConfig {
                timeout_ms: 86_400_001,
                ..saved
            }
        )
        .is_err());
        assert_eq!(read_from(&file).unwrap(), updated);
        std::fs::write(&file, "{").unwrap();
        assert!(read_from(&file).is_err());
        let _ = std::fs::remove_file(&file);
        let _ = std::fs::remove_dir(&root);
    }
}
