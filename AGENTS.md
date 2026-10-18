# AGENTS.md — bag2mkv

> 给在本仓库工作的编码 Agent 的项目说明。先读第 1、2 节；改格式相关代码前必须读第 3、4 节。

## 1. 项目是什么

`bag2mkv` 是一个 **RGB-D 数据格式转换工具**：把 Orbbec SDK 录制的 ROS1 `.bag` 转换成 Azure Kinect（K4A）兼容的 `.mkv`。

核心目标只有一个：**无损、最大程度保留信息**。

- 输入：`#ROSBAG V2.0`，Orbbec SDK v2 的 rosbag 录制格式（非标准 ROS bag 细节，见 §3）。
- 输出：K4A 录制格式的 Matroska `.mkv`，优先保证 `k4aviewer` / Azure Kinect SDK / Orbbec K4A Wrapper 可打开。
- 不是：点云重建、深度滤波、彩色图去畸变、视频转码、实时采集。

### 1.1 仓库现状（重要）

当前仓库已有 **v0.4.3 可运行实现**（C++17 + CMake，命令行 + Windows GUI）。除下面这些第三方/样本目录外，源码在 `src/`，构建文件是 `CMakeLists.txt`，测试在 `tests/`，使用说明在 `README.md`，贡献指南在 `CONTRIBUTING.md`，版本变化见 `CHANGELOG.md`，许可证是 `LICENSE`。

| 路径 | 内容 |
|---|---|
| `azurekinectsdk/` | **构建必需**：预编译的 Orbbec K4A Wrapper 1.4.0-private（`k4a.dll`、`k4arecord.dll`、头文件、lib、工具） |
| `20251111_143747_001_cam1_CL8R35300SH_gait.bag` | 本地测试样本输入，797,840,685 B（被 `.gitignore` 忽略，不提交） |

Orbbec SDK v2 源码（`OrbbecSDK_v2-main/`）和 Body Tracking SDK（`azurekinectbodytrackingsdk/`）
不属于构建依赖，已从仓库删除并加入 `.gitignore`；需要查阅格式实现时从上游仓库获取。
本文件中的 `OrbbecSDK_v2-main/...` 路径均指上游源码参考，不是本仓库文件。

样本文件很大，**不要修改、不要提交、不要复制进构建产物**。它们只用于测试和格式对照。

### 1.2 已实现的 v0.4.3

```powershell
.\build\Release\bag2mkv.exe --help
.\build\Release\bag2mkv.exe help convert
.\build\Release\bag2mkv.exe inspect <bag>
.\build\Release\bag2mkv.exe convert <bag> -o <mkv> [--force] [--no-calibration] [--calibration-json <file>]
                                  [--verify] [--color bgra|mjpg] [--no-original-color]
.\build\Release\bag2mkv.exe verify <bag> <mkv>
.\build\Release\bag2mkv.exe batch <files-or-dir...> --output-dir <dir> [--verify] [--continue-on-error] [--limit N] [--no-recursive]
.\build\Release\bag2mkv.exe mkv2bag <input.mkv> [--output <output.bag>] [--force]
.\build\Release\bag2mkv.exe --version
.\build\Release\bag2mkv.exe --self-test
.\build\Release\bag2mkv-gui.exe [input-dir] [output-dir] [--mjpg] [--start]
.\build\Release\bag2mkv-gui.exe --batch <input-dir> --batch-output <output-dir> [--mjpg]
.\build\Release\bag2mkv-gui.exe --to-bag <input.mkv> [--to-bag-output <output.bag>]
```

GUI 与 CLI 共用 `bag2mkv_core`：输入目录/输出目录、自动判断 bag→mkv 或 mkv→bag、递归批量、彩色模式、自动验证、
覆盖确认、进度、取消、日志、打开输出目录。输入目录必须只含一种格式；子目录会镜像到输出目录。

样本 bag 已跑通：

- 读取 1318 个 chunk、6 个 connection、47 条 property。
- 转换 2715 帧（COLOR 1366 / DEPTH 1349）。
- 默认输出 **BGRA 1080p + ORIGINAL_COLOR_MJPG 源轨道**，约 **12.53 GB**。
- `--color mjpg` 输出约 1.20 GB 的 MJPG-only 文件（K4A 可读；2026-10-18 起 kinect-analyzer 也可播放）。
- `mkv2bag` 已跑通：`--color mjpg` MKV → bag（约 797 MB，LZ4）→ MKV，并对照原始 bag 验证通过；带 source track 的默认 MKV 反向也通过。
- GUI 目录批量已跑通：bag 目录 → 输出目录生成同名 mkv；mkv 目录 → 输出目录生成同名 bag；嵌套子目录结构保留。
- `verify` 逐帧验证 BGRA 像素/深度/时间戳，并校验原始 MJPEG 源轨道：
  - `verify: ORIGINAL_COLOR_MJPG OK (1366 frames)`
  - `verify: OK (1366 color, 1349 depth)`
- 合成 calibration 经 `k4a_playback_get_calibration` 读取后，内参与 bag 的 profile 完全一致：
  - depth `640x576 fx=504.579 fy=504.583 cx=338.478 cy=345.674`
  - color `1920x1080 fx=1118.69 fy=1118.11 cx=963.672 cy=535.888`
- K4A playback 的彩色是 `BGRA` 1920×1080，深度是 `DEPTH16` 小端。

---

## 2. 无损契约（不可违反）

这些是项目验收的硬约束。任何“优化”如果违反其中一条，都必须先和用户确认。

1. **彩色不重编码**
   - bag 里的彩色是 `mjpg`（JPEG 码流）。默认输出把 JPEG 解码成 **BGRA 1080p** 写入 `COLOR` 轨，以兼容旧版播放器（新版 kinect-analyzer 也已支持 MJPG）。
   - 原始 JPEG 字节流必须同时保存到自定义轨道 `ORIGINAL_COLOR_MJPG`（codec `V_MS/VFW/FOURCC`，`MJPG`）。BGRA 只是 JPEG 的确定性解码结果，不引入额外有损步骤；原始码流用于真正的无损追溯。
   - 禁止用 H.264/HEVC/VP9/AV1/JPEG 再编码。
   - `--color mjpg` 是“只复制原始码流、不转 BGRA”的小体积模式（~1.2 GB）。2026-10-18 起 kinect-analyzer 的 `ImageProcessor::convertK4AColorToQImage()` 已支持 `K4A_IMAGE_FORMAT_COLOR_MJPG`（Qt JPEG 插件），MJPG 文件现在也可播放；旧版本播放器仍只支持 BGRA。
   - `--no-original-color` 可省略源轨道，默认输出约 12.3 GB。

2. **深度逐值保留**
   - bag 深度是 `mono16`，640×576，little-endian，单位毫米（抽样值域 0–13804）。
   - K4A 的 `b16g` 轨道是 **big-endian**；只允许做 16-bit 字节序交换，禁止滤波、对齐、缩放、单位换算、无效值替换。
   - 实现上把 **LE** buffer 交给 `k4a_record_write_capture`，由 K4A writer 内部转成文件里的 BE；不要在调用前自己 swap（会双重交换）。
   - 必须能在验证阶段把 MKV 的 BE 数据换回 LE 后，与 bag 原始 `data[]` 逐字节相等。

3. **不使用任何有损编码**
   - 深度/IR 不压缩（K4A 要求 raw `b16g`）。
   - 彩色只允许“原码流复制”；若未来提供压缩选项，只能是 FFV1 等无损编码，且不能作为默认输出，不能替代 K4A 兼容输出。

4. **元数据不丢**
   - 必须保留：设备信息、stream profile（内参/畸变/外参/fps/format）、全部 property、每帧 `number`、`timestamp_usec`、`timestamp_systemusec`、`timestamp_globalusec`、`metadata` 原始字节、`metadatasize`、`pixel_bit_size`、原始 `header.seq`/`header.frame_id`。
   - 推荐用 MKV attachment 保存 JSON/JSONL manifest；K4A 播放器会忽略未知 attachment，不影响兼容性。
   - 不要只保留 ROS header 的 `stamp` 而丢掉 frame 的 `timestamp_usec`。

5. **输入只读，输出原子写**
   - 不修改输入 bag。
   - 先写 `<output>.tmp`，成功 flush/close 后再 rename；失败删除临时文件。
   - 默认不覆盖已存在的输出；需要 `--force`。

6. **可验证**
   - 每次转换都要能输出一份 manifest（帧数、首末时间戳、payload SHA-256、标定来源）。
   - `verify` 必须能独立比较 bag 与 mkv，不能只信转换过程自己的计数。

---

## 3. 输入格式：Orbbec 的 `.bag`

样本 bag 是 `#ROSBAG V2.0`，但由 Orbbec fork 的 rosbag 写入，**不要假设标准 rosbag 库能直接读**。

### 3.1 容器层

- 文件头 13 字节：`#ROSBAG V2.0\n`。
- bag header 记录包含 `chunk_count`、`conn_count`、`index_pos`，后面有 4 字节 data length + padding。
- 样本：`chunk_count=1318`，`conn_count=6`，`index_pos=797670270`。
- 顶层记录：`op=5` chunk、`op=4` index data、`op=6` chunk info、`op=7` connection（位于 `index_pos` 附近）。
- 每个 chunk：
  - chunk header 字段：`compression=lz4`、`op=5`、`size=<uncompressed>`。
  - chunk 数据开头是 **4 字节 compressed_size**，随后是 LZ4 frame。
  - 注意：chunk header 的 `size` 与实际解压长度可能不一致（样本首 chunk 声明 899,744，实际 900,000）。**以 4 字节 compressed_size 为准**，不要用 `size` 定位下一个 chunk。
  - LZ4 frame 是标准 frame（7 字节头、无 content size），Python `lz4.frame` / C++ `LZ4F_decompress` 可解。
  - 也兼容 `compression=none` / `bz2` 的 bag，但样本是 lz4。
- 解压后的 chunk 内是记录流：
  - **connection 记录被拆成两个 header record**：
    1. `{conn, op=7, topic}`
    2. `{md5sum, message_definition, type}`（没有 `op`）
  - **message 记录也没有 `data=` 字段**：
    - header `{conn, op=2, time}`
    - 4 字节 payload size
    - 原始 payload 字节
  - 这是本格式最容易踩的坑；按标准 ROS bag 的 `data` 字段解析会错位。
- 顶层 index 记录（`op=4`）在 header 后还有 4 字节 data length + 数据，跳过时要一起跳。

参考实现（上游 Orbbec SDK，未随本仓库提交）：

- `OrbbecSDK_v2-main/3rdparty/rosbag/src/rosbag_storage/src/bag.cpp`
- `.../include/rosbag/bag.h` 的 `writeConnectionRecord` / `writeMessageDataRecord`
- `OrbbecSDK_v2-main/3rdparty/rosbag/src/rosbag_storage/src/lz4_stream.cpp`
- `OrbbecSDK_v2-main/3rdparty/rosbag/src/roslz4/src/lz4s.c`

### 3.2 样本 bag 的设备与流

- 设备：**Orbbec Femto Bolt**
  - `sn=CL8R35300SH`
  - `name=Femto Bolt`，`fullName=Orbbec Femto Bolt`
  - `fwVersion=1.1.2`，`hwVersion=1.2`
  - `vid=11205 (0x2BC5)`，`pid=1643 (0x066B)`
  - `connectionType=USB3.1`
- 6 个 connection：

| conn | topic | type | 数量 | 说明 |
|---:|---|---|---:|---|
| 0 | `/cam/property` | `custom_msg/OBProperty` | 47 | 设备属性/标定原始数据 |
| 1 | `/cam/sensor_3/frameType_3` | `sensor_msgs/Image` | 1349 | 深度 640×576 `mono16` |
| 2 | `/cam/sensor_2/frameType_2` | `sensor_msgs/Image` | 1366 | 彩色 1920×1080 `mjpg` |
| 3 | `/cam/deviceInfo` | `custom_msg/OBDeviceInfo` | 1 | 设备信息 |
| 4 | `/cam/streamProfileType_2` | `custom_msg/OBStreamProfileInfo` | 1 | 彩色 profile |
| 5 | `/cam/streamProfileType_3` | `custom_msg/OBStreamProfileInfo` | 1 | 深度 profile |

- 时间范围：约 44.93 s；时间戳是 epoch 微秒（`1762843067…`），与文件名本地时间一致。
- 该样本 **没有 IR、没有 IMU**。不要为了“凑齐 K4A 三轨”伪造 IR 数据。

### 3.3 `sensor_msgs/Image` 的真实序列化

Orbbec 的 `Image.h` 在标准 `sensor_msgs/Image` 后面追加了字段，但 **message definition 字符串没有列出 `depth_units` / `pixel_bit_size`**。必须按下面顺序手写解析：

```text
header:
  uint32 seq
  uint32 stamp.sec
  uint32 stamp.nsec
  string frame_id          // uint32 len + bytes
uint32 height
uint32 width
string encoding            // uint32 len + bytes
uint8  is_bigendian
uint32 step
uint8[] data               // uint32 len + bytes
float32 depth_units        // 不在 definition 中
uint64 number
uint64 timestamp_usec
uint64 timestamp_systemusec
uint64 timestamp_globalusec
uint8[] metadata           // uint32 len + bytes
uint32 metadatasize
uint8 pixel_bit_size       // 不在 definition 中
```

样本值：

- 彩色：`1920×1080`，`encoding="mjpg"`，`is_bigendian=0`，`step=1920`，`data` 是完整 JPEG（约 146–164 KB），`pixel_bit_size=0`。
- 深度：`640×576`，`encoding="mono16"`，`step=1280`，`data=737280` B，`pixel_bit_size=16`，`depth_units=0.0`。
- `metadata` 12 B，形如 `0c 00 <8-byte timestamp_usec> <2 bytes>`；**原样保存**，不要猜语义。
- `number`、`timestamp_usec` 等是帧级信息；不要用 `header.stamp` 替代。

参考：`OrbbecSDK_v2-main/3rdparty/rosbag/src/msgs/sensor_msgs/Image.h` 的 `Serializer::allInOne`。

### 3.4 自定义消息

#### `OBDeviceInfo`

```text
header, string name, fullName, asicName,
uint16 vid, uint16 pid,
string uid, sn, fwVersion, hwVersion, supportedSdkVersion, connectionType,
uint16 type, uint8 backendType,
string ipAddress, localMac
```

#### `OBStreamProfileInfo`

```text
header,
uint8 streamType,
uint8 format,
float32 rotationMatrix[9],
float32 translationMatrix[3],
uint16 width, height, fps,
float32 cameraIntrinsic[4],     // fx, fy, cx, cy
float32 cameraDistortion[8],    // k1..k6, p1, p2
uint8 distortionModel
```

样本：

- 彩色 `streamType=2`（COLOR），`format=5`（MJPG），1920×1080@30；
  - intrinsic: `fx=1118.6868, fy=1118.1115, cx=963.6718, cy=535.8882`
  - distortion: `k1=0.07736196, k2=-0.10566475, k3=0.04333767, k4=0, k5=0, k6=0, p1=0.00020745, p2=0.00009601`
  - `distortionModel=4`（Brown-Conrady K6）
- 深度 `streamType=3`（DEPTH），`format=8`（Y16），640×576@30；
  - intrinsic: `fx=504.5792, fy=504.5826, cx=338.4782, cy=345.6738`
  - distortion: `k1=22.80900, k2=12.07064, k3=0.46118, k4=23.09650, k5=19.82645, k6=2.83820, p1=0.00009973, p2=0.0000001966`
  - `distortionModel=4`
  - `rotationMatrix` / `translationMatrix` 是 **depth → color** 外参：
    - rotation ≈ `[0.9944149, -0.0002258, 0.0006361; 0.0001574, 0.9944151, 0.1055395; -0.0006564, -0.1055393, 0.9944149]`
    - translation ≈ `[-32.3196, -1.3198, 1.9251]` **mm**
- 颜色/深度 profile 的 `rotationMatrix`/`translationMatrix` 对彩色是 0，对深度是 D2C。不要搞反方向。

#### `OBProperty`

```text
header,
uint32 propertyId,       // definition 写的是 uint8，但序列化器实际用 uint32
uint8[] data,            // uint32 len + bytes
uint32 datasize
```

样本中 47 条 property，包含：

- 常规设置：曝光、增益、白平衡、镜像、翻转、旋转、自动曝光等。
- `id=4024`：D2C 对齐支持分辨率列表（528 B）。
- `id=4027`：对齐标定参数（644 B）。开头是深度内参 `504.579, 504.583, 530.478, 525.674`，宽高字段为 `1024×1024`，随后是畸变和 model=4。可作为 K4A 深度标定的来源之一。
- `id=4036`：IMU 标定参数（2929 B）。
- `id=1038` 等：多设备同步配置。
- `655xx` / `675xx` 这类高 id 是“属性响应/范围”而非普通设置，原样保留即可。

ID 定义在（上游 Orbbec SDK）：

- `OrbbecSDK_v2-main/include/libobsensor/h/Property.h`
- `OrbbecSDK_v2-main/src/device/component/property/InternalProperty.hpp`（4024/4027/4036 在这里）

### 3.5 话题到传感器的映射

- `sensor_N/frameType_M` 中的 `M` 是 `OBFrameType`：
  - 1=IR，2=COLOR，3=DEPTH，4=ACCEL，7=GYRO，8=IR_LEFT，9=IR_RIGHT，10=RAW_PHASE，11=CONFIDENCE，12=LIDAR，13/14=COLOR_LEFT/RIGHT。
- `streamProfileType_N` 中的 `N` 是 `OBStreamType`：
  - 1=IR，2=COLOR，3=DEPTH，4=ACCEL，5=GYRO，6/7=IR_LEFT/RIGHT，10=LIDAR。
- 样本：`sensor_2/frameType_2` = 彩色；`sensor_3/frameType_3` = 深度。
- 未来遇到 IR/IMU 时按 enum 扩展，不要硬编码“只有两个流”。

参考（上游 Orbbec SDK）：
- `OrbbecSDK_v2-main/src/media/ros/RosbagWriter.cpp`
- `OrbbecSDK_v2-main/src/media/ros/RosbagReader.cpp`
- `OrbbecSDK_v2-main/include/libobsensor/h/ObTypes.h`

---

## 4. 输出格式：K4A 兼容 `.mkv`

> 原始结构参考样本 `20260829_..._CL8T75400SZ_gait.mkv` 已从工作区删除；下面保留它的实测值，
> 仅用于格式对照，不要再把它的 calibration/mode 当成当前 bag 的输出。

样本 `20260829_…_gait.mkv` 是 **Azure Kinect 风格** 的 K4A 录制，不是普通视频 MKV。输出应尽量复刻它的结构。

### 4.1 结构

- EBML / Matroska；`DocTypeVersion=2`。
- `Info`：
  - `TimestampScale=1000`（1000 ns = **1 µs**，不是 1 ms）
  - `Title="Azure Kinect"`
  - `MuxingApp="libmatroska-1.4.9"`
  - `WritingApp="k4arecord-1.4.0-private"`（如果复用 K4A writer）
  - `Duration`（µs）
- `Tracks`：
  - `COLOR`：默认 `V_MS/VFW/FOURCC`，`CodecPrivate` 为 40 B `BITMAPINFOHEADER`，`biCompression='BGRA'`，`biBitCount=32`，`biSizeImage=width*height*4`，tag `K4A_COLOR_MODE=BGRA_1080P`。这是 kinect-analyzer 能播放的格式。
  - `ORIGINAL_COLOR_MJPG`：自定义视频轨，`V_MS/VFW/FOURCC` + `MJPG` BITMAPINFOHEADER，逐帧保存 bag 的原始 JPEG 字节。默认存在；`--no-original-color` 可去掉。
  - `DEPTH`：同样 codec，`biCompression='b16g'`，`biBitCount=16`，`biSizeImage=width*height*2`。
  - `IR`：同 DEPTH，但轨道名 `IR`，`K4A_IR_MODE=ACTIVE`。
  - `DefaultDuration=33333333` ns（30 fps）。
- `Attachments`：
  - `calibration.json`（`application/octet-stream`）
  - 可选：本项目的 `orbbec_manifest.json`、`orbbec_frames.jsonl`、原始标定 blob。
- `Tags`（样本）：
  - `K4A_COLOR_TRACK=<uid>`、`K4A_COLOR_MODE=BGRA_1080P`（默认）/`MJPG_1080P`（`--color mjpg`）/`BGRA_720P`（样本 MKV）
  - `K4A_DEPTH_TRACK=<uid>`、`K4A_DEPTH_MODE=WFOV_2X2BINNED`（样本）/`NFOV_UNBINNED`（640×576）
  - `K4A_IR_TRACK=<uid>`、`K4A_IR_MODE=ACTIVE`
  - `K4A_DEPTH_DELAY_NS`、`K4A_WIRED_SYNC_MODE`、`K4A_SUBORDINATE_DELAY_NS`
  - `K4A_COLOR_FIRMWARE_VERSION`、`K4A_DEPTH_FIRMWARE_VERSION`、`K4A_DEVICE_SERIAL_NUMBER`
  - `K4A_CALIBRATION_FILE=calibration.json`
  - `K4A_START_OFFSET_NS=<首帧设备时间 ns>`
- `Cluster` / `SimpleBlock`：
  - 每个 block 头：track number vint、int16 相对时间码（**µs**）、flags `0x80`。
  - 深度/IR 帧数据是 big-endian。
  - 样本每帧一个 SimpleBlock；cluster 时间戳按帧时间递增。

样本 MKV 的具体值：

- 每轨 273 个非空 block，16.260216 s；实际帧间隔不规则（约 16.8 fps 平均），但 `DefaultDuration` 仍是 33.333 ms。
- COLOR：1280×720，BGRA，3,686,400 B/帧。
- DEPTH：512×512，b16g，524,288 B/帧。
- IR：512×512，b16g，524,288 B/帧。
- `K4A_START_OFFSET_NS=1787990393357464000`。
- 设备序列号 `CL8T75400SZ`，`K4A_DEPTH_MODE=WFOV_2X2BINNED`，`K4A_COLOR_MODE=BGRA_720P`。
- 注意：样本 MKV 与样本 bag **不是同一次录制，也不是同一台设备**（不同 serial、不同分辨率/模式）。只能拿它对照结构，不能拿它的 calibration 给 bag 用。

### 4.2 本 bag 的目标模式映射

| bag 流 | K4A 配置 |
|---|---|
| 彩色 1920×1080 `mjpg`（默认） | 解码为 BGRA 1920×1080 → `color_format=K4A_IMAGE_FORMAT_COLOR_BGRA32`，`color_resolution=K4A_COLOR_RESOLUTION_1080P`，tag `BGRA_1080P`；原始 JPEG 另存 `ORIGINAL_COLOR_MJPG` |
| 彩色 1920×1080 `mjpg`（`--color mjpg`） | `color_format=K4A_IMAGE_FORMAT_COLOR_MJPG`，tag `MJPG_1080P`；小体积，但 BGRA-only 播放器不可用 |
| 深度 640×576 `mono16` | `depth_mode=K4A_DEPTH_MODE_NFOV_UNBINNED`，tag `NFOV_UNBINNED`，640×576 |
| 无 IR | K4A writer 会自动建 IR 轨；有 IR 帧时写 IR，没有则空轨。已支持 IR 往返。 |
| 未来有 IMU | 需 K4A IMU 轨 `S_K4A/IMU` + `matroska_imu_sample_t`；样本无 IMU，先不做 |

默认 BGRA 输出约 12.53 GB（45 s）；`--color mjpg` 约 1.20 GB；`--no-original-color` 约 12.3 GB。

### 4.3 深度字节序

- bag：`mono16`，little-endian。
- K4A `b16g`：big-endian。
- 只做 `uint16` 字节交换：

```cpp
uint16_t be = (v >> 8) | (v << 8);
```

- 这是双射，不损失数值；验证时换回来必须逐值相等。
- 文件层面是 BE；实现上本工具把 LE buffer 交给 K4A writer，由它在写 block 时 swap，见 §4.5。
- 不要自作主张改成 `Y16 ` / little-endian；K4A playback 会按 `b16g` 解析。

### 4.4 K4A 标定 `calibration.json`

K4A playback 会读取 attachment `calibration.json`，格式大致为：

```json
{
  "CalibrationInformation": {
    "Cameras": [
      {
        "Location": "CALIBRATION_CameraLocationD0",
        "Purpose": "CALIBRATION_CameraPurposeDepth",
        "SensorWidth": 1024,
        "SensorHeight": 1024,
        "MetricRadius": 0,
        "Intrinsics": {
          "ModelParameterCount": 14,
          "ModelParameters": [cx, cy, fx, fy, k1, k2, k3, k4, k5, k6, codx, cody, p2, p1],
          "ModelType": "CALIBRATION_LensDistortionModelBrownConrady"
        },
        "Rt": {
          "Rotation": [1,0,0, 0,1,0, 0,0,1],
          "Translation": [0,0,0]
        }
      },
      {
        "Location": "CALIBRATION_CameraLocationPV0",
        "Purpose": "CALIBRATION_CameraPurposePhotoVideo",
        "SensorWidth": 4096,
        "SensorHeight": 3072,
        "Intrinsics": { "...": "..." },
        "Rt": { "Rotation": [ ... ], "Translation": [ ... ] }
      }
    ],
    "InertialSensors": [ {"SensorType": "CALIBRATION_InertialSensorType_Gyro", "...": "..."},
                         {"SensorType": "CALIBRATION_InertialSensorType_Accelerometer", "...": "..."} ],
    "Metadata": { "SerialId": "...", "DeviceName": "...", "Version": { "Major": 1, "Minor": 2 } }
  }
}
```

已知规则（来自 Orbbec K4A Wrapper 源码，已在本项目验证）：

- **参数顺序是 `cx, cy, fx, fy, k1..k6, codx, cody, p2, p1`**（14 个），不是 `fx, fy, cx, cy`。K4A 的 `k4a_calibration_intrinsic_parameters_t.param` 也是这个顺序。写错会导致 fx/cx 互换。
- K4A 模式转换把原始标定当作 **unitized、0-cornered**：
  - `fx_px = fx_norm * bin_w`
  - `fy_px = fy_norm * bin_h`
  - `cx_px = cx_norm * bin_w - crop_x - 0.5`
  - `cy_px = cy_norm * bin_h - crop_y - 0.5`
- 因此合成时用：
  - `fx_norm = profile.fx / bin_w`
  - `fy_norm = profile.fy / bin_h`
  - `cx_norm = (profile.cx + 0.5 + crop_x) / bin_w`
  - `cy_norm = (profile.cy + 0.5 + crop_y) / bin_h`
  - 畸变顺序：`k1..k6` 原样，`codx=0, cody=0`，`p2=bag.p2`，`p1=bag.p1`。
- 模式参数（K4A Wrapper `mode_specific_calibration.c`）：
  - 深度 raw sensor 必须是 **1024×1024**。
  - `NFOV_UNBINNED`: bin `1024×1024`, crop `(192,180)`, out `640×576`。
  - `NFOV_2X2BINNED`: bin `512×512`, crop `(96,90)`, out `320×288`。
  - `WFOV_2X2BINNED`: bin `512×512`, crop `(0,0)`, out `512×512`。
  - `WFOV_UNBINNED`: bin `1024×1024`, crop `(0,0)`, out `1024×1024`。
  - 彩色 raw sensor 用 **4096×3072**（4:3）；`1080p`: bin `1920×1440`, crop `(0,180)`, out `1920×1080`。
  - `720p`: bin `1280×960`, crop `(0,120)`；`1440p`: `2560×1920`, crop `(0,240)`；`1536p`: `2048×1536`, crop 0；`2160p`: `3840×2880`, crop `(0,360)`；`3072p`: `4096×3072`, crop 0。
- `Rt.Translation` 在 JSON 里是 **米**，parser 会 ×1000 转成 mm。bag 的 D2C 是 mm，写入 JSON 前要 ÷1000。
- `Location` 必须是 `CALIBRATION_CameraLocationD0`（深度）和 `CALIBRATION_CameraLocationPV0`（彩色）。
- K4A parser 要求 `InertialSensors` 至少有 gyro 和 accel 两个 entry，即使没有 IMU 数据；本项目使用零占位，并在 manifest 中标注 `IMU calibration unavailable`。如果未来支持 IMU，必须用真实标定替换，不能沿用零占位。
- 本项目已验证：`k4a_playback_get_calibration` 读出的 depth/color 内参与 bag profile 完全一致（见 §1.2）。畸变和 D2C 外参尚未做逐点验证。
- 标定来源优先级：
  1. 用户显式提供的 `--calibration-json`（来自同设备/同模式的 K4A 录制）。
  2. 从 bag 的 `OBStreamProfileInfo` + `4027` 深度标定 + D2C 外参合成。
  3. 不写 calibration，只写 tracks；K4A playback 对“标定缺失”只告警，仍能打开并返回帧（已烟测），但 `k4a_playback_get_calibration` 不可用。必须打 warning，并把原始标定 blob 放进 attachment。
- 无论标定是否合成成功，都把 `4024`、`4027`、`4036` 原始字节和 profile JSON 放进 attachment，确保信息可追溯。

参考：

- K4A Wrapper `src/calibration/calibration.c`
- K4A Wrapper `src/record/internal/matroska_read.cpp`（`parse_recording_config`）
- 样本 MKV 的 `calibration.json`

### 4.5 推荐写入方式（已验证）

仓库自带 `azurekinectsdk/` 是 Orbbec K4A Wrapper 1.4.0-private，**可以在没有设备的情况下创建 K4A 录制**：

```cpp
k4a_device_configuration_t cfg = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
cfg.color_format = K4A_IMAGE_FORMAT_COLOR_BGRA32;   // 旧版播放器兼容；新版也支持 MJPG
cfg.color_resolution = K4A_COLOR_RESOLUTION_1080P;
cfg.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;
cfg.camera_fps = K4A_FRAMES_PER_SECOND_30;
cfg.synchronized_images_only = true;
cfg.wired_sync_mode = K4A_WIRED_SYNC_MODE_STANDALONE;

k4a_record_t rec = nullptr;
k4a_record_create(path, /*device=*/nullptr, cfg, &rec);   // 关键：device 可以为 NULL

// 原始 MJPEG 源轨道（默认；--no-original-color 可不加）
uint8_t mjpg_header[40] = {};
// BITMAPINFOHEADER: biSize=40, width, height, planes=1, bitCount=24, 'MJPG'
k4a_record_video_settings_t settings = {1920, 1080, 30};
k4a_record_add_custom_video_track(rec, "ORIGINAL_COLOR_MJPG", "V_MS/VFW/FOURCC",
                                  mjpg_header, sizeof(mjpg_header), &settings);

k4a_record_add_attachment(rec, "calibration.json", calib, calib_size);
k4a_record_add_tag(rec, "K4A_DEVICE_SERIAL_NUMBER", sn);
k4a_record_write_header(rec);

// 每帧：
k4a_image_t color = nullptr, depth = nullptr;
k4a_image_create_from_buffer(K4A_IMAGE_FORMAT_COLOR_BGRA32, 1920, 1080, 1920 * 4,
                             bgra_data, 1920 * 1080 * 4, nullptr, nullptr, &color);
k4a_image_set_device_timestamp_usec(color, timestamp_usec);
k4a_image_create_from_buffer(K4A_IMAGE_FORMAT_DEPTH16, 640, 576, 1280,
                             depth_le_data, 737280, nullptr, nullptr, &depth);
k4a_image_set_device_timestamp_usec(depth, timestamp_usec);
k4a_capture_t cap = nullptr;
k4a_capture_create(&cap);
k4a_capture_set_color_image(cap, color);
k4a_capture_set_depth_image(cap, depth);
// 注意：此 wrapper 的 set_*_image 会接管 image 所有权；不要再 k4a_image_release
k4a_record_write_capture(rec, cap);
k4a_capture_release(cap);
// 原始 JPEG 必须逐帧写进自定义轨道
k4a_record_write_custom_track_data(rec, "ORIGINAL_COLOR_MJPG", timestamp_usec,
                                   jpeg_data, jpeg_size);

k4a_record_flush(rec);
k4a_record_close(rec);
```

已经验证的事实：

- `k4a_record_create(..., nullptr, ...)` 成功；会生成 COLOR/DEPTH/IR 三轨。
- `BGRA32 + 1080P` 是合法组合：`k4a_playback_get_record_configuration` 返回 `color_format=BGRA32`、`color_resolution=1080P`；playback 返回 1920×1080、8,294,400 B 的 BGRA。
- 深度自动从 LE 转 BE，playback 返回 `DEPTH16`、640×576。
- 彩色 playback 返回 BGRA（`K4A_IMAGE_FORMAT_COLOR_BGRA32`），1920×1080；`--color mjpg` 时才返回 MJPG。
- 空 IR 轨不影响 `k4a_playback_get_next_capture`；返回的 IR image 为 NULL。
- `K4A_START_OFFSET_NS` 自动取第一帧设备时间；`Duration` 自动按 **µs** 写入。
- 自定义 `ORIGINAL_COLOR_MJPG` 轨不干扰 `k4a_playback_get_next_capture`；可用 `k4a_playback_get_next_data_block` 逐帧读回并校验原始 JPEG SHA-256。
- 用 `k4a_playback_open` + `k4a_playback_get_next_capture` 已完整回放真实样本 2715 帧（1366 color + 1349 depth），并校验 1366 帧原始 MJPEG。
- **不要手动把深度换成 BE 再传给 `k4a_image_create_from_buffer`**：`k4a_record_write_capture` 会自己把 `DEPTH16` 小端转成文件里的 `b16g` 大端。手动换一次会导致 playback 返回 BE，hash 不匹配。

**所有权坑（重要）**：在这个 K4A wrapper 里，`k4a_capture_set_color_image` / `set_depth_image` 之后不能再 `k4a_image_release`，否则 release capture 时会 double free（烟测表现为 `0xC0000005`）。让 capture 接管 image，写完 capture 后只 release capture。

如果不想依赖预编译 DLL，可选的第二条路：

- vendor `OrbbecSDK-K4A-Wrapper` 的 `src/record/internal/matroska_write.cpp`（MIT）和 `record.cpp` 的建轨/建 tag 逻辑，直接调用 libmatroska 写同样的结构。
- 代价：要带 libmatroska/libebml（LGPL-2.1），并自己处理 IOCallback、Cues、Duration、Segment size。

参考链接：

- <https://github.com/orbbec/OrbbecSDK-K4A-Wrapper>
- `src/record/internal/matroska_write.cpp`
- `src/record/sdk/record.cpp`
- `src/record/internal/matroska_read.cpp`

### 4.6 `mkv2bag` 反向格式（v0.4.3，已验证）

`mkv2bag` 把 K4A MKV 写回 `#ROSBAG V2.0`，实现位于 `src/bag_writer.*`、`src/mkv2bag.*`、`src/json.*`：

- 读取 `orbbec_manifest.json`（device/streams/properties）和 `orbbec_frames.jsonl`（每帧 header/metadata）。
- 若存在 `ORIGINAL_COLOR_MJPG` 自定义轨道，逐帧取原始 JPEG 字节写入 `sensor_msgs/Image.data`，不重新编码。
- 没有源轨道时：COLOR 是 MJPG 则复制码流；COLOR 是 BGRA 则写 `encoding=bgra8`（raw，不做有损编码）。
- DEPTH 和 IR 写 `encoding=mono16`；K4A playback 返回 LE，bag 也写 LE。
- 输出 chunk 使用 LZ4 frame；chunk record 是 **header + 4-byte compressed_size + compressed data**（没有外层 data-length，和 Orbbec writer 一致）。
- `index_pos` 处先写 connection 记录，再写 chunk-info；没有 index-data 记录（与原样本一致）。`BagReader` 只扫描到 `index_pos`，chunk-info 供其他读取器使用。
- 帧时间戳、header seq/frame_id、number、system/global 时间、metadata、pixel_bit_size 从 frames JSONL 恢复；缺失时用 K4A image 时间戳/默认值。
- **不要**把 chunk 写成普通 record 的 `header + data_len + data`：会多一个 4 字节，`BagReader` 会把 chunk 内的 4 字节 compressed_size 当作 record header，报 `chunk record missing op`。
- 已实测：样本 bag → MJPG MKV → mkv2bag（797,152,740 B，LZ4）→ MKV；再用原始 bag 做 `verify`，1366 color / 1349 depth 全通过。
- 已实测：默认 BGRA + `ORIGINAL_COLOR_MJPG` MKV 反向；`verify: ORIGINAL_COLOR_MJPG OK (1366 frames)`。
- 已实测：无 manifest 的普通 K4A MKV（BGRA + IR）→ bag → MKV；`verify: OK (1158 color, 1158 depth, 1158 IR)`。
- 限制：没有 manifest 时无法恢复原始 profile/property/connection definition；`calibration.json` 不参与反向重建（profile 来自 manifest）。

---

## 5. 代码结构（已实现）

```text
CMakeLists.txt
src/
  main.cpp              // CLI: help / inspect / convert / verify / batch / mkv2bag / --self-test
  converter.hpp/.cpp    // 共享核心：扫描、转换、验证、进度/取消/日志
  errors.hpp            // CancelledError
  gui_win32.cpp         // Windows 原生 GUI（bag2mkv-gui）
  bag.hpp / bag.cpp     // 容器、chunk、LZ4、连接/消息解析
  bag_writer.hpp/.cpp   // 反向：LZ4 ROSBAG writer（chunk/connection/chunk-info）
  mkv2bag.hpp/.cpp      // 反向：K4A playback + manifest → bag
  json.hpp/.cpp         // manifest/JSONL 的小型 JSON DOM
  scan.hpp / scan.cpp   // Pass 1: 索引、profile/property/device、逐帧 hash
  lz4_util.hpp/.cpp     // LZ4 frame 解压/压缩
  bz2_util.hpp/.cpp     // BZ2 chunk 解压（vendored bzip2）
  k4a_writer.hpp/.cpp   // K4A writer 封装（BGRA/MJPG + IR + ORIGINAL_COLOR_MJPG）
  jpeg_decoder.hpp/.cpp // Windows WIC JPEG → BGRA（默认彩色输出）
  manifest.hpp/.cpp     // manifest/JSONL/calibration 合成
  util.hpp/.cpp         // SHA-256、base64、JSON escape、BE swap
third_party/lz4/        // vendored LZ4
third_party/bzip2/      // vendored bzip2 1.0.8（BSD-like）
tests/
  run_sample.ps1        // 样本转换 + verify 集成脚本（-Color bgra|mjpg）
  run_sample.cmd        // 绕过 PowerShell 执行策略的包装
  README.md
```

构建目标：`bag2mkv_core`（静态库）+ `bag2mkv`（CLI）+ `bag2mkv-gui`（WIN32 GUI）。
两者必须共用 `converter.cpp`，不要在 GUI 里复制转换逻辑。

### 5.1 两遍扫描（已实现）

1. **Pass 1（索引）**
   - 顺序扫描所有 chunk；记录每帧的 `chunk_offset`、`payload_offset`、`timestamp_usec`、`number`、`metadata`、`pixel_bit_size`。
   - 同时收集 connection、deviceInfo、stream profiles、全部 property。
   - 计算每帧 payload SHA-256；生成 `orbbec_manifest.json` 和 `orbbec_frames.jsonl`。
   - 按 `timestamp_usec` 稳定排序（bag 内记录顺序不保证严格时间序）。
2. **Pass 2（写入）**
   - 按排序后的帧顺序，用 `chunk_offset` 重读/解压对应 chunk，按 `payload_offset` 取原始 payload。
   - 默认彩色：WIC 解码 JPEG → BGRA → `write_color_bgra()`；同时 `write_original_color_mjpg()` 写原始 JPEG。
   - `--color mjpg`：JPEG 直接 `write_color_mjpg()`，不解码。
   - 深度传 **LE** buffer，由 K4A writer 转 BE。
   - 写 `<output>.tmp`，flush/close 后 rename。

### 5.2 数据所有权

- `ScanResult.frames` 只保存元数据和 chunk/payload 偏移，不保存图像像素；内存占用小。
- Pass 2 用单 chunk cache + 单帧 BGRA 缓冲；WIC 每帧解码一次，内存约 8 MB。
- 写文件前会按帧数/彩色模式估算输出大小并检查输出盘可用空间；空间不足在写入前报错，不会先删除旧输出。
- K4A image/capture 的所有权按 §4.5 处理：set 后不再 release image，只 release capture。

### 5.3 并发

- 第一版单线程即可：样本 800 MB 解压约 2 s；默认 BGRA 输出 12.5 GB，主要受磁盘和 JPEG 解码限制。
- 默认 BGRA 模式每帧 sleep 10 ms 限速；`--color mjpg` 每帧 2 ms。
- 如需优化，只做“下一 chunk 预解压”或“写线程”，不要并行改同一帧缓冲。

---

## 6. 构建、运行、依赖

### 6.1 当前机器事实

- Windows，Visual Studio 2022 17.14，MSVC 19.44，Windows SDK 10.0.26100。
- CMake 可用；Python 3.11.9 可用。
- **没有** `ffmpeg` / `ffprobe` / `mkvmerge` 在 PATH 上；不要把外部 ffmpeg 当作运行时前提。
- `azurekinectsdk/bin` 有 `k4a.dll`、`k4arecord.dll`、`k4aviewer.exe`、`k4arecorder.exe`、`OrbbecSDK.dll`。
- `azurekinectsdk/lib` 有 `k4a.lib`、`k4arecord.lib`；头文件在 `include/k4a`、`include/k4arecord`。
- 上游 Orbbec SDK 源码未随本仓库提交；本工具不必链接整个 OrbbecSDK，除非选择复用其 rosbag 读取器。

### 6.2 构建/运行（已可用）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CMake 的 post-build 会把 `azurekinectsdk/bin/*.dll` 复制到 `build/Release/`，因此通常不需要手动改 PATH。

```powershell
.\build\Release\bag2mkv.exe --self-test
.\build\Release\bag2mkv.exe --help
.\build\Release\bag2mkv.exe help convert
.\build\Release\bag2mkv.exe inspect .\20251111_143747_001_cam1_CL8R35300SH_gait.bag
.\build\Release\bag2mkv.exe convert .\20251111_143747_001_cam1_CL8R35300SH_gait.bag -o .\out.mkv --force
.\build\Release\bag2mkv.exe verify .\20251111_143747_001_cam1_CL8R35300SH_gait.bag .\out.mkv
.\build\Release\bag2mkv.exe batch .\recordings --output-dir .\out\mkv --color mjpg --verify
.\build\Release\bag2mkv-gui.exe    # GUI；可传 [input-dir] [output-dir] [--mjpg] [--start]
.\build\Release\bag2mkv-gui.exe --batch <input-dir> --batch-output <output-dir> [--mjpg]
.\build\Release\bag2mkv-gui.exe --to-bag <input.mkv> [--to-bag-output <output.bag>]
.\tests\run_sample.cmd         # 默认 BGRA + 源轨，转换 + verify；删除 12.5 GB 输出
.\tests\run_sample.cmd -Color mjpg   # 小体积 MJPG-only 测试
```

GUI 的“输入目录/输出目录”默认递归扫描，点击“开始转换”后自动判断 bag→mkv 或 mkv→bag；输入目录里同时有 bag 和 mkv 会报错，输出目录不允许在输入目录内部；嵌套子目录会按相对路径镜像。CLI `batch` 仍只处理 bag；`mkv2bag` 是单文件。

如果执行策略拦截 `.ps1`，用 `tests\run_sample.cmd`（内部带 `-ExecutionPolicy Bypass`），或：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\run_sample.ps1
```

GUI 是 Windows 原生 Win32（Common Controls），不依赖 Qt；`bag2mkv-gui` 是 `WIN32` 子系统，
没有控制台窗口。GUI 的进度/取消通过 `converter` 的 `ProgressCallback` 实现，取消时
`convert_bag()` 抛 `CancelledError` 并删除 `.tmp`。

### 6.3 依赖建议

- C++17。
- LZ4：优先用 Orbbec SDK 里的 `roslz4`（BSD）或系统 `liblz4`；不要自己实现 LZ4。
- BZ2：仓库已 vendor `third_party/bzip2`（BSD-like），静态链接；不要依赖系统 DLL。
- K4A 写入：第一版用 `azurekinectsdk` 的 `k4arecord.lib` + DLL（已烟测）；如要纯源码，再 vendor K4A Wrapper 的 matroska writer。
- JSON：可用 nlohmann/json（MIT）或 cJSON；calibration/manifest 都要写 JSON。
- 压缩 manifest：zlib/gzip 可选；不压缩也能接受（几百 KB）。
- 测试：CTest + 一个轻量断言库即可；不要引入大型测试框架。

---

## 7. 验证（Definition of Done）

一个转换不能只看“文件能打开”。必须通过以下检查。

### 7.1 结构检查

对样本 bag 的期望：

- 连接数 6；chunk 数 1318。
- 彩色帧数 1366；深度帧数 1349。
- 首帧时间约 `1762843067.505110`（彩色）/ `1762843067.506975`（深度）。
- 末帧时间约 `1762843112.426022`（彩色）/ `1762843112.438858`（深度）。
- 输出轨道：`COLOR`、`DEPTH`、`IR`（IR 可为空），默认还有 `ORIGINAL_COLOR_MJPG`。
- `K4A_COLOR_MODE=BGRA_1080P`（默认）/`MJPG_1080P`（`--color mjpg`）；`K4A_DEPTH_MODE=NFOV_UNBINNED`。
- `K4A_START_OFFSET_NS=1762843067505110000`（若以彩色首帧为起点；以实际归并首帧为准）。
- `TimestampScale=1000`（1 µs）；`Duration` 和 block 相对时间码都是 µs。
- 默认彩色轨 CodecPrivate：`BGRA`、32 bpp、`biSizeImage=1920*1080*4`；深度轨：`b16g`、16 bpp、`biSizeImage=640*576*2`。
- `calibration.json` attachment 存在，且 `k4a_playback_get_calibration` 能读出：
  - depth `640x576 fx=504.579 fy=504.583 cx=338.478 cy=345.674`
  - color `1920x1080 fx=1118.69 fy=1118.11 cx=963.672 cy=535.888`

### 7.2 逐帧无损检查

- 默认彩色：把 bag 的 JPEG 用同一 WIC 解码器解成 BGRA，与 `k4a_image_get_buffer()` 逐像素/逐行比较；同时校验 `ORIGINAL_COLOR_MJPG` 每帧 JPEG 的 SHA-256 == bag `data[]`。
- `--color mjpg`：MKV 中每个 COLOR block 的 JPEG 字节 SHA-256 == bag 对应帧 `data[]`。
- 深度：MKV 中 DEPTH block 读出的 BE 数据，换回 LE 后与 bag `data[]` 逐字节相等；或逐 `uint16` 相等。
- 帧数：每个输入帧恰好对应一个输出 block，不重不漏。
- 时间戳：`k4a_image_get_device_timestamp_usec` == bag 的 `timestamp_usec`。K4A 时间基是 **µs**，实测 2715 帧全部精确相等；容差最多 1 µs。
- 元数据：manifest 中每帧的 `number`、`metadata`、`pixel_bit_size`、`header.seq/frame_id` 与 bag 一致。

### 7.3 K4A playback 检查

用 `k4a_playback_open` + `k4a_playback_get_next_capture`：

- 能打开，`k4a_playback_get_recording_length_usec` 合理。
- 捕获数 > 0；首帧彩色/深度时间戳与输入一致。
- 默认彩色 image 格式 == `K4A_IMAGE_FORMAT_COLOR_BGRA32`，1920×1080，buffer 为 BGRA；`--color mjpg` 时 == `K4A_IMAGE_FORMAT_COLOR_MJPG`。
- 深度 image 格式 == `K4A_IMAGE_FORMAT_DEPTH16`，640×576，buffer 值正确（LE）。
- 空 IR 轨返回 NULL；有 IR 帧时写 `IR16`，`verify` 会逐帧校验 IR。不导致 playback 失败。
- `ORIGINAL_COLOR_MJPG` 源轨可用 `k4a_playback_get_next_data_block` 逐帧读回。

### 7.4 失败路径

- 截断 bag：报错并退出，不产生有效输出。
- 不支持的编码（例如未知 `encoding`）：明确报错，不要静默转码。
- 缺 calibration：warning + 仍写出 tracks；`verify` 标注“calibration 缺失”。
- 输出已存在：默认拒绝；`--force` 才覆盖。
- 磁盘满/写失败：删除临时文件，返回非 0。

---

## 8. 陷阱清单（按踩坑概率排序）

1. **不要用标准 ROS bag 解析器**。连接记录拆成两段、message payload 不在 `data` 字段、LZ4 frame 前有 compressed_size。
2. **chunk header 的 `size` 不可信**。用 4 字节 compressed_size 定位下一 chunk；解压后自己校验记录是否恰好用完。
3. **`sensor_msgs/Image` 有隐藏字段** `depth_units` 和 `pixel_bit_size`；message definition 里没有。
4. **`OBProperty.propertyId` 是 uint32**，不是 definition 写的 uint8。
5. **bag 里的彩色是 MJPG，不是 raw**。`step=1920` 对 JPEG 没有行宽意义；不要按 `step*height` 校验 data 大小。输出默认解码成 BGRA，但原始 JPEG 另存 `ORIGINAL_COLOR_MJPG`。
6. **深度在文件里必须 BE**。K4A `b16g` 是 big-endian；但传给 `k4a_record_write_capture` 的 `DEPTH16` image 必须是 **LE**，K4A writer 内部会 swap。自己再 swap 一次会导致 playback 返回 BE、hash 不匹配。
7. **K4A 的 `K4A_COLOR_MODE` 默认是 `BGRA_1080P`**，不是 `1080P`；`--color mjpg` 才是 `MJPG_1080P`。`K4A_DEPTH_MODE` 是 `NFOV_UNBINNED`。tag 值写错 playback 会拒绝。
8. **K4A tag 名必须 ALL CAPS**，只允许 `A-Z 0-9 - _`；`validate_name_characters` 会拒绝其他字符。
9. **attachment/tag 必须在 `k4a_record_write_header` 之前加**；header 之后加会失败。
10. **K4A image/capture 所有权**：`k4a_capture_set_*_image` 后不要 release image；只 release capture。烟测 double free 是 `0xC0000005`。
11. **不要伪造 IR**。样本 bag 没有 IR；空 IR 轨可以，但不要拿深度/彩色冒充 IR。
12. **样本 MKV 不是样本 bag 的配对输出**。不同 serial、不同 mode；不要复制它的 calibration 或 mode tag。
13. **时间戳精度**：K4A 文件是 **1 µs**（`TimestampScale=1000` ns）；manifest 保留原始 µs，verify 实测可精确到 µs。
14. **帧率不是严格 30 fps**。样本 MKV 273 帧/16.26 s，间隔不规则；不要用“帧数 = 时长 × 30”做断言。
15. **大文件**：bag ~800 MB，默认 BGRA 输出 ~12.5 GB，`--color mjpg` ~1.2 GB；所有 offset/size 用 64-bit，不要 `int`。
16. **不要改输入 chunk 做 BE swap**；K4A writer 已经处理，输入只读。
17. **不要依赖 ffmpeg/ffprobe**。当前机器没有；测试用 K4A playback API 或内置 EBML 解析。
18. **标定参数顺序是 `cx,cy,fx,fy,k1..k6,codx,cody,p2,p1`**；K4A 还会按 mode 的 `bin_w/bin_h/crop` 做 unitized→pixel 转换。不要按 `fx,fy,cx,cy` 的顺序写。
19. **`Rt.Translation` 单位是米**；bag 的 D2C 是毫米。
20. **不要提交样本/构建产物**。仓库已有 `.gitignore` 忽略 `*.bag`/`*.mkv`/`build/`/`out/`。
21. **kinect-analyzer 2026-10-18 起支持 MJPG 彩色**（Qt `qjpeg` 逐显示帧解码）；旧版只支持 BGRA。新版本不要再用旧结论否定 MJPG 输出。

---

## 9. Agent 工作约定

- 改代码前先读本文件第 1–4 节；格式假设变更时同步更新本文件。
- 不要修改 `OrbbecSDK_v2-main/`、`azurekinectsdk/`、`azurekinectbodytrackingsdk/` 里的第三方文件；需要改行为时在本项目代码里做适配。
- 不要修改/删除测试 bag（`20251111_...CL8R35300SH_gait.bag`）；需要实验数据时复制到 `_scratch/`，并在提交前删除。结构参考 MKV 已移除，不要依赖它存在。
- 实验脚本、临时 MKV、`_scratch/`、`build/`、`out/` 不要提交。
- 新增依赖前先说明理由、许可证、Windows 构建方式；优先复用仓库已有的 K4A/Orbbec 组件。
- 每个 PR/提交只做一件事；格式修复和功能开发分开。
- 转换逻辑必须有测试；只改 CLI/日志的提交也要跑 `ctest`。
- 如果发现本文件的格式描述与样本不符，**以实测为准**，并在本文件记录“实测值 + 解析方法 + 样本”。
- 不要为了让输出“看起来像视频”而改变码流；本项目的价值在于可验证的无损。

### 9.1 尚未解决的问题（按优先级）

1. 标定的 **畸变系数 / D2C 外参** 还没有做逐点投影验证；内参已验证完全一致。需要和 K4A `k4a_calibration_3d_to_2d` 或实际点云对照。
2. IMU 标定使用零占位（K4A parser 要求 gyro/accel entry）；当前版本不支持 IMU 流。未来支持 IMU 时必须用真实标定。
3. IR 流已支持（`mono16` → K4A `IR16`），但没有独立 IR profile 的畸变验证；样本 bag 本身无 IR，需用其他设备样本继续验证。
4. BZ2 chunk 已支持：vendored bzip2 1.0.8，`scan()` 和 `read_chunk()` 都用 `BZ2_bzDecompress`；LZ4 / 未压缩仍保留。真实 Orbbec BZ2 样本尚未拿到，已用合成 BZ2 bag 做过 inspect/convert/verify。
5. K4A 写入依赖 `azurekinectsdk` 的预编译 DLL；如要完全源码化，需要 vendor K4A Wrapper 的 matroska writer。
6. `K4A_CALIBRATION_FILE` tag 目前是无 target 的全局 tag；playback 按文件名 `calibration.json` 回退成功，但严格 target UID 未设置。
7. 写线程磁盘队列较慢时 K4A 仍会打印 “Disk write speed is too low”；`k4a_set_debug_message_handler` 过滤不到这条内部日志。BGRA 模式每帧 sleep 10 ms、MJPG 2 ms 限速；慢磁盘/大文件仍可能看到该日志。
8. 默认输出是 raw 深度 + BGRA 彩色（~12.5 GB/45 s），没有可选的 FFV1 等无损压缩容器；BGRA 体积是旧播放器兼容的代价。
9. GUI 当前仅 Windows Win32；主界面已简化为“输入目录 + 输出目录 + 开始转换”，自动识别方向并递归镜像子目录。没有安装包、没有多语言、没有可编辑队列；`--start`、`--mjpg`、`--batch`、`--batch-output`、`--to-bag`、`--to-bag-output` 主要用于预填和冒烟测试。

---

## 10. 参考

仓库内（构建依赖）：

- `azurekinectsdk/include/k4a/k4a.h`
- `azurekinectsdk/include/k4arecord/record.h`
- `azurekinectsdk/include/k4arecord/playback.h`

上游参考（`OrbbecSDK_v2-main/`，未随本仓库提交）：

- `OrbbecSDK_v2-main/src/media/ros/RosbagWriter.cpp`
- `OrbbecSDK_v2-main/src/media/ros/RosbagReader.cpp`
- `OrbbecSDK_v2-main/src/media/ros/RosFileFormat.hpp`
- `OrbbecSDK_v2-main/3rdparty/rosbag/src/msgs/sensor_msgs/Image.h`
- `OrbbecSDK_v2-main/3rdparty/rosbag/src/rosbag_storage/src/bag.cpp`
- `OrbbecSDK_v2-main/3rdparty/rosbag/src/rosbag_storage/src/lz4_stream.cpp`
- `OrbbecSDK_v2-main/3rdparty/rosbag/src/roslz4/src/lz4s.c`
- `OrbbecSDK_v2-main/src/device/component/property/InternalProperty.hpp`

外部：

- Orbbec K4A Wrapper：<https://github.com/orbbec/OrbbecSDK-K4A-Wrapper>
- K4A Wrapper matroska writer：`src/record/internal/matroska_write.cpp`
- K4A Wrapper matroska reader：`src/record/internal/matroska_read.cpp`
- K4A Wrapper calibration：`src/calibration/calibration.c`
- Azure Kinect 录制文件格式：<https://learn.microsoft.com/en-us/azure/kinect-dk/record-file-format>

---

**一句话总结**：默认把 JPEG 解码成 BGRA 1080p 写入 COLOR（兼容旧播放器；新版 kinect-analyzer 也支持 MJPG），原始 JPEG 原样保存在 `ORIGINAL_COLOR_MJPG`；深度/IR 16-bit 只做 LE→BE；所有时间戳/帧号/metadata/profile/属性/标定都进 K4A MKV + attachment；用 `k4a_record_create(device=NULL)` 写，用 `k4a_playback_*` 验。`--color mjpg` 可退回 1.2 GB 的纯 MJPEG 输出；`mkv2bag` 可从 MJPG/BGRA/IR MKV 反向重建 LZ4 ROSBAG。

