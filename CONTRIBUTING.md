# 贡献指南

感谢你愿意参与 `bag2mkv`！这个项目会以 **MIT License** 在 GitHub 开源，欢迎任何形式的贡献：

- 报告 bug、兼容性问题或文档错误；
- 提交代码（新功能、性能优化、测试、跨播放器兼容性）；
- 帮助验证不同 Orbbec 设备、固件、bag 录制版本和 K4A 播放器；
- 完善文档、示例和构建说明。

## 先读这些

- [README.md](README.md)：功能、构建和使用方式。
- [AGENTS.md](AGENTS.md)：输入/输出格式细节和**无损契约**。改格式相关代码前必须先读第 2–4 节。
- [LICENSE](LICENSE)：MIT License，版权归林山力所有。

## 最重要的约束

`bag2mkv` 的价值是**可验证的无损**。提交 PR 时不要：

- 把彩色 JPEG 解码后再用 H.264/HEVC/VP9/AV1/JPEG 重编码；
- 对深度做滤波、对齐、缩放、单位换算或无效值替换；
- 丢掉时间戳、帧号、metadata、stream profile、property 或标定信息；
- 为了“播放器兼容”而静默改变码流。

如果确实需要新增有损/压缩模式，必须作为**显式可选**功能，不能替换默认的 K4A 兼容输出，并在 PR 中说明取舍。

## 开发环境

- Windows 10/11
- Visual Studio 2022（MSVC，x64）
- CMake 3.16+
- 仓库自带的 `azurekinectsdk/`（构建必需，已提交）
- 可选：用于完整样本测试的 Orbbec `.bag` 文件

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

构建产物：

- `build\Release\bag2mkv.exe`：命令行
- `build\Release\bag2mkv-gui.exe`：GUI

## 测试

最低要求：

```powershell
ctest --test-dir build -C Release --output-on-failure
.\build\Release\bag2mkv.exe --help
```

有样本 bag 时，建议跑完整转换和验证：

```powershell
# 小体积快速回归（约 1.2 GB 输出）
.\tests\run_sample.cmd -Color mjpg

# 默认 BGRA + 原始 MJPEG 轨道（约 12.5 GB 输出）
.\tests\run_sample.cmd
```

`tests/run_sample.ps1` 需要同目录或通过 `-Bag` 指定 bag；样本文件不会被提交。
如果没有样本，也可以只跑 `ctest`，或自己准备一个短 bag。

## 代码约定

- C++17，Windows/MSVC；不要引入新的重量级依赖。新增依赖前先在 Issue 中讨论许可证、构建方式和可移植性。
- CLI 和 GUI 必须共用 `src/converter.cpp` 的核心逻辑，不要在 GUI 中复制转换/验证逻辑。
- 保持 `inspect` / `convert` / `verify` / `--help` 的行为一致；新增选项要同时更新 CLI 帮助和 README。
- 涉及 MKV/K4A 格式的改动，必须同时更新 `AGENTS.md` 的格式说明和验证步骤。
- 不要在仓库中提交 `.bag`、`.mkv`、`out/`、`build/` 或临时脚本；这些已在 `.gitignore` 中忽略。
- 不要把真实患者/受试者数据、设备序列号或未脱敏录制上传到公开 Issue。

## 提交 Issue

请尽量附上：

- 操作系统、VS/CMake 版本；
- `bag2mkv --version`；
- 完整命令行（可隐去私人路径）；
- 错误日志/`--verify` 输出；
- bag 的设备、固件、分辨率、帧率、彩色/深度编码；
- 能否用 K4A playback / k4aviewer / 你自己的播放器打开。

**不要**直接上传几百 MB/几 GB 的 bag；可以提供一个最小复现片段或关键日志。

## Pull Request

1. Fork 仓库，从 `main` 创建分支。
2. 一个 PR 只做一件事；格式修复和功能开发尽量分开。
3. 确保 `ctest` 通过；有样本时附上 `verify` 结果。
4. 更新相关文档（README / AGENTS / tests）。
5. 在 PR 描述中说明：改了什么、为什么、如何验证、是否有兼容性影响。

提交代码即表示同意以本项目的 MIT License 授权你的贡献。

## 特别欢迎的方向

- IR / IMU 流支持和对应 K4A 轨道；
- 更多 Orbbec 设备/固件/模式的实测；
- 标定畸变和 depth→color 外参的逐点验证；
- 不同 K4A 播放器（k4aviewer、kinect-analyzer、其他 K4A 兼容工具）的兼容性测试；
- 更完善的自动化测试、CI 和错误提示；
- 文档、中文/英文翻译和使用示例。

再次感谢你的贡献！
