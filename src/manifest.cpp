#include "manifest.hpp"
#include "util.hpp"

#include <iomanip>
#include <sstream>

namespace bag2mkv {
namespace {

std::string q(const std::string &s) { return "\"" + json_escape(s) + "\""; }

std::string float_array(const float *values, size_t count) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < count; ++i) {
        if (i) o << ", ";
        o << format_float(values[i]);
    }
    o << "]";
    return o.str();
}

std::string device_json(const DeviceInfo &d) {
    if (!d.present) {
        return "null";
    }
    std::ostringstream o;
    o << "{";
    o << "\"name\":" << q(d.name);
    o << ",\"full_name\":" << q(d.full_name);
    o << ",\"asic_name\":" << q(d.asic_name);
    o << ",\"vid\":" << d.vid;
    o << ",\"pid\":" << d.pid;
    o << ",\"uid\":" << q(d.uid);
    o << ",\"sn\":" << q(d.sn);
    o << ",\"fw_version\":" << q(d.fw_version);
    o << ",\"hw_version\":" << q(d.hw_version);
    o << ",\"supported_sdk_version\":" << q(d.supported_sdk_version);
    o << ",\"connection_type\":" << q(d.connection_type);
    o << ",\"type\":" << d.type;
    o << ",\"backend_type\":" << int(d.backend_type);
    o << ",\"ip_address\":" << q(d.ip_address);
    o << ",\"local_mac\":" << q(d.local_mac);
    o << ",\"timestamp_usec\":" << d.timestamp_usec;
    o << "}";
    return o.str();
}

std::string stream_json(const StreamProfile &p) {
    std::ostringstream o;
    o << "{";
    o << "\"stream_type\":" << int(p.stream_type);
    o << ",\"kind\":" << q(stream_kind(p.stream_type));
    o << ",\"format\":" << int(p.format);
    o << ",\"width\":" << p.width;
    o << ",\"height\":" << p.height;
    o << ",\"fps\":" << p.fps;
    o << ",\"intrinsic\":{\"fx\":" << format_float(p.intrinsic[0]) << ",\"fy\":" << format_float(p.intrinsic[1])
      << ",\"cx\":" << format_float(p.intrinsic[2]) << ",\"cy\":" << format_float(p.intrinsic[3]) << "}";
    o << ",\"distortion\":" << float_array(p.distortion, 8);
    o << ",\"distortion_model\":" << int(p.distortion_model);
    o << ",\"rotation\":" << float_array(p.rotation, 9);
    o << ",\"translation\":" << float_array(p.translation, 3);
    o << ",\"timestamp_usec\":" << p.timestamp_usec;
    o << "}";
    return o.str();
}

struct CalibMode {
    uint32_t sensor_w;
    uint32_t sensor_h;
    uint32_t bin_w;
    uint32_t bin_h;
    uint32_t out_w;
    uint32_t out_h;
    float crop_x;
    float crop_y;
};

CalibMode depth_calib_mode(uint32_t w, uint32_t h) {
    if (w == 320 && h == 288) return {1024, 1024, 512, 512, 320, 288, 96, 90};
    if (w == 512 && h == 512) return {1024, 1024, 512, 512, 512, 512, 0, 0};
    if (w == 1024 && h == 1024) return {1024, 1024, 1024, 1024, 1024, 1024, 0, 0};
    // NFOV_UNBINNED and the default 640x576 case.
    return {1024, 1024, 1024, 1024, w, h, 192, 180};
}

CalibMode color_calib_mode(uint32_t w, uint32_t h) {
    // K4A raw color calibration is 4:3 (4096x3072). The mode-specific code
    // multiplies the unitized parameters by the binned calibration resolution.
    if (w == 1280 && h == 720) return {4096, 3072, 1280, 960, 1280, 720, 0, 120};
    if (w == 1920 && h == 1080) return {4096, 3072, 1920, 1440, 1920, 1080, 0, 180};
    if (w == 2560 && h == 1440) return {4096, 3072, 2560, 1920, 2560, 1440, 0, 240};
    if (w == 2048 && h == 1536) return {4096, 3072, 2048, 1536, 2048, 1536, 0, 0};
    if (w == 3840 && h == 2160) return {4096, 3072, 3840, 2880, 3840, 2160, 0, 360};
    if (w == 4096 && h == 3072) return {4096, 3072, 4096, 3072, 4096, 3072, 0, 0};
    return {4096, 3072, w, h, w, h, 0, 0};
}

std::string calibration_camera(const StreamProfile &p, const char *location, const char *purpose, bool is_depth,
                               const StreamProfile *d2c, const std::string &serial) {
    const CalibMode mode = is_depth ? depth_calib_mode(p.width, p.height) : color_calib_mode(p.width, p.height);
    // k4a_calibration_get_from_raw() plus K4A's mode-specific conversion expects
    // unitized, 0-cornered parameters. The mode conversion applies:
    //   fx_px = fx * bin_w; cx_px = cx * bin_w - crop_x - 0.5
    //   fy_px = fy * bin_h; cy_px = cy * bin_h - crop_y - 0.5
    const float fx = p.intrinsic[0] / float(mode.bin_w);
    const float fy = p.intrinsic[1] / float(mode.bin_h);
    const float cx = (p.intrinsic[2] + 0.5f + mode.crop_x) / float(mode.bin_w);
    const float cy = (p.intrinsic[3] + 0.5f + mode.crop_y) / float(mode.bin_h);
    std::ostringstream o;
    o << "{";
    o << "\"Location\":" << q(location);
    o << ",\"Purpose\":" << q(purpose);
    o << ",\"SensorWidth\":" << mode.sensor_w;
    o << ",\"SensorHeight\":" << mode.sensor_h;
    o << ",\"MetricRadius\":0";
    o << ",\"Intrinsics\":{";
    o << "\"ModelParameterCount\":14";
    o << ",\"ModelParameters\":[";
    // K4A's parameter order is cx, cy, fx, fy, k1..k6, codx, cody, p2, p1.
    o << format_float(cx) << "," << format_float(cy) << "," << format_float(fx) << "," << format_float(fy);
    for (int i = 0; i < 6; ++i) {
        o << "," << format_float(p.distortion[i]);
    }
    o << ",0,0";
    o << "," << format_float(p.distortion[7]) << "," << format_float(p.distortion[6]);
    o << "],\"ModelType\":\"CALIBRATION_LensDistortionModelBrownConrady\"}";
    o << ",\"Rt\":{";
    if (is_depth || d2c == nullptr) {
        o << "\"Rotation\":[1,0,0,0,1,0,0,0,1],\"Translation\":[0,0,0]";
    } else {
        o << "\"Rotation\":" << float_array(d2c->rotation, 9);
        o << ",\"Translation\":[";
        for (int i = 0; i < 3; ++i) {
            if (i) o << ",";
            o << format_float(d2c->translation[i] / 1000.0f);
        }
        o << "]";
    }
    o << "}";
    o << ",\"Shutter\":\"CALIBRATION_ShutterTypeUndefined\"";
    o << ",\"ThermalAdjustmentParams\":{\"Params\":[0,0,0,0,0,0,0,0,0,0,0,0]}";
    o << ",\"Metadata\":{\"SerialId\":" << q(serial) << "}";
    o << "}";
    return o.str();
}

std::string imu_calibration(const char *sensor_type, int model_type_mask) {
    // The bag format does not store a K4A-format IMU calibration. K4A's JSON parser
    // nevertheless requires gyro and accel entries; these zero placeholders keep the
    // camera calibration loadable. No IMU frames are written in this version.
    std::ostringstream o;
    o << "{";
    o << "\"BiasTemperatureModel\":[0,0,0,0,0,0,0,0,0,0,0,0]";
    o << ",\"BiasUncertainty\":[0,0,0]";
    o << ",\"Id\":\"BAG2MKV_IMU\"";
    o << ",\"MixingMatrixTemperatureModel\":[";
    for (int i = 0; i < 36; ++i) {
        if (i) o << ",";
        o << "0";
    }
    o << "]";
    o << ",\"ModelTypeMask\":" << model_type_mask;
    o << ",\"Noise\":[0,0,0,0,0,0]";
    o << ",\"Rt\":{\"Rotation\":[1,0,0,0,1,0,0,0,1],\"Translation\":[0,0,0]}";
    o << ",\"SecondOrderScaling\":[0,0,0,0,0,0,0,0,0]";
    o << ",\"SensorType\":" << q(sensor_type);
    o << ",\"TemperatureBounds\":[0,0]";
    o << ",\"TemperatureC\":0";
    o << "}";
    return o.str();
}

}  // namespace

std::string synthesize_calibration_json(const ScanResult &scan) {
    auto depth_it = scan.profiles.find(3);
    auto color_it = scan.profiles.find(2);
    if (depth_it == scan.profiles.end() && color_it == scan.profiles.end()) {
        return {};
    }
    const std::string serial = scan.device.present ? scan.device.sn : std::string();
    const std::string device_name = scan.device.present ? scan.device.full_name : std::string();

    std::ostringstream o;
    o << "{\"CalibrationInformation\":{\"Cameras\":[";
    bool first = true;
    if (depth_it != scan.profiles.end()) {
        o << calibration_camera(depth_it->second, "CALIBRATION_CameraLocationD0", "CALIBRATION_CameraPurposeDepth",
                                true, nullptr, serial);
        first = false;
    }
    if (color_it != scan.profiles.end()) {
        if (!first) o << ",";
        const StreamProfile *d2c = depth_it != scan.profiles.end() ? &depth_it->second : nullptr;
        o << calibration_camera(color_it->second, "CALIBRATION_CameraLocationPV0",
                                "CALIBRATION_CameraPurposePhotoVideo", false, d2c, serial);
    }
    o << "],\"InertialSensors\":["
      << imu_calibration("CALIBRATION_InertialSensorType_Gyro", 0) << ","
      << imu_calibration("CALIBRATION_InertialSensorType_Accelerometer", 0) << "],";
    o << "\"Metadata\":{\"SerialId\":" << q(serial) << ",\"DeviceName\":" << q(device_name)
      << ",\"Notes\":\"IMU calibration unavailable; zero placeholders\",\"Version\":{\"Major\":1,\"Minor\":2}}}}";
    return o.str();
}

MetadataBundle build_metadata(const ScanResult &scan, const std::string &calibration_json,
                              const std::string &calibration_source, const std::string &color_output,
                              const std::string &source_color_track) {
    MetadataBundle bundle;
    bundle.calibration_json = calibration_json;
    bundle.calibration_source = calibration_source;

    std::ostringstream m;
    m << "{";
    m << "\"tool\":{\"name\":\"bag2mkv\",\"version\":\"0.1.0\"}";
    m << ",\"source\":{\"path\":" << q(scan.source_path) << ",\"size\":" << scan.source_size
      << ",\"index_pos\":" << scan.index_pos << ",\"declared_chunks\":" << scan.declared_chunk_count
      << ",\"actual_chunks\":" << scan.chunk_count << ",\"declared_connections\":" << scan.declared_conn_count
      << "}";
    m << ",\"device\":" << device_json(scan.device);
    m << ",\"streams\":[";
    {
        bool first = true;
        for (auto &kv : scan.profiles) {
            if (!first) m << ",";
            m << stream_json(kv.second);
            first = false;
        }
    }
    m << "]";
    m << ",\"properties\":[";
    {
        bool first = true;
        for (auto &p : scan.properties) {
            if (!first) m << ",";
            m << "{\"id\":" << p.id << ",\"datasize\":" << p.datasize << ",\"timestamp_usec\":" << p.timestamp_usec
              << ",\"sha256\":" << q(sha256_hex(p.data.data(), p.data.size()))
              << ",\"base64\":" << q(base64_encode(p.data.data(), p.data.size())) << "}";
            first = false;
        }
    }
    m << "]";
    m << ",\"frames\":{\"total\":" << scan.frames.size() << ",\"by_kind\":{";
    {
        bool first = true;
        for (auto &kv : scan.frame_counts) {
            if (!first) m << ",";
            m << q(kv.first) << ":" << kv.second;
            first = false;
        }
    }
    m << "},\"first_timestamp_usec\":" << scan.first_timestamp_usec
      << ",\"last_timestamp_usec\":" << scan.last_timestamp_usec
      << ",\"per_frame_attachment\":\"orbbec_frames.jsonl\"}";
    m << ",\"calibration\":{\"source\":" << q(bundle.calibration_source)
      << ",\"attachment\":" << (bundle.calibration_json.empty() ? "null" : q("calibration.json")) << "}";
    m << ",\"color\":{\"output\":" << q(color_output)
      << ",\"source_track\":" << (source_color_track.empty() ? "null" : q(source_color_track))
      << ",\"note\":"
      << q(color_output == "bgra"
               ? "BGRA is decoded from the original MJPEG; the source track preserves the exact JPEG bytes"
               : "MJPEG bitstream copied unchanged")
      << "}";
    m << "}";
    bundle.manifest_json = m.str();

    std::ostringstream f;
    for (size_t i = 0; i < scan.frames.size(); ++i) {
        const FrameMeta &fr = scan.frames[i];
        f << "{\"index\":" << i << ",\"conn\":" << fr.conn_id << ",\"topic\":" << q(fr.topic)
          << ",\"kind\":" << q(fr.kind) << ",\"timestamp_usec\":" << fr.timestamp_usec
          << ",\"number\":" << fr.number << ",\"timestamp_systemusec\":" << fr.timestamp_systemusec
          << ",\"timestamp_globalusec\":" << fr.timestamp_globalusec << ",\"header_seq\":" << fr.header_seq
          << ",\"frame_id\":" << q(fr.frame_id) << ",\"width\":" << fr.width << ",\"height\":" << fr.height
          << ",\"encoding\":" << q(fr.encoding) << ",\"step\":" << fr.step << ",\"data_size\":" << fr.data_size
          << ",\"sha256\":" << q(fr.sha256) << ",\"metadatasize\":" << fr.metadatasize
          << ",\"pixel_bit_size\":" << int(fr.pixel_bit_size) << ",\"metadata_hex\":" << q(fr.metadata_hex)
          << "}\n";
    }
    bundle.frames_jsonl = f.str();

    for (const auto &p : scan.properties) {
        if (p.id == 4024 || p.id == 4026 || p.id == 4027 || p.id == 4036) {
            Attachment a;
            a.name = "orbbec_prop_" + std::to_string(p.id) + ".bin";
            a.data = p.data;
            bundle.extra_attachments.push_back(std::move(a));
        }
    }
    return bundle;
}

}  // namespace bag2mkv
