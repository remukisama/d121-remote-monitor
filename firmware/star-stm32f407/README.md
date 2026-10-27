# 星火 STM32F407 从站

从站地址 1，Modbus RTU 115200 / 8N2，提供 AHT21 温湿度、ICM-42688-P 六轴缓存快照和 LED 保持寄存器。查询路径读取有时效的缓存，不在应答时重新探测传感器。

本目录包含原 RT-Thread Studio 工程的应用代码、板级驱动、STM32 HAL/CMSIS 和 RT-Thread 源码。`build-manifest.json` 保存原已成功构建工程的 166 个编译单元、包含路径和源文件哈希。两个未被当前链接使用的 CMSIS 预编译数学库未纳入仓库；构建使用工具链提供的 C 数学库。上游源码原有许可证保留在 `rt-thread/LICENSE` 和 `libraries/STM32F4xx_HAL_Driver/LICENSE.md` 等位置。

```powershell
python tools/build_star.py --toolchain 'RT-Thread Studio ARM GCC 5.4.1 的 bin 目录'
```

从仓库根目录运行，生成本目录 `build/rtthread.bin`。实际已使用 RT-Thread Studio 的 ARM GCC 5.4.1 在本机从本仓库目录编译通过。新镜像包含构建时间字符串，重复构建的哈希会变化；其长度与此前烧录镜像也不同，具体差异尚未逐项定位。此前已在板上验收的原镜像 SHA256 为 `e53e466c7ccde81c45152e337cc4755b90e4951d50aa57570388359897d8ef41`。当前新编译产物没有再烧录。

原 `.cproject` 可能记录旧电脑的工具链/工作区路径；使用 Studio 导入时重新配置和生成 Debug 构建文件。仓库内可移植脚本以相对路径调用编译器，不依赖原生成 Makefile。
