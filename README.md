# bag2mkv v0.2.0

把 Orbbec SDK 录制的 RGB-D `.bag` 无损转换成 Azure Kinect（K4A）兼容的 `.mkv`。
提供 **命令行版** `bag2mkv.exe` 和 **Windows 原生 GUI** `bag2mkv-gui.exe`，两者共用同一个转换核心。

- 彩色：默认解码为 **BGRA 1080p** 写入 `COLOR` 轨（兼容 kinect-analyzer 等只处理 BGRA 的 K4A 播放器）；原始 MJPEG 字节流原样保存到 `ORIGINAL_COLOR_MJPG` 自定义轨道，不丢失压缩源码流。
- 深度：`mono16` 逐值保留，文件内为 K4A `b16g`（BE）；由 K4A writer 从 LE 转换。
- 元数据：设备信息、stream profile、property、逐帧时间戳/帧号/metadata 写入 MKV attachment。
- 验证：K4A playback 逐帧比对 BGRA 像素/深度值/时间戳，并校验原始 MJPEG 轨道的 SHA-256。

> 默认输出约 **12.5 GB / 45 s**（BGRA 1080p 很大）。只需要 K4A 原码流、不要求旧播放器兼容时，用 `--color mjpg` 输出约 **1.2 GB**。

## 构建

需要 Visual Studio 2022（MSVC）、CMake 3.16+、仓库自带的 `azurekinectsdk/`。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

构建后会把 K4A 运行时 DLL 复制到 `build/Release/`，生成：

- `build\Release\bag2mkv.exe`：命令行。
- `build\Release\bag2mkv-gui.exe`：GUI（没有控制台窗口）。

## GUI 使用

双击 `build\Release\bag2mkv-gui.exe`，然后：

1. 选择或拖拽输入 `.bag`；输出路径会自动建议为同名 `.mkv`。
2. 选择彩色输出：
   - `BGRA 1080p`：默认，kinect-analyzer 可播放，约 12.5 GB。
   - `MJPEG 原码流`：约 1.2 GB，但只支持 MJPG 的播放器才能显示。
3. 按需勾选“保留原始 MJPEG 轨道”“转换后自动验证”“覆盖已存在的输出文件”。
4. 可选：选择同设备/同模式的 `calibration.json`；不选则从 bag 的 stream profile 合成。
5. 点击“开始转换”。界面会显示扫描/写入/验证进度，可随时取消；日志在底部。

GUI 也支持命令行预填，方便“打开方式”或脚本冒烟测试：

```powershell
.\build\Release\bag2mkv-gui.exe capture.bag capture.mkv
.\build\Release\bag2mkv-gui.exe capture.bag capture_mjpg.mkv --mjpg --start
```

- 第一个非选项参数：输入 bag。
- 第二个非选项参数：输出 mkv（省略则自动建议）。
- `--mjpg`：预选 MJPEG 原码流。
- `--start`：启动后自动开始转换。

## 命令行使用

### 帮助

```powershell
.\build\Release\bag2mkv.exe --help
.\build\Release\bag2mkv.exe help convert
.\build\Release\bag2mkv.exe help verify
.\build\Release\bag2mkv.exe --version
```

`convert --help` 包含全部选项、彩色模式、文件大小、标定说明、示例和退出码。

### 常用命令

```powershell
# 查看 bag 内容
.\build\Release\bag2mkv.exe inspect .\20251111_143747_001_cam1_CL8R35300SH_gait.bag

# 默认：BGRA 1080p + 原始 MJPEG 轨道，转换并验证
.\build\Release\bag2mkv.exe convert .\20251111_143747_001_cam1_CL8R35300SH_gait.bag `
    -o .\out.mkv --verify

# 只保留原始 MJPEG（体积小，但 kinect-analyzer 不认 MJPG 彩色）
.\build\Release\bag2mkv.exe convert .\...bag -o .\out_mjpg.mkv --color mjpg

# 只要 BGRA、不要原始 MJPEG 轨道
.\build\Release\bag2mkv.exe convert .\...bag -o .\out.mkv --no-original-color

# 单独验证
.\build\Release\bag2mkv.exe verify .\...bag .\out.mkv
```

常用选项：

- `--color bgra|mjpg`：彩色输出格式，默认 `bgra`。
- `--no-original-color`：BGRA 输出时不再附加 `ORIGINAL_COLOR_MJPG` 轨道，输出约 12.3 GB。
- `--force`：覆盖已存在的输出。
- `--no-calibration`：不写 `calibration.json`。
- `--calibration-json <file>`：使用同设备/同模式的参考 K4A 标定。
- `--verify`：转换后立即逐帧验证。

退出码：`0` 成功，`1` 转换/验证失败，`2` 用法错误，`3` 用户取消。

## 输出结构（默认 BGRA）

- `COLOR`：`BGRA_1080P`，1920×1080，8,294,400 B/帧。
- `ORIGINAL_COLOR_MJPG`：自定义轨道，原始 JPEG 字节流逐帧保存（CodecID `V_MS/VFW/FOURCC`, `MJPG`）。
- `DEPTH`：`NFOV_UNBINNED`，640×576，`b16g`。
- `IR`：K4A writer 自动创建的空轨（本样本无 IR 数据）。
- `calibration.json`：默认从 stream profile 合成；见下方限制。
- `orbbec_manifest.json`：设备、流、属性、帧统计、彩色输出模式。
- `orbbec_frames.jsonl`：逐帧时间戳、帧号、metadata、原始 JPEG payload SHA-256。
- `orbbec_prop_*.bin`：原始标定/属性 blob。

## 播放器兼容性

kinect-analyzer 的 `ImageProcessor::convertK4AColorToQImage()` / `SyncPlaybackManager::convertK4AColorToQImage()` 只对 `K4A_IMAGE_FORMAT_COLOR_BGRA32` 做正确处理；其他格式会按每行 `width*4` 字节读取，MJPG 码流会被越界误读。因此本工具默认把彩色解码成 BGRA 1080p，同时用自定义轨道保留原始 MJPEG。K4A playback API 与 kinect-analyzer 的 `PlaybackSession` 使用同一条读取路径。

## 标定限制

`calibration.json` 的默认版本由 `OBStreamProfileInfo` 合成。深度按 K4A 的 1024×1024 原始传感器模型、彩色按 4096×3072/4:3 模型，经过 K4A 的 mode-specific crop/scale 后，内参与 bag profile 完全一致（已用 `k4a_playback_get_calibration` 验证）。畸变系数和 depth→color 外参还没有做逐点投影验证；要做严格点云/对齐，建议用 `--calibration-json` 传入同设备、同模式的 K4A 录制中的 `calibration.json`。

当前版本不支持 IMU；为了让 K4A 能解析相机标定，`calibration.json` 里包含 gyro/accel 的零占位 entry，不应作为真实 IMU 标定使用。

## 当前版本限制

- 输入：Orbbec SDK v2 的 `#ROSBAG V2.0`，支持 LZ4 / 未压缩 chunk；暂不支持 BZ2。
- 支持 MJPG 1080p（以及 1440p/1536p/2160p/3072p）和深度 NFOV/WFOV 常见模式。
- 暂不支持 IR / IMU 流；遇到会明确报错，不会静默丢弃。
- K4A 输出依赖 `azurekinectsdk/` 的 `k4a.dll` / `k4arecord.dll`，GUI 还依赖 Windows 自带的 Common Controls。
- BGRA 1080p 输出体积很大；这是为了旧播放器兼容而做的解码，不是有损重编码。
- GUI 当前为 Windows 原生界面；不提供 macOS/Linux GUI。

## 无损约定

- 原始 MJPEG 字节流原样保存在 `ORIGINAL_COLOR_MJPG`；BGRA 只是同一 JPEG 的确定性解码结果，没有额外有损处理。
- `--color mjpg` 时彩色只复制原码流，不做任何解码/重编码。
- 不做深度滤波、对齐、缩放或单位换算。
- 验证：BGRA 模式下逐像素比对 bag JPEG 解码结果；MJPG 模式下逐帧 SHA-256 比对；同时校验 `ORIGINAL_COLOR_MJPG` 的原始 JPEG 哈希。
- 输出先写 `.tmp`，成功后再 rename；取消或失败会删除临时文件。
