# 阶段 B：xyz-gap-inspector 切换到 LyFlow

> **M7 起的现状**：gap 包的正确性不再以「复现原算法的每一位输出」为标准（`docs/m7-plan.md` J1/J3），行为已有意偏离基线。下文以 39 样本 A/B、194 车回放「逐值一致」、导入器与 Python 生成器「逐节点等价」为门槛的条目不再成立；业务侧切换的验收口径要另定（M7 不涉及宿主仓库，J11）。

上游设计：[gap-inspector-integration-design.md](gap-inspector-integration-design.md) §3、§4；依赖阶段 A 的产物：
C ABI v7 与 `client.hpp`（[phase-a1-acceptance.md](phase-a1-acceptance.md)）、`@lyflow/editor` 与 HTTP 传输契约（[http-transport.md](http-transport.md)）。
工作在 `D:\project\xyz-gap-inspector` 的新分支 `lyflow-integration`（从 `release/2.1.0` 切出，`lyflow-ops` 已合入其中）。**不双轨**：同一分支上完成切换与删除，完成后以 PR 合入 `release/2.1.0`。用户已确认：标定 `calibrateWithCylinder` 保留在业务仓库。

## 1. 决定

| # | 决定 |
|---|---|
| B1 | 业务仓库通过 `find_package(lyflow CONFIG)` 依赖 LyFlow 的安装目录（`LYFLOW_ROOT`，由 `scripts/install-lyflow.ps1` 产出，包含 std-pointcloud、std-ml、gap 三个包）；不再依赖 PCL、Eigen、onnxruntime、yaml-cpp（yaml-cpp 仍被业务自己的 setting/camera 配置使用，保留） |
| B2 | 新增 `src/application/measurement/lyflow_measurer.{hpp,cpp}`：唯一的算法入口。输入 = 图文本 + baseDir + 两片云（内存注入）或 PCD 路径 + 参数 patch（可选）+ 选项；输出 = `MeasurementResult`（保留为业务 DTO），由 `lyflow_run_outputs` 的 `gap`/`flush`/`bundle` 装配。进程内一个 `lyflow::Client`，每设备一条计算线程各自 `run`，并发靠 A1-2 |
| B3 | 数据库 v5：`<point>/<point>.lyflow.json` + `templates/`，`<point>.yml` 去掉 `active_equipment`；`PrepareDatabase` 的 v4→v5 迁移用 `lyflow_import("StandardGap.yml", …)`（模式按 `setting.yml` 的 `model_roi.enabled` 选 `model` 生成带回退的图，否则 `template`），`StandardGap/` 改名 `templates/`，图里的模板路径相对图目录。迁移走 `RecoverableDirectoryTransaction`。`parameter_schema_version`/`parameter_template_sha256`/`parameter_migrations.yml` 机制删除；打开图时 `validate` 返回的算子迁移诊断由业务落盘并记日志 |
| B4 | 图文件缓存：`DoMeasurement` 按 mtime 缓存图文本与 sha256；`config_sha256` 语义改为图文件 sha256，CSV 列名不变；`metadata.json`/`results.csv` 增加 `pack_versions` 列 |
| B5 | 服务 API：新增 `/lyflow/*` 全部端点（按 http-transport.md，工作区根 = 数据库目录，`files/*` 的 path 只允许数据库内）；图的读写走 `GET|PUT /database/models/:model/devices/:device/points/:point/graph`；删除 `GET/PUT/PATCH …/configs/:name`、`POST /database/configs:batch-update`、`/config/schema`、`/config/roi-types`、`/database/parameter-upgrade/*`；模板端点路径改 `templates/`。WS 事件多一类 `lyflow.event`（就是 ExecutionEvent） |
| B6 | UI：`YmlPage` → `GraphPage`（`<LyFlowEditor transport={HttpTransport(service)}>` + 结论面板 + 模板工具栏）；交互计算页复用 `GraphPage`，历史点云以注入方式喂给图；删除 `ParamTree/*`、`GapCanvas/*`、`net/xgpc.ts`、`cloudBuffer.ts`、`stores/interactive.ts` 中的参数状态；React 版本与 `@lyflow/editor` peer 范围对齐 |
| B7 | 三处调用点统一换 `LyFlowMeasurer`：生产（`ComputeMeasurement`）、重算（`FreezePoint` 冻结图 + `templates/`）、交互计算（`parameters_override` 改为节点参数 patch）、异常图（几何从 `bundle` 取）。`render_spec` 的 `DebugGeometry` 依赖改为 bundle 字段 |
| B8 | 删除：`src/gap_core`（除 `MeasurementTypes` 中作为 DTO 保留的结果类型，搬到 `src/domain/measurement/`）、`src/gap_detection`、`src/gap_ml`、`src/gap_io`（`Sha256` 搬到 `src/infrastructure/`，`BatchManifest` 删除）、`src/gap_batch`、`param_config/StandardGap.yml`、`point_template.yml`、`parameter_migrations.yml`、`properties*.yml`、`roi_type.yml`、`PropertySchema`、`DetectionConfigurationAdapter`、`DetectionConfiguration.hpp`（`PointCloud` typedef 搬到业务头）；标定 `calibrateWithCylinder` 搬到 `src/application/calibration/`（它依赖 PCL 的圆柱拟合 → 用 LyFlow std 的 `fit.circle_2d`？**不**：它是设备标定，保留原实现并只为它链 PCL 的 common+sample_consensus 两个库） |
| B9 | 部署：`GapInstall.cmake` 把 `${LYFLOW_ROOT}/bin/` 整目录进 `backend/bin`，`library/` 进 `backend/library`；`stage-tauri.ps1` 校验清单更新；`vcpkg.json` 精简 |
| B10 | 工具链：`tools/` 里 14 个依赖 `results.csv`/`diagnostics.jsonl` 的脚本，凡是靠 `gap_batch_runner` 产出的，改为调用 `lyflow` CLI + `packs/gap/tools/lyflow_ab.py` 的输出（列名兼容）；`replay_baseline.py`/`check_replay_run.py` 不改 |

## 2. 顺序（每步业务仓库可编译、`ctest --preset windows-ci` 绿）

1. B1 + B2：引入 LyFlow 依赖与 `LyFlowMeasurer`，先只在**交互计算**路径接入（可观测、易调试）
2. B3 + B4：数据库 v5 迁移、图缓存；样例库 `config/car_config/**` 迁成图
3. B7：生产、重算、异常图切到 `LyFlowMeasurer`
4. B5 + B6：服务端点与 UI
5. B8 + B9 + B10：删除、部署、工具链
6. 文档：`docs/configuration_storage.md`、`capture_compute_decoupling.md`、`offline_batch.md`（改为 lyflow CLI）、`offline_field_replay.md`、`model_roi_port_contract.md`（指向 LyFlow）、`CHANGELOG.md`、`README.md`

## 3. 验收

- [ ] 交互计算 API 对 39 样本：与模型基线 39/39（模型路径图）、与模板基线 39/39（模板路径图）
- [ ] **194 台车离线回放**（`scripts/run_replay.ps1` + `tools/check_replay_run.py`）对 `baselines/full-194-KUN10-db0904.json` 逐值一致；随后 `replay_baseline.py freeze` 生成新基线，`frozen_at_commit` 指切换 commit
- [ ] 生产路径节拍：回放日志里每点计算时长分布不劣于切换前（同一台机器对比）
- [ ] 数据库 v4 样例库经 `PrepareDatabase` 迁到 v5，再次启动不再迁移；迁移失败可回滚
- [ ] UI：`GraphPage` 打开一个点位、改参数、运行、看剖面与 ROI 框、保存；交互计算页加载历史点云运行
- [ ] 打包：`stage-tauri.ps1` 产物在干净目录启动、`backend/bin` 含 LyFlow 全部 DLL
- [ ] 删除后全仓 `grep -r "gap_core\|gap_detection\|GapUtils\|parseDetectionConfiguration"` 为空（除 CHANGELOG）
- [ ] 业务仓库 CLAUDE.md/AGENTS.md 已有规则不违反

## 4. 风险与对策

- 切换 commit 巨大：按 §2 顺序分 commit，最终以一个 PR 合入；PR 描述附三份验收（A/B、回放、节拍）。
- LyFlow 版本漂移：业务仓库 `cmake/lyflow.cmake` 钉 LyFlow 的 commit 与 C ABI 版本（7），不匹配 FATAL。
- 现场配置目录（ProgramData）迁移不可逆：迁移前 `DatabaseService` 先做 backup（已有 `/database/backup` 机制），迁移事务失败自动回滚。
- `bundle.point_counts` 缺口（A1 已知）：CSV 需要的列全部有；`diagnostics.jsonl` 的额外键在业务侧不再产出，依赖它们的脚本（`compare_icp_diagnostics.py` 等）在 B10 里标记为「读 LyFlow 的 bundle」或退休。
