#include "converter.hpp"
#include "errors.hpp"
#include "scan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
              << "  verify  <input.bag> <out.mkv>     Independently verify frames, timestamps and calibration.\n"
              << "  help [command]                    Show this help, or detailed help for one command.\n"
              << "\n"
              << "GUI\n"
              << "  Run bag2mkv-gui.exe for the Windows GUI. It uses the same converter and supports\n"
              << "  drag-and-drop, file pickers, progress, cancel and an embedded log.\n"
              << "\n"
              << "QUICK START\n"
              << "  bag2mkv inspect capture.bag\n"
              << "  bag2mkv convert capture.bag -o capture.mkv --verify\n"
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
