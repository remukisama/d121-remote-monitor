# D121 主站

`overlay/` 保存项目对 ArtInChip Luban-Lite 的主站应用、黑匣子、Modbus、网络任务、界面及板级配置等覆盖源码；`source-manifest.json` 固定上游基线、文件哈希及 360 个当时删除的文件，其中 359 个是厂商示例及素材，另一个是旧工具。终端历史、个人编辑器配置及工具状态未导出。

在仓库根目录执行：

```powershell
python tools/prepare_vendor.py d121 --destination ../d121-sdk
```

脚本检出上游 `b523d1eb6063daebce7885fb4cbbbf0c7047182a` 并应用覆盖文件与已验证配置种子；也可通过 `--source <已有SDK Git目录>` 使用本地源进行隔离克隆。目标目录必须不存在或是同一干净基线。所有删除操作限定在目标 SDK 内，脚本不会修改原工作目录。已应用目录再次执行会校验覆盖文件。

构建需要 ArtInChip 对应交叉工具链及 SDK 自带 SCons/Python 环境。配置为 `d12x_demo68-nor_rt-thread_helloworld_defconfig`；先在隔离目录应用并构建 `d12x_demo68-nor_baremetal_bootloader_defconfig`，再恢复主站配置并构建应用，以生成完整打包所需的 Bootloader 参数。Windows 的现有交叉链接器对中文绝对路径存在兼容问题，应在不含中文的路径，或通过 ASCII 路径的目录联接构建。`config-seed/` 是现有已验证配置的快照，其他板型必须重新配置。

历史实机写入的主站 OS/rodata 镜像 SHA256 为 `7761ee85bef2dc34377b6c57a229e206a268dd061c817845495d66094af15a41`。新构建的二进制需要单独计算哈希；编译通过不代表已在板上烧录或验证。使用 SDK 内 Python 运行 `tools/flash/package_update.py --sdk <SDK目录>` 生成仅含 OS/rodata 的更新包，再用 `tools/flash/d12x_flash.ps1 -Image <镜像> -Tool <upgcmd.exe>` 明确指定已安装的烧录工具。现有 SDK 的 `scons --aicupg` 仍依赖原本机工作目录，请使用本仓库维护工具。
