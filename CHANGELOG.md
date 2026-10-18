# Changelog

## 0.4.1

- GUI 增加“MKV 转 bag...”按钮：选择 K4A MKV 和输出 `.bag`，复用 `mkv_to_bag`，支持日志、取消和输出目录。
- GUI 新增启动参数 `--to-bag <input.mkv>` / `--to-bag-output <output.bag>`，便于快速测试和“打开方式”。
- `mkv_to_bag` 增加进度/取消回调；取消时删除 `.tmp`，不会留下半成品。
- 已用 GUI 路径完成一次样本反向转换，输出 797,152,740 B，`inspect` 和 `verify` 均通过。

## 0.4.0

- 新增 `mkv2bag`：把 K4A MKV 转回 Orbbec ROSBAG V2.0。
- 反向转换优先使用 `orbbec_manifest.json` / `orbbec_frames.jsonl` 恢复设备、profile、property、帧号/时间戳/metadata。
- 优先复制 `ORIGINAL_COLOR_MJPG` 的原始 JPEG 字节；没有源轨道时支持 MJPG 原码流或 BGRA raw（`bgra8`）。
- 深度/IR 以 `mono16` 原始数据写回；新增 IR 输出支持，K4A MKV 的 IR 轨道可往返。
- 输出 LZ4 压缩的 bag chunk，并写 connection/chunk-info；样本反向输出约 797 MB。
- 无 manifest 的普通 K4A MKV 也能转换图像（从 tag 取设备序列号；无原始 profile/property）。
- `--help` 增加 `mkv2bag`，CTest 增加对应帮助测试。

## 0.3.1

- 修复 `batch` 对目录只扫描顶层、嵌套目录中 0 个 `.bag` 时报 `no .bag inputs found` 的问题。
- 目录输入现在默认递归扫描；CLI 可用 `--no-recursive` 恢复只扫顶层。
- 新增 CLI `batch --limit <n>`，便于先试跑或分批处理大型目录。
- 批量输入排序后再执行，`--limit` 行为可复现。
- 新增批量级空间预检：按输入 bag 总大小和彩色模式估算输出总量，空间明显不足时在开始扫描/转换前就停止。
- GUI“批量目录...”同样递归扫描，并在开始前提示/阻止空间不足的批次。
- README、AGENTS、`batch --help` 更新递归、`--limit` 和空间预检说明。

## 0.3.0

- 新增 CLI `batch` 命令：支持多个 `.bag` 或目录，输出到 `--output-dir`，每个输入生成同名 `.mkv`。
- 新增 CLI `batch --continue-on-error`，批量失败后可继续处理剩余文件。
- GUI 新增“批量目录...”按钮，选择输入/输出目录后批量转换；单个文件失败不中断其余文件。
- GUI 新增 `--batch <input-dir> --batch-output <output-dir>` 启动参数，便于脚本和冒烟测试。
- 新增输出空间预检：按帧数/彩色模式估算输出大小并检查磁盘剩余空间；不足时在写入前报错。
- 修复 `--force` 覆盖时可能先删除旧输出、再发现空间不足的问题；现在先预检，通过后才删除旧文件。
- 版本号改为 CMake `PROJECT_VERSION` 单一来源，避免 CLI/GUI/文档版本不一致。
- GUI 标题显示版本号；README/AGENTS 更新批量模式、预检和版本说明。

## 0.2.0

- 新增 Windows GUI（`bag2mkv-gui`），与 CLI 共用 `bag2mkv_core`。
- GUI 支持输入/输出选择、拖拽、彩色模式、原始 MJPEG 轨道、自动验证、覆盖确认、进度、取消、日志和打开输出目录。
- 默认 BGRA 1080p + `ORIGINAL_COLOR_MJPG`，兼容 kinect-analyzer；`--color mjpg` 提供小体积模式。
- 新增 `--help` / `help convert` / `help verify` / `--version` / `--self-test`。
- 新增独立 `verify`，逐帧校验 BGRA 像素、深度字节序、时间戳和原始 JPEG SHA-256。

## 0.1.0

- 首个可用版本：Orbbec `#ROSBAG V2.0` → K4A 兼容 MKV。
- 支持 LZ4/未压缩 chunk、COLOR/DEPTH、标定 JSON、manifest/JSONL attachment。
