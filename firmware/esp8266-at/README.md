# ESP8266 网络模块

本项目以 ESP-AT 2.2.2.0 为基础，启用证书有效期校验和自动 SNTP；最终 AT 接口 TX=GPIO15/D8、RX=GPIO13/D7。CA 索引 0 用于实际服务器认证，索引 1、2 用于负向测试。

```powershell
python tools/prepare_vendor.py esp8266-at --destination ../esp-at
```

该命令在新目录检出 ESP-AT 基线 `5249c31d1df78c68622b33ffa9fd82b9991c28be`，并应用 `overlay/` 中的项目更改。项目额外指定工厂镜像打包依赖 Bootloader，避免首次并行构建时提前打包。然后按上游方法准备 ESP8266 RTOS SDK `ff16a35c362047b47c0b3d29c02ef9755b623447` 及其子模块、LX106 交叉工具链和隔离 Python 环境；项目保留的 `tools/d121_build.sh` 和 SDK Unicode 路径补丁会检查版本并构建。原机器的 WSL 工具链路径与依赖版本见 `overlay/D121_TLS_BUILD.md`，迁移电脑时需按实际安装路径调整。

2026-10-25 实际模块对正确 CA/域名、错误 CA、错误域名和过期证书进行了正反对照，主站加密上传及短时恢复通过。2026-10-27 隔离重建中修正上述打包依赖，构建成功；新应用 886,768 字节，证书日期校验开启，工厂参数 TX GPIO15 / RX GPIO13，CA 分区 SHA256 与已验收版本一致。新应用二进制与历史上板版本不相同，尚未烧录验证。长期观测仍未验收；设备 Wi-Fi 与服务凭据不在本仓库。
