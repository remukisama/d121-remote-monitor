# 数据库备份与恢复

更新：2026-10-22。工具为 `tools/database_backup.py`，备份七张应用表：samples、diagnostics、motions、contexts、events、sessions、members。它与只迁移历史、不迁移授权的 `migrate_history.py` 用途不同。

备份使用 SQLite 文件作为可校验的离线载体，另有 `.manifest.json` 保存文件 SHA256、每表行数和按主键排序的逐行摘要。文件包含用户授权及会话哈希，应保存在私有目录；本机使用已被 Git 忽略的 `output/backups/`，不进入部署源码 ZIP。哈希用于发现意外修改，不提供加密或签名。

## 本机备份与新文件恢复

在项目目录执行：

```powershell
& .\.venv\Scripts\python.exe tools\database_backup.py backup-sqlite --source data\monitor.db --target output\backups\monitor-20261020.db
& .\.venv\Scripts\python.exe tools\database_backup.py verify --source output\backups\monitor-20261020.db
& .\.venv\Scripts\python.exe tools\database_backup.py restore-sqlite --source output\backups\monitor-20261020.db --target output\backups\drill-20261020.db
```

SQLite 在线备份 API 包含已提交的 WAL 数据；不直接复制正在使用的 `.db`。缺失源文件不会被当作空数据库创建。目标数据库或清单已存在则拒绝覆盖；同目录完整临时文件以硬链接发布。恢复只创建新文件，先检查备份完整性，再比较恢复后的七表内容，不替换运行中的原库。

旧版本缺少 motions/contexts 时，仅在隔离备份中补建对应空表，并在清单 `schema_added_empty_tables` 记录；原库不升级，缺少其他必需表或列不匹配会失败。

## 云 MySQL 备份

须在能完成数据库认证的网络内运行。工具使用独立连接，以 REPEATABLE READ + WITH CONSISTENT SNAPSHOT + READ ONLY 导出；要求现有应用表均为 InnoDB。数据分批流式读取，不停止采集进程，不修改生产表。备份期间不要进行表结构变更。

电脑上读取现有私有配置：

```powershell
& .\.venv\Scripts\python.exe tools\database_backup.py backup-mysql --target output\backups\mysql-20261020.db
```

本版容器包含此工具；在具备容器终端访问权限时，可显式使用已配置的运行时环境变量，不需要重新粘贴密码：

```sh
python tools/database_backup.py backup-mysql --runtime-env --target /tmp/d121-mysql-20261020.db
python tools/database_backup.py verify --source /tmp/d121-mysql-20261020.db
```

`--runtime-env` 读取 MYSQL_ADDRESS、MYSQL_USERNAME、MYSQL_PASSWORD、MYSQL_DATABASE；未指定时使用本机私有配置，环境变量不会悄悄覆盖目标。备份完成后须将 `.db` 和 `.manifest.json` 一起导出到受控持久位置，不能只留在容器临时文件系统。

## MySQL 恢复演练

恢复始终创建全新的 `d121_restore_*` 数据库。名称必须与配置中的生产库及备份记录的来源库不同；目标即使已存在且为空也会拒绝。账号需有建库权限。不会清库、删表或写回原生产库。

```sh
python tools/database_backup.py restore-mysql --runtime-env --source /tmp/d121-mysql-20261020.db --target-database d121_restore_20261020_01
```

恢复创建七表，单个数据事务内写入，提交前检查每表全部行的摘要；数据写入或检查失败会回滚，保留新建的隔离库供检查。恢复库不能启动第二个 MQTT 采集器；先离线检查授权、最新数据、事件与快照，再另行安排正式切换。重新运行时使用新名称，工具不自动删除演练库。

数据库备份不包含运行时 AppSecret、配对配置和 MQTT 凭据；灾难恢复还需要保留原私有运行配置。现有 `config.private.json` 与生成的云端私有配置应在受控位置单独备份，不上传公开仓库。

## 本轮证据与边界

- 真实本机旧历史：23,091 条摘要、4,241 条诊断、312 条事件、17 条会话；恢复副本七表逐行摘要一致，最新读数及 20 条事件详情一致。motions/contexts 原库不存在，在备份中补建为空表。
- 自动测试覆盖 WAL 提交/未提交边界、后续写入隔离、覆盖竞争、损坏、旧表兼容、授权保留和失败清理；MySQL 事务控制及回滚使用适配器模拟，不代表真实 MySQL 引擎验收。
- 2026-10-22 已在生产版本 `d121-monitor-015` 的容器终端完成真实 MySQL 一致性备份，并恢复至全新 `d121_restore_20261022_01`：21,577 条摘要、5,593 条诊断、17,221 条振动、87 条上下文、296 条事件、1 条会话和 4 个成员，共 44,779 条记录。原生产库未被恢复工具写入。
- 恢复提交后，用新连接重新导出隔离库，七表逐行摘要与原备份一致；最新读数和 20 条事件详情通过实际 Store 查询比较。生产库同期摘要数由 22,346 增至 22,349，双站新数据正常入库，`/ready` 各项为 true。
- 备份及清单已从容器导出并在本机重新校验，位于 `output/backups/mysql-20261022-export/`。备份 SHA256 为 `02de10cfe39d4f1c695c16c8607daba3197020aca9cde7d1474d3118111cca44`。下载接口本轮限制单文件 10,240 字节，使用 220 个分段传输压缩包，重组后验证 ZIP CRC、完整文件 SHA256 和逐表摘要。不要把临时容器副本当作持久备份。
- 云平台另有发布前整集群快照 `11092651`（2026-10-22 16:02:47 成功）和逻辑备份 `11064300`（2026-10-20 17:50:29 成功）。尚未实际执行平台整集群回档或生产库替换；本次完成的是隔离恢复演练。演练库保留供检查，没有启动第二个采集器。
- 电脑直连 MySQL 的旧握手失败（2013）仍为网络路径边界；当前有效运维路径是已认证的云端容器终端。
- 本机备份及演练副本位于 `output/backups/`；跨工程公开证据为 `D:\xuantie\work\delivery-hardening-20261020\sqlite-restore-drill.json`，不含用户行内容或密钥。

参考：[SQLite 在线备份](https://sqlite.org/backup.html)、[Python sqlite3 backup](https://docs.python.org/3.13/library/sqlite3.html)、[MySQL 一致性事务](https://dev.mysql.com/doc/refman/8.0/en/commit.html)。
