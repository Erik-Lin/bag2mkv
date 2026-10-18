#include "converter.hpp"
#include "errors.hpp"
#include "mkv2bag.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bag2mkv;

constexpr int ID_INPUT = 1001;
constexpr int ID_BROWSE_INPUT = 1002;
constexpr int ID_OUTPUT = 1003;
constexpr int ID_BROWSE_OUTPUT = 1004;
constexpr int ID_COLOR = 1005;
constexpr int ID_PRESERVE = 1006;
constexpr int ID_VERIFY = 1007;
constexpr int ID_FORCE = 1008;
constexpr int ID_CALIB = 1009;
constexpr int ID_BROWSE_CALIB = 1010;
constexpr int ID_CLEAR_CALIB = 1011;
constexpr int ID_START = 1012;
constexpr int ID_CANCEL = 1013;
constexpr int ID_OPEN = 1014;
constexpr int ID_PROGRESS = 1015;
constexpr int ID_STATUS = 1016;
constexpr int ID_LOG = 1017;
constexpr int ID_NO_CALIB = 1018;
constexpr int ID_BATCH = 1019;
constexpr int ID_TO_BAG = 1020;

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_LOG = WM_APP + 2;
constexpr UINT WM_APP_DONE = WM_APP + 3;
constexpr UINT WM_APP_ERROR = WM_APP + 4;
constexpr UINT WM_APP_CANCELLED = WM_APP + 5;
constexpr UINT WM_APP_BATCH_DONE = WM_APP + 6;
constexpr UINT WM_APP_TO_BAG_DONE = WM_APP + 7;

struct BatchResult {
    uint64_t total = 0;
    uint64_t succeeded = 0;
    uint64_t failed = 0;
    std::wstring output_dir;
};

struct ToBagResult {
    Mkv2BagSummary summary;
    std::wstring output_path;
};

std::string narrow(const std::wstring &s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring get_text(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) {
        GetWindowTextW(hwnd, s.data(), len + 1);
    }
    s.resize(static_cast<size_t>(len));
    return s;
}

std::wstring human_size(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        swprintf_s(buf, L"%.2f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        swprintf_s(buf, L"%.1f MB", double(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

std::wstring pick_folder(HWND owner, const wchar_t *title) {
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE idlist = SHBrowseForFolderW(&info);
    if (idlist == nullptr) return {};
    wchar_t path[MAX_PATH] = {};
    std::wstring result;
    if (SHGetPathFromIDListW(idlist, path)) {
        result = path;
    }
    CoTaskMemFree(idlist);
    return result;
}

std::wstring pick_mkv_file(HWND owner) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"K4A MKV (*.mkv)\0*.mkv\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return {};
    return path;
}

std::wstring pick_bag_save(HWND owner, const std::wstring &suggested) {
    wchar_t path[MAX_PATH] = {};
    if (!suggested.empty()) {
        wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Orbbec ROSBAG (*.bag)\0*.bag\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bag";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return {};
    return path;
}

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    int dpi = 96;

    HWND hInput = nullptr;
    HWND hBrowseInput = nullptr;
    HWND hOutput = nullptr;
    HWND hBrowseOutput = nullptr;
    HWND hColor = nullptr;
    HWND hPreserve = nullptr;
    HWND hVerify = nullptr;
    HWND hForce = nullptr;
    HWND hNoCalib = nullptr;
    HWND hCalib = nullptr;
    HWND hBrowseCalib = nullptr;
    HWND hClearCalib = nullptr;
    HWND hStart = nullptr;
    HWND hCancel = nullptr;
    HWND hBatch = nullptr;
    HWND hToBag = nullptr;
    HWND hOpen = nullptr;
    HWND hProgress = nullptr;
    HWND hStatus = nullptr;
    HWND hLog = nullptr;

    std::thread worker;
    std::atomic<bool> cancel{false};
    bool running = false;
    bool close_when_done = false;
    bool batch_mode = false;
    std::atomic<uint64_t> batch_index{0};
    uint64_t batch_total = 0;
    std::wstring last_output_dir;
    ConvertSummary last_summary;

    int S(int value) const { return MulDiv(value, dpi, 96); }

    void create_controls();
    void layout();
    void apply_font();
    void set_running(bool value);
    void append_log(const std::wstring &line);
    void set_status(const std::wstring &line);
    void reset_progress();
    std::wstring suggested_output(const std::wstring &input) const;
    void browse_input();
    void browse_output();
    void browse_calibration();
    void start_conversion();
    void start_directory_batch(const std::wstring &input_dir, const std::wstring &output_dir);
    void start_batch();
    void start_batch_dirs(const std::wstring &input_dir, const std::wstring &output_dir);
    void start_mkv2bag();
    void start_mkv2bag_paths(const std::wstring &input, const std::wstring &output);
    void request_cancel();
    void open_output_folder();
    void on_progress(const ProgressInfo &info);
    void on_done(const ConvertSummary &summary);
    void on_batch_done(const BatchResult &result);
    void on_to_bag_done(const ToBagResult &result);
    void on_error(const std::string &error);
    void on_cancelled();
    void on_command(int id, int code);
    void on_drop(HDROP drop);
    void on_close();
};

void App::apply_font() {
    if (font == nullptr) return;
    EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), static_cast<LPARAM>(TRUE));
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
}

void App::create_controls() {
    auto make = [&](const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex_style, int id) {
        return CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };

    make(L"STATIC", L"输入目录", SS_LEFT, 0, 0);
    hInput = make(L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_INPUT);
    hBrowseInput = make(L"BUTTON", L"浏览...", BS_PUSHBUTTON, 0, ID_BROWSE_INPUT);

    make(L"STATIC", L"输出目录", SS_LEFT, 0, 0);
    hOutput = make(L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_OUTPUT);
    hBrowseOutput = make(L"BUTTON", L"浏览...", BS_PUSHBUTTON, 0, ID_BROWSE_OUTPUT);

    make(L"BUTTON", L"输出选项", BS_GROUPBOX, 0, 0);

    hColor = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, ID_COLOR);
    SendMessageW(hColor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
        L"BGRA 1080p（兼容 kinect-analyzer；约 12.5 GB）"));
    SendMessageW(hColor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
        L"MJPEG 原码流（约 1.2 GB；部分播放器不支持）"));
    SendMessageW(hColor, CB_SETCURSEL, 0, 0);

    hPreserve = make(L"BUTTON", L"保留原始 MJPEG 轨道（推荐，约 +0.2 GB）", BS_AUTOCHECKBOX, 0, ID_PRESERVE);
    hVerify = make(L"BUTTON", L"转换后自动验证（推荐）", BS_AUTOCHECKBOX, 0, ID_VERIFY);
    hForce = make(L"BUTTON", L"覆盖已存在的输出文件", BS_AUTOCHECKBOX, 0, ID_FORCE);
    hNoCalib = make(L"BUTTON", L"不写入 calibration.json（不推荐）", BS_AUTOCHECKBOX, 0, ID_NO_CALIB);

    make(L"STATIC", L"标定 JSON（可选）", SS_LEFT, 0, 0);
    hCalib = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY, WS_EX_CLIENTEDGE, ID_CALIB);
    hBrowseCalib = make(L"BUTTON", L"选择...", BS_PUSHBUTTON, 0, ID_BROWSE_CALIB);
    hClearCalib = make(L"BUTTON", L"清除", BS_PUSHBUTTON, 0, ID_CLEAR_CALIB);

    hStart = make(L"BUTTON", L"开始转换", BS_DEFPUSHBUTTON, 0, ID_START);
    hCancel = make(L"BUTTON", L"取消", BS_PUSHBUTTON, 0, ID_CANCEL);
    hOpen = make(L"BUTTON", L"打开输出目录", BS_PUSHBUTTON, 0, ID_OPEN);

    hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROGRESS)), instance, nullptr);
    hStatus = make(L"STATIC", L"选择包含 bag 或 mkv 的输入目录，再选择输出目录，点击“开始转换”。", SS_LEFT, 0,
                   ID_STATUS);
    hLog = make(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, WS_EX_CLIENTEDGE, ID_LOG);

    SendMessageW(hPreserve, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(hVerify, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    set_running(false);
    EnableWindow(hOpen, FALSE);
    apply_font();
}

void App::layout() {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int margin = S(16);
    const int label_w = S(92);
    const int button_w = S(76);
    const int row_h = S(25);
    const int gap = S(8);
    int y = margin;

    // Input row.
    // Labels are created with id 0; enumerate the first three static labels.
    HWND labels[4] = {};
    int found = 0;
    std::pair<HWND *, int *> label_ctx{labels, &found};
    EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
        auto *ctx = reinterpret_cast<std::pair<HWND *, int *> *>(param);
        wchar_t cls[16] = {};
        GetClassNameW(child, cls, 16);
        if (wcscmp(cls, L"Static") == 0 && GetDlgCtrlID(child) == 0) {
            if (*ctx->second < 4) {
                ctx->first[(*ctx->second)++] = child;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&label_ctx));

    if (found >= 1) MoveWindow(labels[0], margin, y + S(4), label_w, row_h, TRUE);
    MoveWindow(hInput, margin + label_w, y, rc.right - 2 * margin - label_w - button_w - gap, row_h, TRUE);
    MoveWindow(hBrowseInput, rc.right - margin - button_w, y, button_w, row_h, TRUE);
    y += row_h + gap;

    if (found >= 2) MoveWindow(labels[1], margin, y + S(4), label_w, row_h, TRUE);
    MoveWindow(hOutput, margin + label_w, y, rc.right - 2 * margin - label_w - button_w - gap, row_h, TRUE);
    MoveWindow(hBrowseOutput, rc.right - margin - button_w, y, button_w, row_h, TRUE);
    y += row_h + gap + S(4);

    const int group_h = S(196);
    // Group box is the only BS_GROUPBOX; find it.
    HWND group = nullptr;
    EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
        wchar_t text[64] = {};
        GetWindowTextW(child, text, 64);
        if (wcscmp(text, L"输出选项") == 0) {
            *reinterpret_cast<HWND *>(param) = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&group));
    if (group) MoveWindow(group, margin, y, rc.right - 2 * margin, group_h, TRUE);

    const int inner = y + S(28);
    MoveWindow(GetDlgItem(hwnd, ID_COLOR), margin + S(16), inner, rc.right - 2 * margin - S(32), S(220), TRUE);
    int cy = inner + S(32);
    MoveWindow(hPreserve, margin + S(16), cy, rc.right - 2 * margin - S(32), row_h, TRUE);
    cy += S(28);
    MoveWindow(hVerify, margin + S(16), cy, S(270), row_h, TRUE);
    MoveWindow(hForce, margin + S(300), cy, rc.right - margin - S(316), row_h, TRUE);
    cy += S(28);
    MoveWindow(hNoCalib, margin + S(16), cy, rc.right - 2 * margin - S(32), row_h, TRUE);
    cy += S(30);
    if (found >= 3) MoveWindow(labels[2], margin + S(16), cy + S(4), S(124), row_h, TRUE);
    MoveWindow(hCalib, margin + S(144), cy, rc.right - margin - S(144) - S(160), row_h, TRUE);
    MoveWindow(hBrowseCalib, rc.right - margin - S(156), cy, S(72), row_h, TRUE);
    MoveWindow(hClearCalib, rc.right - margin - S(76), cy, S(60), row_h, TRUE);
    y += group_h + gap;

    MoveWindow(hStart, margin, y, S(120), S(32), TRUE);
    MoveWindow(hCancel, margin + S(132), y, S(90), S(32), TRUE);
    MoveWindow(hOpen, margin + S(234), y, S(120), S(32), TRUE);
    y += S(42);

    MoveWindow(hProgress, margin, y, rc.right - 2 * margin, S(18), TRUE);
    y += S(26);
    MoveWindow(hStatus, margin, y, rc.right - 2 * margin, S(24), TRUE);
    y += S(30);

    MoveWindow(hLog, margin, y, rc.right - 2 * margin, rc.bottom - y - margin, TRUE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void App::set_running(bool value) {
    running = value;
    EnableWindow(hInput, !value);
    EnableWindow(hBrowseInput, !value);
    EnableWindow(hOutput, !value);
    EnableWindow(hBrowseOutput, !value);
    EnableWindow(hColor, !value);
    EnableWindow(hPreserve, !value);
    EnableWindow(hVerify, !value);
    EnableWindow(hForce, !value);
    EnableWindow(hNoCalib, !value);
    EnableWindow(hCalib, !value);
    EnableWindow(hBrowseCalib, !value);
    EnableWindow(hClearCalib, !value);
    EnableWindow(hStart, !value);
    EnableWindow(hCancel, value);
    if (value) {
        EnableWindow(hOpen, FALSE);
    }
}

void App::append_log(const std::wstring &line) {
    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    std::wstring text = line;
    text += L"\r\n";
    SendMessageW(hLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);
}

void App::set_status(const std::wstring &line) {
    SetWindowTextW(hStatus, line.c_str());
}

void App::reset_progress() {
    SendMessageW(hProgress, PBM_SETPOS, 0, 0);
}

std::wstring App::suggested_output(const std::wstring &input) const {
    if (input.empty()) return {};
    std::filesystem::path p(input);
    p.replace_extension(L".mkv");
    return p.wstring();
}

void App::browse_input() {
    std::wstring dir = pick_folder(hwnd, L"选择输入目录（只含 bag 或只含 mkv）");
    if (dir.empty()) return;
    SetWindowTextW(hInput, dir.c_str());
}

void App::browse_output() {
    std::wstring dir = pick_folder(hwnd, L"选择输出目录");
    if (dir.empty()) return;
    SetWindowTextW(hOutput, dir.c_str());
}

void App::browse_calibration() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Calibration JSON (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"选择同设备/同模式的 calibration.json";
    if (!GetOpenFileNameW(&ofn)) return;
    SetWindowTextW(hCalib, file);
}

void App::start_conversion() {
    std::wstring input = get_text(hInput);
    std::wstring output = get_text(hOutput);
    if (input.empty()) {
        MessageBoxW(hwnd, L"请先选择输入 bag 文件。", L"bag2mkv", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!std::filesystem::exists(std::filesystem::path(input))) {
        MessageBoxW(hwnd, L"输入 bag 文件不存在。", L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    if (std::filesystem::is_directory(std::filesystem::path(input))) {
        if (output.empty()) {
            MessageBoxW(hwnd, L"请选择输出目录。", L"bag2mkv", MB_ICONINFORMATION | MB_OK);
            return;
        }
        start_directory_batch(input, output);
        return;
    }
    std::wstring input_ext = std::filesystem::path(input).extension().wstring();
    std::transform(input_ext.begin(), input_ext.end(), input_ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (input_ext == L".mkv") {
        if (output.empty() || std::filesystem::is_directory(std::filesystem::path(output))) {
            std::filesystem::path out = output.empty()
                ? std::filesystem::path(input)
                : std::filesystem::path(output) / std::filesystem::path(input).filename();
            out.replace_extension(L".bag");
            output = out.wstring();
            SetWindowTextW(hOutput, output.c_str());
        }
        start_mkv2bag_paths(input, output);
        return;
    }
    if (output.empty() || std::filesystem::is_directory(std::filesystem::path(output))) {
        std::filesystem::path out = output.empty()
            ? std::filesystem::path(input)
            : std::filesystem::path(output) / std::filesystem::path(input).filename();
        out.replace_extension(L".mkv");
        output = out.wstring();
        SetWindowTextW(hOutput, output.c_str());
    }
    if (input == output) {
        MessageBoxW(hwnd, L"输入和输出不能是同一个文件。", L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }

    bool force = SendMessageW(hForce, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!force && std::filesystem::exists(std::filesystem::path(output))) {
        int answer = MessageBoxW(hwnd, L"输出文件已存在，是否覆盖？", L"bag2mkv",
                                 MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        force = true;
    }

    ConvertOptions options;
    options.input_bag = narrow(input);
    options.output_mkv = narrow(output);
    options.force = force;
    options.no_calibration = SendMessageW(hNoCalib, BM_GETCHECK, 0, 0) == BST_CHECKED;
    options.verify_after = SendMessageW(hVerify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    options.color_output =
        SendMessageW(hColor, CB_GETCURSEL, 0, 0) == 1 ? ColorOutput::Mjpg : ColorOutput::Bgra;
    options.preserve_source_color =
        options.color_output == ColorOutput::Bgra &&
        SendMessageW(hPreserve, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring calib = get_text(hCalib);
    if (!calib.empty()) {
        if (!std::filesystem::exists(std::filesystem::path(calib))) {
            MessageBoxW(hwnd, L"calibration.json 不存在。", L"bag2mkv", MB_ICONERROR | MB_OK);
            return;
        }
        options.calibration_json_path = narrow(calib);
    }

    if (worker.joinable()) {
        worker.join();
    }
    cancel = false;
    close_when_done = false;
    batch_mode = false;
    batch_index = 0;
    batch_total = 0;
    last_output_dir.clear();
    set_running(true);
    reset_progress();
    SetWindowTextW(hLog, L"");
    set_status(L"正在转换...");
    append_log(L"输入: " + input);
    append_log(L"输出: " + output);
    append_log(L"彩色: " + std::wstring(options.color_output == ColorOutput::Bgra ? L"BGRA 1080p" : L"MJPEG"));
    append_log(L"验证: " + std::wstring(options.verify_after ? L"开" : L"关"));

    worker = std::thread([this, options]() {
        try {
            ConvertSummary summary = convert_bag(
                options,
                [this](const ProgressInfo &info) -> bool {
                    if (cancel.load()) return false;
                    auto *copy = new ProgressInfo(info);
                    if (!PostMessageW(hwnd, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(copy))) {
                        delete copy;
                        return false;
                    }
                    return true;
                },
                [this](const std::string &line) {
                    auto *copy = new std::wstring(widen(line));
                    if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
                        delete copy;
                    }
                });
            auto *copy = new ConvertSummary(summary);
            if (!PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(copy))) {
                delete copy;
            }
        } catch (const CancelledError &) {
            PostMessageW(hwnd, WM_APP_CANCELLED, 0, 0);
        } catch (const std::exception &e) {
            auto *copy = new std::wstring(widen(e.what()));
            if (!PostMessageW(hwnd, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(copy))) {
                delete copy;
            }
        }
    });
}

void App::start_directory_batch(const std::wstring &input_dir, const std::wstring &output_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(input_dir), ec) || ec) {
        MessageBoxW(hwnd, L"输入目录不存在。", L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    if (output_dir.empty()) {
        MessageBoxW(hwnd, L"请选择输出目录。", L"bag2mkv", MB_ICONINFORMATION | MB_OK);
        return;
    }

    std::vector<std::filesystem::path> bags;
    std::vector<std::filesystem::path> mkvs;
    uint64_t other_files = 0;
    std::filesystem::recursive_directory_iterator it(
        input_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            MessageBoxW(hwnd, (L"无法读取输入目录：\n" + input_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
            return;
        }
        if (!it->is_regular_file()) continue;
        std::wstring ext = it->path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (ext == L".bag") {
            bags.push_back(it->path());
        } else if (ext == L".mkv") {
            mkvs.push_back(it->path());
        } else {
            ++other_files;
        }
    }
    if (ec) {
        MessageBoxW(hwnd, (L"无法读取输入目录：\n" + input_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    std::sort(bags.begin(), bags.end());
    std::sort(mkvs.begin(), mkvs.end());

    if (!bags.empty() && !mkvs.empty()) {
        MessageBoxW(hwnd,
                    (L"输入目录同时包含 bag 和 mkv，无法判断转换方向。\n\nbag: " + std::to_wstring(bags.size()) +
                     L"\nmkv: " + std::to_wstring(mkvs.size()) +
                     L"\n\n请让第一个目录只包含 bag 或只包含 mkv。").c_str(),
                    L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    if (bags.empty() && mkvs.empty()) {
        MessageBoxW(hwnd, L"输入目录及其子目录中没有 bag 或 mkv 文件。", L"bag2mkv",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

    const bool input_is_bag = !bags.empty();
    const auto &files = input_is_bag ? bags : mkvs;
    const std::wstring target_ext = input_is_bag ? L".mkv" : L".bag";

    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        MessageBoxW(hwnd, (L"无法创建输出目录：\n" + output_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    const std::filesystem::path input_abs = std::filesystem::weakly_canonical(input_dir, ec);
    if (ec) {
        MessageBoxW(hwnd, (L"无法解析输入目录：\n" + input_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    const std::filesystem::path output_abs = std::filesystem::weakly_canonical(output_dir, ec);
    if (ec) {
        MessageBoxW(hwnd, (L"无法解析输出目录：\n" + output_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    std::wstring input_abs_text = input_abs.wstring();
    std::wstring output_abs_text = output_abs.wstring();
    std::transform(input_abs_text.begin(), input_abs_text.end(), input_abs_text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::transform(output_abs_text.begin(), output_abs_text.end(), output_abs_text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    const std::wstring input_prefix = input_abs_text + L"\\";
    if (output_abs_text == input_abs_text || output_abs_text.rfind(input_prefix, 0) == 0) {
        MessageBoxW(hwnd, L"输出目录不能是输入目录或其子目录，否则下次运行会混入另一种格式。", L"bag2mkv",
                    MB_ICONERROR | MB_OK);
        return;
    }

    std::vector<std::filesystem::path> outputs;
    outputs.reserve(files.size());
    std::set<std::wstring> seen_outputs;
    for (const auto &file : files) {
        std::error_code rel_ec;
        std::filesystem::path rel = std::filesystem::relative(file, input_dir, rel_ec);
        if (rel_ec || rel.empty()) rel = file.filename();
        std::filesystem::path out = std::filesystem::path(output_dir) / rel.parent_path() / rel.stem();
        out.replace_extension(target_ext);
        std::wstring key = out.wstring();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (!seen_outputs.insert(key).second) {
            MessageBoxW(hwnd, (L"输出路径冲突：\n" + out.wstring()).c_str(), L"bag2mkv",
                        MB_ICONERROR | MB_OK);
            return;
        }
        outputs.push_back(out);
    }

    bool force = SendMessageW(hForce, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!force) {
        int existing = 0;
        for (const auto &out : outputs) {
            std::error_code exists_ec;
            if (std::filesystem::exists(out, exists_ec) && !exists_ec) ++existing;
        }
        if (existing > 0) {
            int answer = MessageBoxW(hwnd,
                                     (L"输出目录中已有 " + std::to_wstring(existing) +
                                      L" 个目标文件，是否全部覆盖？").c_str(),
                                     L"bag2mkv", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
            if (answer != IDYES) return;
            force = true;
        }
    }

    ConvertOptions base;
    base.force = force;
    base.no_calibration = SendMessageW(hNoCalib, BM_GETCHECK, 0, 0) == BST_CHECKED;
    base.verify_after = SendMessageW(hVerify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    base.color_output =
        SendMessageW(hColor, CB_GETCURSEL, 0, 0) == 1 ? ColorOutput::Mjpg : ColorOutput::Bgra;
    base.preserve_source_color =
        base.color_output == ColorOutput::Bgra &&
        SendMessageW(hPreserve, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (input_is_bag) {
        std::wstring calib = get_text(hCalib);
        if (!calib.empty()) {
            if (!std::filesystem::exists(std::filesystem::path(calib))) {
                MessageBoxW(hwnd, L"calibration.json 不存在。", L"bag2mkv", MB_ICONERROR | MB_OK);
                return;
            }
            base.calibration_json_path = narrow(calib);
        }
    }

    uint64_t input_bytes = 0;
    uint64_t existing_bytes = 0;
    bool sizes_ok = true;
    for (size_t i = 0; i < files.size(); ++i) {
        std::error_code size_ec;
        const uint64_t size = std::filesystem::file_size(files[i], size_ec);
        if (size_ec) {
            sizes_ok = false;
            break;
        }
        input_bytes += size;
        const uint64_t out_size = std::filesystem::file_size(outputs[i], size_ec);
        if (!size_ec) existing_bytes += out_size;
    }
    if (sizes_ok && input_bytes > 0) {
        const double factor = input_is_bag ? (base.color_output == ColorOutput::Bgra ? 16.0 : 1.7) : 1.2;
        const uint64_t estimate = uint64_t(double(input_bytes) * factor) + 64ull * 1024ull * 1024ull;
        std::error_code space_ec;
        const std::filesystem::space_info space = std::filesystem::space(output_dir, space_ec);
        if (!space_ec && space.available > 0) {
            const uint64_t available = space.available + existing_bytes;
            if (estimate > available) {
                MessageBoxW(hwnd,
                            (L"磁盘空间不足，尚未开始转换。\n\n文件数：" + std::to_wstring(files.size()) +
                             L"\n预计输出：" + human_size(estimate) + L"\n输出盘可用：" + human_size(available) +
                             L"\n\n可以改用 MJPEG 模式或增加磁盘空间。").c_str(),
                            L"bag2mkv", MB_ICONERROR | MB_OK);
                return;
            }
        }
    }

    if (worker.joinable()) worker.join();
    cancel = false;
    close_when_done = false;
    batch_mode = true;
    batch_total = files.size();
    batch_index = 0;
    last_output_dir = output_dir;
    set_running(true);
    reset_progress();
    SetWindowTextW(hLog, L"");
    set_status(input_is_bag ? L"正在批量转换 bag → mkv..." : L"正在批量转换 mkv → bag...");
    append_log(L"输入目录: " + input_dir);
    append_log(L"输出目录: " + output_dir);
    append_log(std::wstring(L"方向: ") + (input_is_bag ? L"bag → mkv" : L"mkv → bag"));
    append_log(L"文件数: " + std::to_wstring(files.size()));
    if (other_files > 0) {
        append_log(L"已忽略 " + std::to_wstring(other_files) + L" 个其他文件");
    }

    worker = std::thread([this, input_is_bag, files, outputs, force, base]() {
        BatchResult result;
        result.total = files.size();
        result.output_dir = last_output_dir;
        for (size_t i = 0; i < files.size(); ++i) {
            if (cancel.load()) {
                PostMessageW(hwnd, WM_APP_CANCELLED, 0, 0);
                return;
            }
            batch_index = i + 1;
            std::wstring prefix = L"[" + std::to_wstring(i + 1) + L"/" + std::to_wstring(files.size()) + L"] ";
            auto *start_line = new std::wstring(prefix + files[i].wstring() + L" -> " + outputs[i].wstring());
            if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(start_line))) {
                delete start_line;
            }
            try {
                if (input_is_bag) {
                    ConvertOptions options = base;
                    options.input_bag = narrow(files[i].wstring());
                    options.output_mkv = narrow(outputs[i].wstring());
                    convert_bag(
                        options,
                        [this, prefix](const ProgressInfo &info) -> bool {
                            if (cancel.load()) return false;
                            auto *copy = new ProgressInfo(info);
                            copy->message = narrow(prefix) + copy->message;
                            if (!PostMessageW(hwnd, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(copy))) {
                                delete copy;
                                return false;
                            }
                            return true;
                        },
                        [this, prefix](const std::string &line) {
                            auto *copy = new std::wstring(prefix + widen(line));
                            if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
                                delete copy;
                            }
                        });
                } else {
                    Mkv2BagOptions options;
                    options.input_mkv = narrow(files[i].wstring());
                    options.output_bag = narrow(outputs[i].wstring());
                    options.force = force;
                    mkv_to_bag(
                        options,
                        [this, prefix](const std::string &line) {
                            auto *copy = new std::wstring(prefix + widen(line));
                            if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
                                delete copy;
                            }
                        },
                        [this, prefix, i, total = files.size()](uint64_t current, uint64_t) -> bool {
                            if (cancel.load()) return false;
                            if (current % 100 == 0) {
                                auto *info = new ProgressInfo{Stage::Writing, i + 1, total,
                                                              narrow(prefix) + "mkv2bag " +
                                                                  std::to_string(current)};
                                if (!PostMessageW(hwnd, WM_APP_PROGRESS, 0,
                                                  reinterpret_cast<LPARAM>(info))) {
                                    delete info;
                                    return false;
                                }
                            }
                            return true;
                        });
                }
                ++result.succeeded;
            } catch (const CancelledError &) {
                PostMessageW(hwnd, WM_APP_CANCELLED, 0, 0);
                return;
            } catch (const std::exception &e) {
                ++result.failed;
                auto *error_line = new std::wstring(prefix + L"错误: " + widen(e.what()));
                if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(error_line))) {
                    delete error_line;
                }
            }
        }
        auto *copy = new BatchResult(result);
        if (!PostMessageW(hwnd, WM_APP_BATCH_DONE, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });
}
void App::start_batch() {
    std::wstring input_dir = pick_folder(hwnd, L"选择输入目录（只含 bag 或只含 mkv）");
    if (input_dir.empty()) return;
    std::wstring output_dir = pick_folder(hwnd, L"选择输出目录");
    if (output_dir.empty()) return;
    start_directory_batch(input_dir, output_dir);
}

void App::start_batch_dirs(const std::wstring &input_dir, const std::wstring &output_dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> bags;
    std::filesystem::recursive_directory_iterator it(
        input_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            MessageBoxW(hwnd, (L"无法读取输入目录：\n" + input_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
            return;
        }
        if (!it->is_regular_file()) continue;
        std::wstring ext = it->path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (ext == L".bag") bags.push_back(it->path());
    }
    if (ec) {
        MessageBoxW(hwnd, (L"无法读取输入目录：\n" + input_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }
    std::sort(bags.begin(), bags.end());
    if (bags.empty()) {
        MessageBoxW(hwnd, L"该目录及其子目录下没有 .bag 文件。", L"bag2mkv", MB_ICONINFORMATION | MB_OK);
        return;
    }
    {
        std::set<std::wstring> stems;
        for (const auto &bag : bags) {
            std::wstring stem = bag.stem().wstring();
            std::transform(stem.begin(), stem.end(), stem.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (!stems.insert(stem).second) {
                MessageBoxW(hwnd,
                            (L"输入目录中存在同名 bag，输出会冲突：\n" + bag.wstring() +
                             L"\n\n请先拆分目录，或改用 CLI 并自行处理命名。").c_str(),
                            L"bag2mkv", MB_ICONERROR | MB_OK);
                return;
            }
        }
    }
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        MessageBoxW(hwnd, (L"无法创建输出目录：\n" + output_dir).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
        return;
    }

    bool force = SendMessageW(hForce, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!force) {
        int existing = 0;
        for (const auto &bag : bags) {
            std::filesystem::path out = std::filesystem::path(output_dir) / (bag.stem().wstring() + L".mkv");
            if (std::filesystem::exists(out)) ++existing;
        }
        if (existing > 0) {
            int answer = MessageBoxW(hwnd, L"部分输出文件已存在，是否全部覆盖？", L"bag2mkv",
                                     MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
            if (answer != IDYES) return;
            force = true;
        }
    }

    ConvertOptions base;
    base.force = force;
    base.no_calibration = SendMessageW(hNoCalib, BM_GETCHECK, 0, 0) == BST_CHECKED;
    base.verify_after = SendMessageW(hVerify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    base.color_output =
        SendMessageW(hColor, CB_GETCURSEL, 0, 0) == 1 ? ColorOutput::Mjpg : ColorOutput::Bgra;
    base.preserve_source_color =
        base.color_output == ColorOutput::Bgra &&
        SendMessageW(hPreserve, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring calib = get_text(hCalib);
    if (!calib.empty()) {
        if (!std::filesystem::exists(std::filesystem::path(calib))) {
            MessageBoxW(hwnd, L"calibration.json 不存在。", L"bag2mkv", MB_ICONERROR | MB_OK);
            return;
        }
        base.calibration_json_path = narrow(calib);
    }

    uint64_t input_bytes = 0;
    uint64_t existing_bytes = 0;
    bool input_sizes_ok = true;
    for (const auto &bag : bags) {
        std::error_code size_ec;
        uint64_t size = std::filesystem::file_size(bag, size_ec);
        if (size_ec) {
            input_sizes_ok = false;
            break;
        }
        input_bytes += size;
        std::filesystem::path out = std::filesystem::path(output_dir) / (bag.stem().wstring() + L".mkv");
        size = std::filesystem::file_size(out, size_ec);
        if (!size_ec) existing_bytes += size;
    }
    if (input_sizes_ok && input_bytes > 0) {
        const double factor = base.color_output == ColorOutput::Bgra ? 16.0 : 1.7;
        const uint64_t estimate = uint64_t(double(input_bytes) * factor) + 64ull * 1024ull * 1024ull;
        std::error_code space_ec;
        std::filesystem::space_info space = std::filesystem::space(output_dir, space_ec);
        if (!space_ec && space.available > 0) {
            const uint64_t available = space.available + existing_bytes;
            if (estimate > available) {
                std::wstring message = L"批量空间不足，未开始转换。\n\n文件数：" + std::to_wstring(bags.size()) +
                                       L"\n预计输出：" + human_size(estimate) +
                                       L"\n输出盘可用：" + human_size(available) +
                                       L"\n\n可以改用 MJPEG 模式、增加磁盘空间，或先处理一部分文件。";
                MessageBoxW(hwnd, message.c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
                return;
            }
        }
    }

    if (worker.joinable()) worker.join();
    cancel = false;
    close_when_done = false;
    batch_mode = true;
    batch_total = bags.size();
    batch_index = 0;
    set_running(true);
    reset_progress();
    SetWindowTextW(hLog, L"");
    set_status(L"批量转换中...");
    append_log(L"批量输入: " + input_dir);
    append_log(L"批量输出: " + output_dir);
    append_log(L"文件数: " + std::to_wstring(bags.size()));

    worker = std::thread([this, base, bags, output_dir]() {
        BatchResult result;
        result.total = bags.size();
        result.output_dir = output_dir;
        for (size_t i = 0; i < bags.size(); ++i) {
            if (cancel.load()) {
                PostMessageW(hwnd, WM_APP_CANCELLED, 0, 0);
                return;
            }
            batch_index = i + 1;
            std::wstring out_path = (std::filesystem::path(output_dir) /
                                     (bags[i].stem().wstring() + L".mkv")).wstring();
            std::wstring prefix = L"[" + std::to_wstring(i + 1) + L"/" + std::to_wstring(bags.size()) + L"] ";
            auto *start_line = new std::wstring(prefix + bags[i].wstring() + L" -> " + out_path);
            if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(start_line))) {
                delete start_line;
            }
            ConvertOptions options = base;
            options.input_bag = narrow(bags[i].wstring());
            options.output_mkv = narrow(out_path);
            try {
                convert_bag(
                    options,
                    [this, prefix](const ProgressInfo &info) -> bool {
                        if (cancel.load()) return false;
                        auto *copy = new ProgressInfo(info);
                        copy->message = narrow(prefix) + copy->message;
                        if (!PostMessageW(hwnd, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(copy))) {
                            delete copy;
                            return false;
                        }
                        return true;
                    },
                    [this, prefix](const std::string &line) {
                        auto *copy = new std::wstring(prefix + widen(line));
                        if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
                            delete copy;
                        }
                    });
                ++result.succeeded;
            } catch (const CancelledError &) {
                PostMessageW(hwnd, WM_APP_CANCELLED, 0, 0);
                return;
            } catch (const std::exception &e) {
                ++result.failed;
                auto *error_line = new std::wstring(prefix + L"错误: " + widen(e.what()));
                if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(error_line))) {
                    delete error_line;
                }
            }
        }
        auto *copy = new BatchResult(result);
        if (!PostMessageW(hwnd, WM_APP_BATCH_DONE, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });
}

void App::start_mkv2bag() {
    std::wstring input = pick_mkv_file(hwnd);
    if (input.empty()) return;

    std::filesystem::path suggested(input);
    suggested.replace_extension(".bag");
    std::wstring output = pick_bag_save(hwnd, suggested.wstring());
    if (output.empty()) return;
    start_mkv2bag_paths(input, output);
}

void App::start_mkv2bag_paths(const std::wstring &input, const std::wstring &output) {
    if (input.empty() || output.empty()) return;
    if (worker.joinable()) worker.join();
    cancel = false;
    close_when_done = false;
    batch_mode = false;
    batch_index = 0;
    batch_total = 0;
    last_output_dir = std::filesystem::path(output).parent_path().wstring();
    if (last_output_dir.empty()) last_output_dir = L".";
    set_running(true);
    reset_progress();
    SetWindowTextW(hLog, L"");
    set_status(L"MKV 转 bag...");
    append_log(L"MKV 转 bag: " + input + L" -> " + output);

    worker = std::thread([this, input, output]() {
        Mkv2BagOptions options;
        options.input_mkv = narrow(input);
        options.output_bag = narrow(output);
        options.force = true;  // GetSaveFileName already confirmed overwrite
        try {
            Mkv2BagSummary summary = mkv_to_bag(
                options,
                [this](const std::string &line) {
                    auto *copy = new std::wstring(widen(line));
                    if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
                        delete copy;
                    }
                },
                [this](uint64_t current, uint64_t) -> bool {
                    if (cancel.load()) return false;
                    if (current % 100 == 0) {
                        auto *copy = new std::wstring(L"已处理 " + std::to_wstring(current) + L" 帧");
                        if (!PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
                            delete copy;
                        }
                    }
                    return true;
                });
            auto *result = new ToBagResult{summary, output};
            if (!PostMessageW(hwnd, WM_APP_TO_BAG_DONE, 0, reinterpret_cast<LPARAM>(result))) {
                delete result;
            }
        } catch (const CancelledError &) {
            PostMessageW(hwnd, WM_APP_CANCELLED, 0, 0);
        } catch (const std::exception &e) {
            auto *error = new std::wstring(widen(e.what()));
            if (!PostMessageW(hwnd, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(error))) {
                delete error;
            }
        }
    });
}

void App::request_cancel() {
    if (!running) return;
    cancel = true;
    EnableWindow(hCancel, FALSE);
    set_status(L"正在取消...");
    append_log(L"用户请求取消，等待当前帧结束...");
}

void App::open_output_folder() {
    std::wstring folder = last_output_dir;
    if (folder.empty() && !last_summary.output_path.empty()) {
        folder = std::filesystem::path(last_summary.output_path).parent_path().wstring();
    }
    if (folder.empty()) return;
    ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::on_progress(const ProgressInfo &info) {
    std::wstring stage;
    switch (info.stage) {
        case Stage::Scanning: stage = L"扫描 bag"; break;
        case Stage::Preparing: stage = L"准备元数据"; break;
        case Stage::Writing: stage = L"写入帧"; break;
        case Stage::Finalizing: stage = L"完成文件"; break;
        case Stage::Verifying: stage = L"验证"; break;
        case Stage::Done: stage = L"完成"; break;
    }
    if (info.total > 0) {
        int pos = int((info.current * 1000) / info.total);
        SendMessageW(hProgress, PBM_SETPOS, pos, 0);
    }
    if (info.stage == Stage::Writing || info.stage == Stage::Verifying || info.stage == Stage::Scanning) {
        wchar_t buf[160];
        if (batch_mode) {
            swprintf_s(buf, L"[%llu/%llu] %s：%llu / %llu",
                       static_cast<unsigned long long>(batch_index.load()),
                       static_cast<unsigned long long>(batch_total), stage.c_str(),
                       static_cast<unsigned long long>(info.current),
                       static_cast<unsigned long long>(info.total));
        } else {
            swprintf_s(buf, L"%s：%llu / %llu", stage.c_str(), static_cast<unsigned long long>(info.current),
                       static_cast<unsigned long long>(info.total));
        }
        set_status(buf);
    } else {
        set_status(stage + L"...");
    }
}

void App::on_done(const ConvertSummary &summary) {
    if (worker.joinable()) worker.join();
    batch_mode = false;
    last_summary = summary;
    last_output_dir = std::filesystem::path(summary.output_path).parent_path().wstring();
    if (last_output_dir.empty()) last_output_dir = L".";
    set_running(false);
    cancel = false;
    SendMessageW(hProgress, PBM_SETPOS, 1000, 0);
    std::wstring status = L"转换完成";
    if (summary.verified) status += L"（验证通过）";
    set_status(status);
    append_log(L"完成: " + widen(summary.output_path));
    append_log(L"大小: " + human_size(summary.output_size));
    append_log(L"帧: " + std::to_wstring(summary.total_frames) + L"（COLOR " +
               std::to_wstring(summary.color_frames) + L", DEPTH " + std::to_wstring(summary.depth_frames) + L"）");
    append_log(L"标定: " + widen(summary.calibration_source));
    EnableWindow(hOpen, TRUE);
    std::wstring message = L"转换完成。\n\n输出：\n" + widen(summary.output_path) + L"\n\n大小：" +
                           human_size(summary.output_size);
    if (summary.verified) message += L"\n验证：通过";
    MessageBoxW(hwnd, message.c_str(), L"bag2mkv", MB_ICONINFORMATION | MB_OK);
    if (close_when_done) {
        DestroyWindow(hwnd);
    }
}

void App::on_batch_done(const BatchResult &result) {
    if (worker.joinable()) worker.join();
    batch_mode = false;
    last_output_dir = result.output_dir;
    set_running(false);
    cancel = false;
    SendMessageW(hProgress, PBM_SETPOS, 1000, 0);
    std::wstring status = L"批量完成：" + std::to_wstring(result.succeeded) + L" 成功";
    if (result.failed > 0) status += L"，" + std::to_wstring(result.failed) + L" 失败";
    set_status(status);
    append_log(status);
    EnableWindow(hOpen, TRUE);
    std::wstring message = L"批量转换完成。\n\n成功：" + std::to_wstring(result.succeeded) +
                           L"\n失败：" + std::to_wstring(result.failed) + L"\n总计：" +
                           std::to_wstring(result.total) + L"\n\n输出目录：\n" + result.output_dir;
    MessageBoxW(hwnd, message.c_str(), L"bag2mkv",
                (result.failed == 0 ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
    if (close_when_done) {
        DestroyWindow(hwnd);
    }
}

void App::on_to_bag_done(const ToBagResult &result) {
    if (worker.joinable()) worker.join();
    batch_mode = false;
    last_output_dir = std::filesystem::path(result.output_path).parent_path().wstring();
    if (last_output_dir.empty()) last_output_dir = L".";
    set_running(false);
    cancel = false;
    SendMessageW(hProgress, PBM_SETPOS, 1000, 0);
    const auto &summary = result.summary;
    set_status(L"MKV 转 bag 完成");
    append_log(L"MKV 转 bag 完成: " + result.output_path);
    append_log(L"帧: " + std::to_wstring(summary.color_frames) + L" color, " +
               std::to_wstring(summary.depth_frames) + L" depth, " + std::to_wstring(summary.ir_frames) +
               L" IR");
    append_log(L"属性: " + std::to_wstring(summary.properties) + L"，profile: " +
               std::to_wstring(summary.profiles) + L"，大小: " + human_size(summary.bag_size));
    EnableWindow(hOpen, TRUE);
    std::wstring message = L"MKV 转 bag 完成。\n\n输出：\n" + result.output_path + L"\n\n大小：" +
                           human_size(summary.bag_size) + L"\n帧：" + std::to_wstring(summary.color_frames) +
                           L" color / " + std::to_wstring(summary.depth_frames) + L" depth / " +
                           std::to_wstring(summary.ir_frames) + L" IR";
    MessageBoxW(hwnd, message.c_str(), L"bag2mkv", MB_ICONINFORMATION | MB_OK);
    if (close_when_done) {
        DestroyWindow(hwnd);
    }
}

void App::on_error(const std::string &error) {
    if (worker.joinable()) worker.join();
    batch_mode = false;
    set_running(false);
    cancel = false;
    set_status(L"转换失败");
    append_log(L"错误: " + widen(error));
    MessageBoxW(hwnd, (L"转换失败：\n\n" + widen(error)).c_str(), L"bag2mkv", MB_ICONERROR | MB_OK);
    if (close_when_done) {
        DestroyWindow(hwnd);
    }
}

void App::on_cancelled() {
    if (worker.joinable()) worker.join();
    batch_mode = false;
    set_running(false);
    cancel = false;
    reset_progress();
    set_status(L"已取消");
    append_log(L"已取消；临时文件已清理。");
    if (close_when_done) {
        DestroyWindow(hwnd);
    }
}

void App::on_command(int id, int code) {
    switch (id) {
        case ID_BROWSE_INPUT:
            if (code == BN_CLICKED) browse_input();
            break;
        case ID_BROWSE_OUTPUT:
            if (code == BN_CLICKED) browse_output();
            break;
        case ID_BROWSE_CALIB:
            if (code == BN_CLICKED) browse_calibration();
            break;
        case ID_CLEAR_CALIB:
            if (code == BN_CLICKED) SetWindowTextW(hCalib, L"");
            break;
        case ID_START:
            if (code == BN_CLICKED) start_conversion();
            break;
        case ID_CANCEL:
            if (code == BN_CLICKED) request_cancel();
            break;
        case ID_BATCH:
            if (code == BN_CLICKED) start_batch();
            break;
        case ID_TO_BAG:
            if (code == BN_CLICKED) start_mkv2bag();
            break;
        case ID_OPEN:
            if (code == BN_CLICKED) open_output_folder();
            break;
        case ID_COLOR:
            if (code == CBN_SELCHANGE) {
                bool bgra = SendMessageW(hColor, CB_GETCURSEL, 0, 0) == 0;
                EnableWindow(hPreserve, bgra && !running);
            }
            break;
        default:
            break;
    }
}

void App::on_drop(HDROP drop) {
    if (running) {
        DragFinish(drop);
        return;
    }
    wchar_t file[MAX_PATH] = {};
    if (DragQueryFileW(drop, 0, file, MAX_PATH) > 0) {
        std::filesystem::path dropped(file);
        if (std::filesystem::is_directory(dropped)) {
            SetWindowTextW(hInput, file);
        } else {
            SetWindowTextW(hInput, file);
            std::wstring old = get_text(hOutput);
            if (old.empty() || old == suggested_output(get_text(hInput))) {
                SetWindowTextW(hOutput, suggested_output(file).c_str());
            }
        }
    }
    DragFinish(drop);
}

void App::on_close() {
    if (running) {
        int answer = MessageBoxW(hwnd, L"正在转换，取消并退出吗？", L"bag2mkv",
                                 MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        close_when_done = true;
        request_cancel();
        return;
    }
    DestroyWindow(hwnd);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            app = reinterpret_cast<App *>(create->lpCreateParams);
            app->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->create_controls();
            app->layout();
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        }
        case WM_SIZE:
            if (app) app->layout();
            return 0;
        case WM_COMMAND:
            if (app) app->on_command(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_DROPFILES:
            if (app) app->on_drop(reinterpret_cast<HDROP>(wparam));
            return 0;
        case WM_APP_PROGRESS: {
            std::unique_ptr<ProgressInfo> info(reinterpret_cast<ProgressInfo *>(lparam));
            if (app && info) app->on_progress(*info);
            return 0;
        }
        case WM_APP_LOG: {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring *>(lparam));
            if (app && line) app->append_log(*line);
            return 0;
        }
        case WM_APP_DONE: {
            std::unique_ptr<ConvertSummary> summary(reinterpret_cast<ConvertSummary *>(lparam));
            if (app && summary) app->on_done(*summary);
            return 0;
        }
        case WM_APP_ERROR: {
            std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring *>(lparam));
            if (app && error) app->on_error(narrow(*error));
            return 0;
        }
        case WM_APP_BATCH_DONE: {
            std::unique_ptr<BatchResult> result(reinterpret_cast<BatchResult *>(lparam));
            if (app && result) app->on_batch_done(*result);
            return 0;
        }
        case WM_APP_TO_BAG_DONE: {
            std::unique_ptr<ToBagResult> result(reinterpret_cast<ToBagResult *>(lparam));
            if (app && result) app->on_to_bag_done(*result);
            return 0;
        }
        case WM_APP_CANCELLED:
            if (app) app->on_cancelled();
            return 0;
        case WM_CLOSE:
            if (app) app->on_close();
            return 0;
        case WM_DESTROY:
            if (app) {
                if (app->worker.joinable()) {
                    app->cancel = true;
                    app->worker.join();
                }
                if (app->font) DeleteObject(app->font);
            }
            PostQuitMessage(0);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_GETMINMAXINFO: {
            auto *mmi = reinterpret_cast<MINMAXINFO *>(lparam);
            mmi->ptMinTrackSize.x = app ? app->S(700) : 700;
            mmi->ptMinTrackSize.y = app ? app->S(480) : 480;
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    App app;
    app.instance = instance;
    HDC screen = GetDC(nullptr);
    app.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    if (app.dpi <= 0) app.dpi = 96;

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        metrics.lfMessageFont.lfHeight = -MulDiv(9, app.dpi, 72);
        app.font = CreateFontIndirectW(&metrics.lfMessageFont);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"Bag2MkvGuiWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"窗口类注册失败。", L"bag2mkv", MB_ICONERROR | MB_OK);
        return 1;
    }

    std::wstring window_title = L"bag2mkv v" + widen(version()) + L" - Orbbec bag 与 K4A MKV 互转";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, window_title.c_str(),
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                app.S(760), app.S(520), nullptr, nullptr, instance, &app);
    if (!hwnd) {
        MessageBoxW(nullptr, L"窗口创建失败。", L"bag2mkv", MB_ICONERROR | MB_OK);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Optional launch arguments:
    //   [input.bag] [output.mkv] [--mjpg] [--start]
    //   --batch <input-dir> --batch-output <output-dir>
    //   --to-bag <input.mkv> [--to-bag-output <output.bag>]
    // These also make "Open with" and scripted smoke tests possible.
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        std::vector<std::wstring> positional;
        std::wstring batch_input;
        std::wstring batch_output;
        std::wstring to_bag_input;
        std::wstring to_bag_output;
        bool mjpg = false;
        bool autostart = false;
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--start") {
                autostart = true;
            } else if (arg == L"--mjpg" || arg == L"--color-mjpg") {
                mjpg = true;
            } else if (arg == L"--batch" && i + 1 < argc) {
                batch_input = argv[++i];
            } else if (arg == L"--batch-output" && i + 1 < argc) {
                batch_output = argv[++i];
            } else if (arg == L"--to-bag" && i + 1 < argc) {
                to_bag_input = argv[++i];
            } else if (arg == L"--to-bag-output" && i + 1 < argc) {
                to_bag_output = argv[++i];
            } else if (arg.rfind(L"--", 0) == 0) {
                // ignore unknown GUI switches
            } else {
                positional.push_back(arg);
            }
        }
        if (!positional.empty()) {
            SetWindowTextW(app.hInput, positional[0].c_str());
            std::wstring out = positional.size() > 1 ? positional[1] : app.suggested_output(positional[0]);
            SetWindowTextW(app.hOutput, out.c_str());
        }
        if (mjpg) {
            SendMessageW(app.hColor, CB_SETCURSEL, 1, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_COLOR, CBN_SELCHANGE), 0);
        }
        LocalFree(argv);
        if (!to_bag_input.empty()) {
            if (to_bag_output.empty()) {
                std::filesystem::path output(to_bag_input);
                output.replace_extension(".bag");
                to_bag_output = output.wstring();
            }
            app.start_mkv2bag_paths(to_bag_input, to_bag_output);
        } else if (!batch_input.empty() && !batch_output.empty()) {
            app.start_directory_batch(batch_input, batch_output);
        } else if (autostart && !positional.empty()) {
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_START, BN_CLICKED), 0);
        }
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
