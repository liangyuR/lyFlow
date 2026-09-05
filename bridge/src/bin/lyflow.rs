// headless CLI 的入口（ADR-0012）。真正的实现在 lyflow_lib::cli —— 放进 lib 里
// 才能被 cargo test 覆盖，而 bin 只负责把退出码交回给 shell。

fn main() {
    std::process::exit(lyflow_lib::cli::main());
}
