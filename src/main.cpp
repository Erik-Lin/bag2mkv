#include "converter.hpp"
#include "errors.hpp"
#include "mkv2bag.hpp"
#include "ros1_export.hpp"
#include "ros1_input.hpp"
#include "ros2_export.hpp"
#include "ros2_input.hpp"
#include "scan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace bag2mkv;

void print_overview() {
    std::cout << "bag2mkv " << version() << " - Orbbec RGB-D .bag to Azure Kinect .mkv converter\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv <command> [options]\n"
              << "  bag2mkv help <command>\n"
              << "  bag2mkv --version\n"
              << "  bag2mkv --self-test\n"
              << "\n"
              << "COMMANDS\n"
              << "  inspect <input.bag>              Show device, streams, properties, frame counts and timestamps.\n"
              << "  convert <input.bag> -o <out.mkv>  Convert a bag to a K4A-compatible MKV.\n"
              << "  batch <files-or-dir...> --output-dir <dir>\n"
              << "                                    Convert many bags, one MKV per input.\n"
              << "  mkv2bag <input.mkv> [-o output.bag]\n"
              << "                                    Rebuild an Orbbec ROSBAG V2.0 from a K4A MKV.\n"
              << "  ros1 <input.bag> -o <output.bag>\n"
              << "                                    Export an Orbbec bag as a standard ROS1 bag.\n"
              << "  ros1mkv <input.bag> -o <out.mkv>  Convert a standard ROS1 bag to K4A MKV.\n"
              << "  ros2 <input.bag> -o <output-dir>  Export an Orbbec bag as a ROS2 sqlite3 bag.\n"
              << "  ros2mkv <bag-dir> -o <out.mkv>    Convert a ROS2 sqlite3 bag to K4A MKV.\n"
              << "  mkv2ros1 <input.mkv> -o <out.bag> Convert a K4A MKV to standard ROS1 bag.\n"
              << "  mkv2ros2 <input.mkv> -o <out-dir> Convert a K4A MKV to ROS2 sqlite3 bag.\n"
              << "  verify  <input.bag> <out.mkv>     Independently verify frames, timestamps and calibration.\n"
              << "  help [command]                    Show this help, or detailed help for one command.\n"
              << "\n"
              << "GUI\n"
              << "  Run bag2mkv-gui.exe for the Windows GUI. It uses the same converter and supports\n"
              << "  drag-and-drop, a batch-folder button, progress, cancel and an embedded log.\n"
              << "\n"
              << "QUICK START\n"
              << "  bag2mkv inspect capture.bag\n"
              << "  bag2mkv convert capture.bag -o capture.mkv --verify\n"
              << "  bag2mkv batch .\\recordings --output-dir .\\mkv --verify\n"
              << "  bag2mkv mkv2bag capture.mkv -o capture.bag\n"
              << "  bag2mkv verify capture.bag capture.mkv\n"
              << "\n"
              << "DEFAULT OUTPUT\n"
              << "  Color is BGRA 1080p (decoded from the original MJPEG, compatible with\n"
              << "  kinect-analyzer). The exact original JPEG bytes are preserved in the\n"
              << "  ORIGINAL_COLOR_MJPG custom track. This is large: about 12.5 GB / 45 s.\n"
              << "  Use --color mjpg for a ~1.2 GB MJPEG-only file, or --no-original-color\n"
              << "  to omit the source track (about 12.3 GB).\n"
              << "\n"
              << "  Run 'bag2mkv help convert' for every option and size/calibration details.\n";
}

void print_convert_help() {
    std::cout << "bag2mkv convert " << version() << " - convert an Orbbec bag to a K4A-compatible MKV\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv convert <input.bag> -o <output.mkv> [options]\n"
              << "\n"
              << "REQUIRED\n"
              << "  <input.bag>              Orbbec SDK v2 / #ROSBAG V2.0 recording.\n"
              << "  -o, --output <file>      Output .mkv path. Parent directories are created.\n"
              << "\n"
              << "OUTPUT OPTIONS\n"
              << "  --color bgra|mjpg        Color format. Default: bgra.\n"
              << "                             bgra = decode JPEG to BGRA 1080p for players such as\n"
              << "                                    kinect-analyzer; original JPEG is also kept.\n"
              << "                             mjpg = copy the original MJPEG bitstream only (~1.2 GB),\n"
              << "                                    but BGRA-only players cannot display it.\n"
              << "  --no-original-color      In BGRA mode, omit the ORIGINAL_COLOR_MJPG source track.\n"
              << "                             Output is about 12.3 GB; original JPEG bytes are not kept.\n"
              << "  --force                  Overwrite an existing output file. Without it, an existing\n"
              << "                             output is an error.\n"
              << "\n"
              << "CALIBRATION OPTIONS\n"
              << "  --calibration-json <f>   Use a calibration.json from the same device/mode instead of\n"
              << "                             synthesizing it from the bag's stream profiles.\n"
              << "  --no-calibration         Do not write calibration.json (not recommended; K4A playback\n"
              << "                             can still read images but get_calibration may fail).\n"
              << "\n"
              << "VERIFICATION OPTIONS\n"
              << "  --verify                 After conversion, run the full independent verifier:\n"
              << "                             color pixels/JPEG hashes, depth values, timestamps,\n"
              << "                             frame counts and the original MJPEG source track.\n"
              << "\n"
              << "OTHER\n"
              << "  -h, --help               Show this help and exit.\n"
              << "  --version                Show version and exit.\n"
              << "\n"
              << "LOSSLESS CONTRACT\n"
              << "  - Original MJPEG bytes are copied verbatim to ORIGINAL_COLOR_MJPG.\n"
              << "  - BGRA is a deterministic decode of the same JPEG; no lossy re-encode.\n"
              << "  - Depth is kept as 16-bit values; only LE (memory) -> BE (b16g file) byte order.\n"
              << "  - No filtering, alignment, resizing or unit conversion.\n"
              << "\n"
              << "OUTPUT SIZE (sample 45 s, 1920x1080 MJPG + 640x576 depth)\n"
              << "  default (BGRA + source track)       ~12.5 GB\n"
              << "  --no-original-color                 ~12.3 GB\n"
              << "  --color mjpg                        ~1.2 GB\n"
              << "\n"
              << "EXAMPLES\n"
              << "  bag2mkv convert gait.bag -o gait.mkv --verify\n"
              << "  bag2mkv convert gait.bag -o gait_bgra.mkv --color bgra --no-original-color\n"
              << "  bag2mkv convert gait.bag -o gait_mjpg.mkv --color mjpg --force\n"
              << "  bag2mkv convert gait.bag -o gait.mkv --calibration-json device_calib.json\n"
              << "\n"
              << "EXIT CODES\n"
              << "  0  success (and verification succeeded when --verify was used)\n"
              << "  1  runtime/conversion/verification error\n"
              << "  2  usage error\n"
              << "  3  cancelled by the user\n";
}

void print_batch_help() {
    std::cout << "bag2mkv batch " << version() << " - convert many bags into one output directory\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv batch <file-or-directory...> --output-dir <dir> [options]\n"
              << "\n"
              << "Each input .bag produces <output-dir>/<input-stem>.mkv. Directory inputs are\n"
              << "scanned recursively by default; duplicate output names are an error.\n"
              << "\n"
              << "OPTIONS\n"
              << "  --output-dir, -o <dir>   Required. Created if it does not exist.\n"
              << "  --color bgra|mjpg        Same as convert. Default: bgra.\n"
              << "  --no-original-color      Omit the ORIGINAL_COLOR_MJPG source track.\n"
              << "  --force                  Overwrite existing MKVs.\n"
              << "  --no-calibration         Do not write calibration.json.\n"
              << "  --calibration-json <f>   Use one calibration for all inputs (same device/mode only).\n"
              << "  --verify                 Verify each MKV after conversion.\n"
              << "  --continue-on-error      Keep converting remaining files after a failure.\n"
              << "  --no-recursive           Only scan the top level of directory inputs.\n"
              << "  --limit <n>              Convert at most n inputs (after sorting), useful for testing.\n"
              << "  -h, --help               Show this help and exit.\n"
              << "\n"
              << "EXAMPLES\n"
              << "  bag2mkv batch .\\recordings --output-dir .\\mkv --verify\n"
              << "  bag2mkv batch a.bag b.bag --output-dir .\\mkv --color mjpg --continue-on-error\n"
              << "\n"
              << "EXIT CODES\n"
              << "  0 all conversions succeeded; 1 at least one failed; 2 usage error; 3 cancelled.\n";
}

void print_mkv2bag_help() {
    std::cout << "bag2mkv mkv2bag " << version() << " - rebuild an Orbbec ROSBAG V2.0 from a K4A MKV\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv mkv2bag <input.mkv> [--output <output.bag>] [--force]\n"
              << "\n"
              << "The reverse path reads orbbec_manifest.json, orbbec_frames.jsonl,\n"
              << "ORIGINAL_COLOR_MJPG and the K4A COLOR/DEPTH/IR tracks. It writes LZ4-compressed\n"
              << "ROSBAG V2.0 chunks and a chunk-info index. When the source track is present the\n"
              << "original JPEG bytes are copied unchanged; otherwise the current COLOR track is\n"
              << "written as MJPG or raw BGRA.\n"
              << "\n"
              << "OPTIONS\n"
              << "  -o, --output <file>   Output bag. Default: input file with .bag extension.\n"
              << "  --force               Overwrite an existing output bag.\n"
              << "  -h, --help            Show this help and exit.\n"
              << "\n"
              << "EXAMPLES\n"
              << "  bag2mkv mkv2bag capture.mkv\n"
              << "  bag2mkv mkv2bag capture.mkv -o capture_roundtrip.bag --force\n"
              << "\n"
              << "EXIT CODES\n"
              << "  0 success; 1 conversion failure; 2 usage error.\n";
}

void print_ros1_help() {
    std::cout << "bag2mkv ros1 " << version() << " - export an Orbbec bag as a standard ROS1 bag\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv ros1 <input.bag> -o <output.bag> [--force]\n"
              << "\n"
              << "Color MJPEG is written as sensor_msgs/CompressedImage (format=jpeg), keeping the\n"
              << "original JPEG bytes. Raw color, depth and IR are written as sensor_msgs/Image;\n"
              << "depth/IR use encoding 16UC1 (little-endian). The output includes standard ROS1\n"
              << "connection records, chunk index data and chunk-info records.\n"
              << "\n"
              << "Topic names\n"
              << "  /camera/color/image_raw/compressed  color MJPEG\n"
              << "  /camera/color/image_raw             raw color\n"
              << "  /camera/depth/image_raw             depth\n"
              << "  /camera/ir/image_raw                IR\n"
              << "\n"
              << "OPTIONS\n"
              << "  -o, --output <file>   Output ROS1 bag.\n"
              << "  --force               Overwrite an existing output bag.\n"
              << "  -h, --help            Show this help and exit.\n"
              << "\n"
              << "EXAMPLES\n"
              << "  bag2mkv ros1 capture.bag -o capture_ros1.bag\n"
              << "  bag2mkv ros1 capture.bag -o capture_ros1.bag --force\n"
              << "\n"
              << "This command reads an Orbbec ROSBAG V2.0. It does not read standard ROS1 input yet.\n";
}

void print_ros1mkv_help() {
    std::cout << "bag2mkv ros1mkv " << version() << " - convert a standard ROS1 bag to a K4A MKV\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv ros1mkv <input.bag> -o <output.mkv> [--color bgra|mjpg] [--force]\n"
              << "\n"
              << "Reads standard ROS1 sensor_msgs/Image and sensor_msgs/CompressedImage messages.\n"
              << "Color supports rgb8, bgr8, bgra8, mono8 and JPEG CompressedImage; depth/IR support\n"
              << "16UC1/mono16 (IR also accepts 8UC1 and expands to 16-bit).\n"
              << "\n"
              << "K4A only supports specific color/depth resolutions. The input must use one of:\n"
              << "  color: 1920x1080, 3840x2160, 2560x1440, 2048x1536, 4096x3072, 1280x720\n"
              << "  depth/IR: 640x576, 320x288, 512x512, 1024x1024\n"
              << "Other resolutions are rejected rather than silently resized.\n"
              << "\n"
              << "OPTIONS\n"
              << "  -o, --output <file>   Output K4A MKV.\n"
              << "  --color bgra|mjpg     bgra (default) decodes CompressedImage to BGRA;\n"
              << "                        mjpg copies JPEG CompressedImage bytes unchanged.\n"
              << "  --force               Overwrite an existing output MKV.\n"
              << "  -h, --help            Show this help and exit.\n"
              << "\n"
              << "EXAMPLES\n"
              << "  bag2mkv ros1mkv capture_ros1.bag -o capture.mkv\n"
              << "  bag2mkv ros1mkv capture_ros1.bag -o capture.mkv --color mjpg --force\n";
}

void print_ros2_help() {
    std::cout << "bag2mkv ros2 " << version() << " - export an Orbbec bag as a ROS2 sqlite3 bag\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv ros2 <input.bag> -o <output-dir> [--force]\n"
              << "\n"
              << "Creates a ROS2 bag2 directory containing metadata.yaml and bag_0.db3.\n"
              << "Messages use CDR serialization:\n"
              << "  sensor_msgs/msg/CompressedImage  color JPEG (original bytes preserved)\n"
              << "  sensor_msgs/msg/Image            raw color, depth (16UC1) and IR\n"
              << "\n"
              << "Topics\n"
              << "  /camera/color/image_raw/compressed\n"
              << "  /camera/color/image_raw\n"
              << "  /camera/depth/image_raw\n"
              << "  /camera/ir/image_raw\n"
              << "\n"
              << "OPTIONS\n"
              << "  -o, --output <dir>   Output ROS2 bag directory.\n"
              << "  --force              Overwrite metadata.yaml/bag_0.db3.\n"
              << "  -h, --help           Show this help and exit.\n";
}

void print_ros2mkv_help() {
    std::cout << "bag2mkv ros2mkv " << version() << " - convert a ROS2 sqlite3 bag to a K4A MKV\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv ros2mkv <bag-dir> -o <output.mkv> [--color bgra|mjpg] [--force]\n"
              << "\n"
              << "Reads metadata.yaml + *.db3 from a ROS2 bag2 directory. Sensor messages use CDR:\n"
              << "  sensor_msgs/msg/Image            rgb8/bgr8/bgra8/mono8, depth/IR 16UC1\n"
              << "  sensor_msgs/msg/CompressedImage  jpeg\n"
              << "\n"
              << "K4A resolution limits are the same as ros1mkv:\n"
              << "  color: 1920x1080, 1280x720, 3840x2160, 2560x1440, 2048x1536, 4096x3072\n"
              << "  depth/IR: 640x576, 320x288, 512x512, 1024x1024\n"
              << "Other dimensions are rejected rather than resized.\n"
              << "\n"
              << "OPTIONS\n"
              << "  -o, --output <file>   Output K4A MKV.\n"
              << "  --color bgra|mjpg     bgra (default) decodes JPEG to BGRA; mjpg copies JPEG.\n"
              << "  --force               Overwrite an existing output MKV.\n"
              << "  -h, --help            Show this help and exit.\n";
}

void print_mkv2ros1_help() {
    std::cout << "bag2mkv mkv2ros1 " << version() << " - convert a K4A MKV to a standard ROS1 bag\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv mkv2ros1 <input.mkv> -o <output.bag> [--force]\n"
              << "\n"
              << "The K4A MKV is first rebuilt as an Orbbec bag, then exported as standard ROS1.\n"
              << "Original MJPEG (ORIGINAL_COLOR_MJPG) is preserved as CompressedImage/jpeg.\n"
              << "Depth and IR are written as Image/16UC1.\n"
              << "\n"
              << "OPTIONS\n"
              << "  -o, --output <file>   Output ROS1 bag.\n"
              << "  --force               Overwrite existing output.\n"
              << "  -h, --help            Show this help and exit.\n";
}

void print_mkv2ros2_help() {
    std::cout << "bag2mkv mkv2ros2 " << version() << " - convert a K4A MKV to a ROS2 sqlite3 bag\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv mkv2ros2 <input.mkv> -o <output-dir> [--force]\n"
              << "\n"
              << "The K4A MKV is first rebuilt as an Orbbec bag, then exported as ROS2 bag2\n"
              << "(metadata.yaml + bag_0.db3, CDR messages).\n"
              << "\n"
              << "OPTIONS\n"
              << "  -o, --output <dir>   Output ROS2 bag directory.\n"
              << "  --force              Overwrite metadata.yaml/bag_0.db3.\n"
              << "  -h, --help           Show this help and exit.\n";
}

void print_inspect_help() {
    std::cout << "bag2mkv inspect " << version() << "\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv inspect <input.bag>\n"
              << "\n"
              << "Prints the container summary (chunks/connections), device info, stream profiles\n"
              << "(intrinsics/distortion), frame counts, timestamps and all raw properties. This is a\n"
              << "read-only operation; it does not write an MKV.\n";
}

void print_verify_help() {
    std::cout << "bag2mkv verify " << version() << "\n"
              << "\n"
              << "USAGE\n"
              << "  bag2mkv verify <input.bag> <output.mkv>\n"
              << "\n"
              << "Independently rescans the bag and compares it with the MKV through K4A playback:\n"
              << "  - BGRA color pixels (or JPEG hashes in --color mjpg files) and source JPEG hashes;\n"
              << "  - depth payload hashes after BE->LE normalization;\n"
              << "  - every device timestamp and frame count;\n"
              << "  - serial number and calibration readability.\n"
              << "Exit code is 0 only if every check passes.\n";
}

void print_scan(const ScanResult &scan) {
    std::cout << "bag: " << scan.source_path << "\n";
    std::cout << "size: " << scan.source_size << " bytes\n";
    std::cout << "chunks: " << scan.chunk_count << " (declared " << scan.declared_chunk_count << ")\n";
    std::cout << "index_pos: " << scan.index_pos << "\n";
    std::cout << "connections: " << scan.connections.size() << " (declared " << scan.declared_conn_count << ")\n";
    for (const auto &kv : scan.connections) {
        const auto &c = kv.second;
        std::cout << "  [" << c.id << "] " << c.topic << "  " << c.type << "\n";
    }
    if (scan.device.present) {
        std::cout << "device: " << scan.device.name << " / " << scan.device.full_name << "\n";
        std::cout << "  sn=" << scan.device.sn << " fw=" << scan.device.fw_version << " hw=" << scan.device.hw_version
                  << " connection=" << scan.device.connection_type << "\n";
        std::cout << "  vid=" << scan.device.vid << " pid=" << scan.device.pid << " uid=" << scan.device.uid << "\n";
    } else {
        std::cout << "device: (no deviceInfo message)\n";
    }
    std::cout << "streams:\n";
    for (const auto &kv : scan.profiles) {
        const auto &p = kv.second;
        std::cout << "  " << stream_kind(p.stream_type) << " stream_type=" << int(p.stream_type)
                  << " format=" << int(p.format) << " " << p.width << "x" << p.height << "@" << p.fps
                  << " fx=" << p.intrinsic[0] << " fy=" << p.intrinsic[1] << " cx=" << p.intrinsic[2]
                  << " cy=" << p.intrinsic[3] << " distortion_model=" << int(p.distortion_model) << "\n";
    }
    std::cout << "frames: " << scan.frames.size() << "\n";
    for (const auto &kv : scan.frame_counts) {
        std::cout << "  " << kv.first << ": " << kv.second << "\n";
    }
    if (scan.first_timestamp_usec) {
        std::cout << "time: " << scan.first_timestamp_usec << " .. " << scan.last_timestamp_usec << " usec ("
                  << double(scan.last_timestamp_usec - scan.first_timestamp_usec) / 1e6 << " s)\n";
    }
    std::cout << "properties: " << scan.properties.size() << "\n";
    for (const auto &p : scan.properties) {
        std::cout << "  id=" << p.id << " size=" << p.data.size() << " datasize=" << p.datasize << "\n";
    }
}

struct CliProgress {
    bool operator()(const ProgressInfo &info) {
        if (info.stage == Stage::Writing || info.stage == Stage::Verifying) {
            uint64_t total = info.total ? info.total : 1;
            unsigned pct = unsigned((info.current * 100) / total);
            if (pct != last_pct || info.current == info.total) {
                std::fprintf(stderr, "\r  %s %llu/%llu (%u%%)", info.stage == Stage::Writing ? "writing" : "verifying",
                             (unsigned long long)info.current, (unsigned long long)info.total, pct);
                std::fflush(stderr);
                last_pct = pct;
            }
            if (info.current == info.total) {
                std::fputc('\n', stderr);
            }
        } else if (info.stage == Stage::Scanning) {
            if (info.current == 0) {
                std::fprintf(stderr, "  scanning bag...\n");
            }
        } else if (info.stage == Stage::Preparing && info.current == 0) {
            std::fprintf(stderr, "  preparing metadata...\n");
        } else if (info.stage == Stage::Finalizing && info.current == 0) {
            std::fprintf(stderr, "  finalizing...\n");
        }
        return true;
    }

    unsigned last_pct = 101;
};

std::string format_size(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

int cmd_batch(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_batch_help();
            return 0;
        }
    }
    ConvertOptions base;
    std::string output_dir;
    std::vector<std::string> inputs;
    bool continue_on_error = false;
    bool recursive = true;
    size_t limit = 0;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--output-dir" || arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << arg << "\n";
                return 2;
            }
            output_dir = argv[++i];
        } else if (arg == "--force") {
            base.force = true;
        } else if (arg == "--no-calibration") {
            base.no_calibration = true;
        } else if (arg == "--no-original-color") {
            base.preserve_source_color = false;
        } else if (arg == "--verify") {
            base.verify_after = true;
        } else if (arg == "--continue-on-error") {
            continue_on_error = true;
        } else if (arg == "--recursive") {
            recursive = true;
        } else if (arg == "--no-recursive") {
            recursive = false;
        } else if (arg == "--limit") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --limit\n";
                return 2;
            }
            try {
                limit = static_cast<size_t>(std::stoull(argv[++i]));
            } catch (const std::exception &) {
                std::cerr << "error: --limit requires a positive integer\n";
                return 2;
            }
            if (limit == 0) {
                std::cerr << "error: --limit must be greater than zero\n";
                return 2;
            }
        } else if (arg == "--color") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --color\n";
                return 2;
            }
            std::string value = argv[++i];
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (value == "bgra") {
                base.color_output = ColorOutput::Bgra;
            } else if (value == "mjpg" || value == "mjpeg") {
                base.color_output = ColorOutput::Mjpg;
            } else {
                std::cerr << "error: --color must be bgra or mjpg\n";
                return 2;
            }
        } else if (arg == "--calibration-json") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --calibration-json\n";
                return 2;
            }
            base.calibration_json_path = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for batch: " << arg << "\n";
            return 2;
        } else {
            inputs.push_back(arg);
        }
    }
    if (inputs.empty()) {
        std::cerr << "error: batch requires at least one input bag or directory\n\n";
        print_batch_help();
        return 2;
    }
    if (output_dir.empty()) {
        std::cerr << "error: batch requires --output-dir <dir>\n\n";
        print_batch_help();
        return 2;
    }

    std::vector<std::string> expanded;
    for (const auto &input : inputs) {
        std::error_code ec;
        if (fs::is_directory(input, ec)) {
            if (recursive) {
                fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
                const fs::recursive_directory_iterator end;
                for (; it != end; it.increment(ec)) {
                    if (ec) {
                        std::cerr << "error: cannot scan directory " << input << ": " << ec.message() << "\n";
                        return 1;
                    }
                    if (!it->is_regular_file()) continue;
                    std::string ext = it->path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
                    if (ext == ".bag") expanded.push_back(it->path().string());
                }
            } else {
                for (const auto &entry : fs::directory_iterator(input, ec)) {
                    if (!entry.is_regular_file()) continue;
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
                    if (ext == ".bag") expanded.push_back(entry.path().string());
                }
            }
            if (ec) {
                std::cerr << "error: cannot scan directory " << input << ": " << ec.message() << "\n";
                return 1;
            }
        } else {
            expanded.push_back(input);
        }
    }
    if (expanded.empty()) {
        std::cerr << "error: no .bag inputs found\n";
        return 1;
    }
    std::sort(expanded.begin(), expanded.end());
    if (limit > 0 && expanded.size() > limit) {
        std::cout << "limit: using first " << limit << " of " << expanded.size() << " inputs\n";
        expanded.resize(limit);
    }

    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) {
        std::cerr << "error: cannot create output directory " << output_dir << ": " << ec.message() << "\n";
        return 1;
    }

    // Rough batch-level guard before scanning every bag. The per-file preflight
    // in convert_bag still performs the exact check.
    std::set<std::string> planned_outputs;
    for (const auto &input : expanded) {
        fs::path output = fs::path(output_dir) / (fs::path(input).stem().string() + ".mkv");
        std::string key = output.string();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (!planned_outputs.insert(key).second) {
            std::cerr << "error: duplicate output name: " << output.string() << "\n";
            return 1;
        }
    }

    uint64_t input_bytes = 0;
    uint64_t existing_bytes = 0;
    bool input_sizes_ok = true;
    for (const auto &input : expanded) {
        std::error_code size_ec;
        uint64_t size = fs::file_size(input, size_ec);
        if (size_ec) {
            input_sizes_ok = false;
            break;
        }
        input_bytes += size;
        fs::path output = fs::path(output_dir) / (fs::path(input).stem().string() + ".mkv");
        size = fs::file_size(output, size_ec);
        if (!size_ec) existing_bytes += size;
    }
    if (input_sizes_ok && input_bytes > 0) {
        const double factor = base.color_output == ColorOutput::Bgra ? 16.0 : 1.7;
        const uint64_t estimate = uint64_t(double(input_bytes) * factor) + 64ull * 1024ull * 1024ull;
        std::error_code space_ec;
        fs::space_info space = fs::space(output_dir, space_ec);
        if (!space_ec && space.available > 0) {
            const uint64_t available = space.available + existing_bytes;
            std::cout << "batch estimate: ~" << format_size(estimate) << " for " << expanded.size()
                      << " input(s); free: " << format_size(available) << "\n";
            if (estimate > available) {
                std::cerr << "error: not enough free space for this batch; use --limit, --color mjpg, "
                             "or a larger output volume\n";
                return 1;
            }
        }
    }

    std::set<std::string> used_outputs;
    uint64_t succeeded = 0;
    uint64_t failed = 0;
    const uint64_t total = expanded.size();
    for (size_t i = 0; i < expanded.size(); ++i) {
        const std::string &input = expanded[i];
        const std::string prefix = "[" + std::to_string(i + 1) + "/" + std::to_string(total) + "] ";
        fs::path input_path(input);
        fs::path output_path = fs::path(output_dir) / (input_path.stem().string() + ".mkv");
        std::string output = output_path.string();
        std::string output_key = output;
        std::transform(output_key.begin(), output_key.end(), output_key.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (!used_outputs.insert(output_key).second) {
            std::cerr << prefix << "error: duplicate output name: " << output << "\n";
            ++failed;
            if (!continue_on_error) break;
            continue;
        }

        std::cout << prefix << input << " -> " << output << "\n";
        ConvertOptions options = base;
        options.input_bag = input;
        options.output_mkv = output;
        CliProgress progress;
        try {
            convert_bag(
                options, [&progress](const ProgressInfo &info) { return progress(info); },
                [&prefix](const std::string &line) { std::cout << prefix << line << "\n"; });
            ++succeeded;
            std::cout << prefix << "OK\n";
        } catch (const CancelledError &) {
            std::cerr << "cancelled\n";
            return 3;
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << prefix << "error: " << e.what() << "\n";
            if (!continue_on_error) break;
        }
    }

    std::cout << "batch: " << succeeded << " succeeded, " << failed << " failed, " << total << " total\n";
    return failed == 0 ? 0 : 1;
}

int cmd_mkv2bag(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_mkv2bag_help();
            return 0;
        }
    }

    Mkv2BagOptions options;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << arg << "\n";
                return 2;
            }
            options.output_bag = argv[++i];
        } else if (arg == "--force") {
            options.force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for mkv2bag: " << arg << "\n";
            return 2;
        } else if (options.input_mkv.empty()) {
            options.input_mkv = arg;
        } else {
            std::cerr << "error: mkv2bag accepts one input MKV\n";
            return 2;
        }
    }
    if (options.input_mkv.empty()) {
        std::cerr << "error: mkv2bag requires an input MKV\n\n";
        print_mkv2bag_help();
        return 2;
    }
    if (options.output_bag.empty()) {
        std::filesystem::path output(options.input_mkv);
        output.replace_extension(".bag");
        options.output_bag = output.string();
    }

    try {
        Mkv2BagSummary summary = mkv_to_bag(options, [](const std::string &line) {
            std::cout << line << "\n";
        });
        std::cout << "mkv2bag: " << summary.color_frames << " color, " << summary.depth_frames
                  << " depth, " << summary.ir_frames << " IR, " << summary.properties << " properties, "
                  << summary.profiles << " profiles\n";
        std::cout << "output: " << options.output_bag << " (" << summary.bag_size << " bytes)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_ros1(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_ros1_help();
            return 0;
        }
    }
    Ros1ExportOptions options;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << arg << "\n";
                return 2;
            }
            options.output_bag = argv[++i];
        } else if (arg == "--force") {
            options.force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for ros1: " << arg << "\n";
            return 2;
        } else if (options.input_bag.empty()) {
            options.input_bag = arg;
        } else {
            std::cerr << "error: ros1 accepts one input bag\n";
            return 2;
        }
    }
    if (options.input_bag.empty()) {
        std::cerr << "error: ros1 requires an input bag\n\n";
        print_ros1_help();
        return 2;
    }
    if (options.output_bag.empty()) {
        fs::path out(options.input_bag);
        out.replace_extension(".ros1.bag");
        options.output_bag = out.string();
    }
    try {
        Ros1ExportSummary summary = export_ob_bag_to_ros1(options, [](const std::string &line) {
            std::cout << line << "\n";
        });
        std::cout << "ros1: " << summary.color_frames << " color, " << summary.depth_frames << " depth, "
                  << summary.ir_frames << " IR\n";
        std::cout << "output: " << options.output_bag << " (" << summary.bag_size << " bytes)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_ros1mkv(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_ros1mkv_help();
            return 0;
        }
    }
    Ros1InputOptions options;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << arg << "\n";
                return 2;
            }
            options.output_mkv = argv[++i];
        } else if (arg == "--color") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --color\n";
                return 2;
            }
            std::string value = argv[++i];
            if (value == "bgra") options.color_mjpg = false;
            else if (value == "mjpg" || value == "mjpeg") options.color_mjpg = true;
            else {
                std::cerr << "error: --color must be bgra or mjpg\n";
                return 2;
            }
        } else if (arg == "--force") {
            options.force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for ros1mkv: " << arg << "\n";
            return 2;
        } else if (options.input_bag.empty()) {
            options.input_bag = arg;
        } else {
            std::cerr << "error: ros1mkv accepts one input bag\n";
            return 2;
        }
    }
    if (options.input_bag.empty()) {
        std::cerr << "error: ros1mkv requires an input bag\n\n";
        print_ros1mkv_help();
        return 2;
    }
    if (options.output_mkv.empty()) {
        fs::path out(options.input_bag);
        out.replace_extension(".mkv");
        options.output_mkv = out.string();
    }
    try {
        Ros1InputSummary summary = convert_ros1_to_mkv(options, [](const std::string &line) {
            std::cout << line << "\n";
        });
        std::cout << "ros1mkv: " << summary.color_frames << " color, " << summary.depth_frames << " depth, "
                  << summary.ir_frames << " IR\n";
        std::cout << "output: " << options.output_mkv << " (" << summary.mkv_size << " bytes)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_ros2(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_ros2_help();
            return 0;
        }
    }
    Ros2ExportOptions options;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << arg << "\n";
                return 2;
            }
            options.output_dir = argv[++i];
        } else if (arg == "--force") {
            options.force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for ros2: " << arg << "\n";
            return 2;
        } else if (options.input_bag.empty()) {
            options.input_bag = arg;
        } else {
            std::cerr << "error: ros2 accepts one input bag\n";
            return 2;
        }
    }
    if (options.input_bag.empty()) {
        std::cerr << "error: ros2 requires an input bag\n\n";
        print_ros2_help();
        return 2;
    }
    if (options.output_dir.empty()) {
        fs::path out(options.input_bag);
        out.replace_extension("");
        options.output_dir = (out.string() + "_ros2");
    }
    try {
        Ros2ExportSummary summary = export_ob_bag_to_ros2(options, [](const std::string &line) {
            std::cout << line << "\n";
        });
        std::cout << "ros2: " << summary.color_frames << " color, " << summary.depth_frames << " depth, "
                  << summary.ir_frames << " IR, " << summary.message_count << " messages\n";
        std::cout << "output: " << options.output_dir << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_ros2mkv(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_ros2mkv_help();
            return 0;
        }
    }
    Ros2InputOptions options;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << arg << "\n";
                return 2;
            }
            options.output_mkv = argv[++i];
        } else if (arg == "--color") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --color\n";
                return 2;
            }
            std::string value = argv[++i];
            if (value == "bgra") options.color_mjpg = false;
            else if (value == "mjpg" || value == "mjpeg") options.color_mjpg = true;
            else {
                std::cerr << "error: --color must be bgra or mjpg\n";
                return 2;
            }
        } else if (arg == "--force") {
            options.force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for ros2mkv: " << arg << "\n";
            return 2;
        } else if (options.input_dir.empty()) {
            options.input_dir = arg;
        } else {
            std::cerr << "error: ros2mkv accepts one input directory\n";
            return 2;
        }
    }
    if (options.input_dir.empty()) {
        std::cerr << "error: ros2mkv requires a bag directory\n\n";
        print_ros2mkv_help();
        return 2;
    }
    if (options.output_mkv.empty()) {
        fs::path out(options.input_dir);
        if (out.filename().empty()) out = out.parent_path();
        options.output_mkv = (out.string() + ".mkv");
    }
    try {
        Ros2InputSummary summary = convert_ros2_to_mkv(options, [](const std::string &line) {
            std::cout << line << "\n";
        });
        std::cout << "ros2mkv: " << summary.color_frames << " color, " << summary.depth_frames << " depth, "
                  << summary.ir_frames << " IR\n";
        std::cout << "output: " << options.output_mkv << " (" << summary.mkv_size << " bytes)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_mkv2ros1(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_mkv2ros1_help();
            return 0;
        }
    }
    std::string input;
    std::string output;
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) return 2;
            output = argv[++i];
        } else if (arg == "--force") {
            force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for mkv2ros1: " << arg << "\n";
            return 2;
        } else if (input.empty()) {
            input = arg;
        } else {
            std::cerr << "error: mkv2ros1 accepts one input MKV\n";
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        std::cerr << "error: mkv2ros1 requires <input.mkv> -o <output.bag>\n";
        return 2;
    }
    try {
        const fs::path temp = fs::path(output + ".orbbec.tmp.bag");
        fs::remove(temp);
        try {
            Mkv2BagOptions to_bag;
            to_bag.input_mkv = input;
            to_bag.output_bag = temp.string();
            to_bag.force = true;
            mkv_to_bag(to_bag);
            Ros1ExportOptions export_options;
            export_options.input_bag = temp.string();
            export_options.output_bag = output;
            export_options.force = force;
            Ros1ExportSummary summary = export_ob_bag_to_ros1(export_options, [](const std::string &line) {
                std::cout << line << "\n";
            });
            fs::remove(temp);
            std::cout << "mkv2ros1: " << summary.color_frames << " color, " << summary.depth_frames << " depth, "
                      << summary.ir_frames << " IR\n";
            std::cout << "output: " << output << " (" << summary.bag_size << " bytes)\n";
            return 0;
        } catch (...) {
            fs::remove(temp);
            throw;
        }
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_mkv2ros2(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_mkv2ros2_help();
            return 0;
        }
    }
    std::string input;
    std::string output;
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) return 2;
            output = argv[++i];
        } else if (arg == "--force") {
            force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option for mkv2ros2: " << arg << "\n";
            return 2;
        } else if (input.empty()) {
            input = arg;
        } else {
            std::cerr << "error: mkv2ros2 accepts one input MKV\n";
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        std::cerr << "error: mkv2ros2 requires <input.mkv> -o <output-dir>\n";
        return 2;
    }
    try {
        const fs::path temp = fs::path(output + ".orbbec.tmp.bag");
        fs::remove(temp);
        try {
            Mkv2BagOptions to_bag;
            to_bag.input_mkv = input;
            to_bag.output_bag = temp.string();
            to_bag.force = true;
            mkv_to_bag(to_bag);
            Ros2ExportOptions export_options;
            export_options.input_bag = temp.string();
            export_options.output_dir = output;
            export_options.force = force;
            Ros2ExportSummary summary = export_ob_bag_to_ros2(export_options, [](const std::string &line) {
                std::cout << line << "\n";
            });
            fs::remove(temp);
            std::cout << "mkv2ros2: " << summary.color_frames << " color, " << summary.depth_frames << " depth, "
                      << summary.ir_frames << " IR, " << summary.message_count << " messages\n";
            std::cout << "output: " << output << "\n";
            return 0;
        } catch (...) {
            fs::remove(temp);
            throw;
        }
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_inspect(int argc, char **argv) {
    if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
        print_inspect_help();
        return 0;
    }
    if (argc != 3) {
        std::cerr << "error: inspect expects exactly one bag path\n\n";
        print_inspect_help();
        return 2;
    }
    ScanResult scan = inspect_bag(argv[2], false);
    print_scan(scan);
    return 0;
}

int cmd_convert(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_convert_help();
            return 0;
        }
    }
    if (argc < 4) {
        std::cerr << "error: convert requires <input.bag> -o <output.mkv>\n\n";
        print_convert_help();
        return 2;
    }

    ConvertOptions options;
    options.input_bag = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << arg << "\n";
                return 2;
            }
            options.output_mkv = argv[++i];
        } else if (arg == "--force") {
            options.force = true;
        } else if (arg == "--no-calibration") {
            options.no_calibration = true;
        } else if (arg == "--no-original-color") {
            options.preserve_source_color = false;
        } else if (arg == "--verify") {
            options.verify_after = true;
        } else if (arg == "--color") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --color\n";
                return 2;
            }
            std::string value = argv[++i];
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (value == "bgra") {
                options.color_output = ColorOutput::Bgra;
            } else if (value == "mjpg" || value == "mjpeg") {
                options.color_output = ColorOutput::Mjpg;
            } else {
                std::cerr << "error: --color must be bgra or mjpg\n";
                return 2;
            }
        } else if (arg == "--calibration-json") {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --calibration-json\n";
                return 2;
            }
            options.calibration_json_path = argv[++i];
        } else {
            std::cerr << "error: unknown option: " << arg << "\n";
            return 2;
        }
    }
    if (options.output_mkv.empty()) {
        std::cerr << "error: missing -o <output.mkv>\n";
        return 2;
    }

    CliProgress progress;
    try {
        ConvertSummary summary = convert_bag(
            options, [&progress](const ProgressInfo &info) { return progress(info); },
            [](const std::string &line) { std::cout << line << "\n"; });
        std::cout << "output: " << summary.output_path << "\n";
        if (options.verify_after) {
            std::cout << "verified: OK\n";
        }
        return 0;
    } catch (const CancelledError &) {
        std::cerr << "cancelled\n";
        return 3;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int cmd_verify(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_verify_help();
            return 0;
        }
    }
    if (argc != 4) {
        std::cerr << "error: verify expects <input.bag> <output.mkv>\n\n";
        print_verify_help();
        return 2;
    }
    CliProgress progress;
    try {
        VerifySummary summary = verify_bag(
            argv[2], argv[3], [&progress](const ProgressInfo &info) { return progress(info); },
            [](const std::string &line) { std::cout << line << "\n"; });
        return summary.ok ? 0 : 1;
    } catch (const CancelledError &) {
        std::cerr << "cancelled\n";
        return 3;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace

int main(int argc, char **argv) {
    install_k4a_log_filter();
    try {
        if (argc < 2) {
            print_overview();
            return 0;
        }
        std::string command = argv[1];
        if (command == "--help" || command == "-h" || command == "help") {
            if (argc >= 3) {
                std::string topic = argv[2];
                if (topic == "convert") {
                    print_convert_help();
                } else if (topic == "batch") {
                    print_batch_help();
                } else if (topic == "mkv2bag") {
                    print_mkv2bag_help();
                } else if (topic == "ros1") {
                    print_ros1_help();
                } else if (topic == "ros1mkv") {
                    print_ros1mkv_help();
                } else if (topic == "ros2") {
                    print_ros2_help();
                } else if (topic == "ros2mkv") {
                    print_ros2mkv_help();
                } else if (topic == "mkv2ros1") {
                    print_mkv2ros1_help();
                } else if (topic == "mkv2ros2") {
                    print_mkv2ros2_help();
                } else if (topic == "inspect") {
                    print_inspect_help();
                } else if (topic == "verify") {
                    print_verify_help();
                } else {
                    std::cerr << "error: unknown help topic: " << topic << "\n\n";
                    print_overview();
                    return 2;
                }
            } else {
                print_overview();
            }
            return 0;
        }
        if (command == "--version" || command == "version") {
            std::cout << "bag2mkv " << version() << "\n";
            return 0;
        }
        if (command == "--self-test") {
            if (self_test()) {
                std::cout << "self-test: OK\n";
                return 0;
            }
            std::cerr << "self-test: FAILED\n";
            return 1;
        }
        if (command == "inspect") {
            return cmd_inspect(argc, argv);
        }
        if (command == "convert") {
            return cmd_convert(argc, argv);
        }
        if (command == "batch") {
            return cmd_batch(argc, argv);
        }
        if (command == "mkv2bag") {
            return cmd_mkv2bag(argc, argv);
        }
        if (command == "ros1") {
            return cmd_ros1(argc, argv);
        }
        if (command == "ros1mkv") {
            return cmd_ros1mkv(argc, argv);
        }
        if (command == "ros2") {
            return cmd_ros2(argc, argv);
        }
        if (command == "ros2mkv") {
            return cmd_ros2mkv(argc, argv);
        }
        if (command == "mkv2ros1") {
            return cmd_mkv2ros1(argc, argv);
        }
        if (command == "mkv2ros2") {
            return cmd_mkv2ros2(argc, argv);
        }
        if (command == "verify") {
            return cmd_verify(argc, argv);
        }
        std::cerr << "error: unknown command: " << command << "\n\n";
        print_overview();
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
