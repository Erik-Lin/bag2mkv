#include "gui_backend.hpp"

#include "converter.hpp"
#include "errors.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {

using namespace bag2mkv;
using namespace bag2mkv::gui;
namespace fs = std::filesystem;

constexpr int ID_SOURCE_EDIT = 3001;
constexpr int ID_OUTPUT_EDIT = 3002;
constexpr int ID_CALIB_EDIT = 3003;
constexpr int ID_SOURCE_FILE = 3010;
constexpr int ID_SOURCE_FOLDER = 3011;
constexpr int ID_OUTPUT_BROWSE = 3012;
constexpr int ID_CALIB_BROWSE = 3013;
constexpr int ID_START = 3020;
constexpr int ID_CANCEL = 3021;
constexpr int ID_OPEN = 3022;
constexpr int ID_CLEAR = 3023;
constexpr int ID_LOG = 3030;

constexpr UINT WM_APP_PROGRESS = WM_APP + 20;
constexpr UINT WM_APP_LOG = WM_APP + 21;
constexpr UINT WM_APP_DONE = WM_APP + 22;

constexpr COLORREF kBg = RGB(8, 12, 20);
constexpr COLORREF kSidebar = RGB(11, 17, 28);
constexpr COLORREF kCard = RGB(17, 24, 38);
constexpr COLORREF kCardAlt = RGB(21, 30, 47);
constexpr COLORREF kField = RGB(11, 17, 27);
constexpr COLORREF kBorder = RGB(35, 48, 70);
constexpr COLORREF kBorderSoft = RGB(27, 38, 56);
constexpr COLORREF kText = RGB(242, 247, 255);
constexpr COLORREF kMuted = RGB(143, 159, 183);
constexpr COLORREF kFaint = RGB(91, 108, 132);
constexpr COLORREF kAccent = RGB(74, 134, 255);
constexpr COLORREF kAccentSoft = RGB(37, 67, 128);
constexpr COLORREF kTeal = RGB(45, 212, 191);
constexpr COLORREF kGreen = RGB(74, 222, 128);
constexpr COLORREF kAmber = RGB(251, 191, 36);
constexpr COLORREF kRed = RGB(248, 113, 113);
constexpr COLORREF kWhite = RGB(255, 255, 255);

constexpr UINT_PTR kDwmDark = 20;

enum class View {
    Single,
    Batch,
    About,
};

enum class UiState {
    Ready,
    Running,
    Success,
    Error,
    Cancelled,
};

struct ProgressPayload {
    int percent = 0;
    int current = 0;
    int total = 0;
    std::wstring stage;
};

struct DonePayload {
    int succeeded = 0;
    int failed = 0;
    bool cancelled = false;
    std::wstring message;
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
    std::wstring out(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) GetWindowTextW(hwnd, out.data(), len + 1);
    out.resize(static_cast<size_t>(len));
    return out;
}

void set_text(HWND hwnd, const std::wstring &text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::wstring human_size(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        swprintf_s(buf, L"%.2f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        swprintf_s(buf, L"%.1f MB", double(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        swprintf_s(buf, L"%.1f KB", double(bytes) / 1024.0);
    } else {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

std::wstring file_name(const fs::path &path) {
    const std::wstring name = path.filename().wstring();
    return name.empty() ? path.wstring() : name;
}

std::wstring pick_file(HWND owner, const wchar_t *title) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter =
        L"Supported recordings\0*.bag;*.mkv\0"
        L"ROSBAG / Orbbec (*.bag)\0*.bag\0"
        L"K4A recording (*.mkv)\0*.mkv\0"
        L"All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return {};
    return path;
}

std::wstring pick_save(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *extension) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return {};
    return path;
}

std::wstring pick_folder(HWND owner, const wchar_t *title) {
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE idlist = SHBrowseForFolderW(&info);
    if (!idlist) return {};
    wchar_t path[MAX_PATH] = {};
    std::wstring result;
    if (SHGetPathFromIDListW(idlist, path)) result = path;
    CoTaskMemFree(idlist);
    return result;
}

bool is_subpath(const fs::path &child, const fs::path &parent) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(child, ec);
    if (ec) c = child;
    fs::path p = fs::weakly_canonical(parent, ec);
    if (ec) p = parent;
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

std::wstring now_time() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring ellipsize(const std::wstring &text, size_t max_chars) {
    if (text.size() <= max_chars) return text;
    return text.substr(0, max_chars > 3 ? max_chars - 3 : 0) + L"...";
}

class StudioApp {
public:
    explicit StudioApp(HWND hwnd) : hwnd_(hwnd) {}
    ~StudioApp();

    bool create();
    LRESULT handle(UINT msg, WPARAM wparam, LPARAM lparam);
    void load_path(const std::wstring &path);
    void load_batch(const std::wstring &input_dir, const std::wstring &output_dir);
    void select_target(const std::wstring &target);
    void set_output_path(const std::wstring &path);
    void show_about();
    void begin();

private:
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    double ui_scale_ = 1.0;
    HFONT font_display_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_h2_ = nullptr;
    HFONT font_body_ = nullptr;
    HFONT font_body_bold_ = nullptr;
    HFONT font_small_ = nullptr;
    HFONT font_micro_ = nullptr;
    HFONT font_mono_ = nullptr;

    HBRUSH edit_brush_ = nullptr;
    HBRUSH log_brush_ = nullptr;

    HWND source_edit_ = nullptr;
    HWND output_edit_ = nullptr;
    HWND calib_edit_ = nullptr;
    HWND log_edit_ = nullptr;
    HWND source_file_ = nullptr;
    HWND source_folder_ = nullptr;
    HWND output_browse_ = nullptr;
    HWND calib_browse_ = nullptr;
    HWND start_ = nullptr;
    HWND cancel_ = nullptr;
    HWND open_ = nullptr;
    HWND clear_ = nullptr;

    View view_ = View::Single;
    UiState state_ = UiState::Ready;
    InputKind input_kind_ = InputKind::None;
    OutputKind output_kind_ = OutputKind::K4aMkv;
    std::wstring source_path_;
    std::wstring output_path_;
    std::wstring calib_path_;
    std::wstring stage_ = L"Ready when you are";
    std::wstring message_;
    std::wstring batch_error_;
    std::vector<SourceItem> batch_items_;
    std::wstring batch_root_;
    InputKind batch_kind_ = InputKind::None;
    bool batch_scanned_ = false;

    bool verify_ = false;
    bool overwrite_ = false;
    bool preserve_ = true;
    bool no_calibration_ = false;
    bool color_mjpg_ = false;

    bool busy_ = false;
    std::atomic<bool> cancel_requested_{false};
    std::thread worker_;
    int progress_ = 0;

    bool tracking_mouse_ = false;
    int hover_target_ = -1;
    int hover_color_ = -1;
    int hover_toggle_ = -1;
    int hover_nav_ = -1;
    bool hover_link_ = false;

    std::array<RECT, 4> target_rects_{};
    std::array<RECT, 2> color_rects_{};
    std::array<RECT, 4> toggle_rects_{};
    std::array<RECT, 3> nav_rects_{};
    RECT link_rect_{};
    RECT source_card_{};
    RECT output_card_{};
    RECT options_card_{};
    RECT command_row_{};
    RECT activity_card_{};

    int S(int value) const { return static_cast<int>(std::lround(value * ui_scale_)); }
    void create_fonts();
    void destroy_fonts();
    void apply_fonts();
    void create_controls();
    void layout();
    void paint(HDC hdc);
    void paint_sidebar(HDC hdc, const RECT &client);
    void paint_header(HDC hdc, const RECT &client);
    void paint_source(HDC hdc);
    void paint_output(HDC hdc);
    void paint_options(HDC hdc);
    void paint_command(HDC hdc);
    void paint_activity(HDC hdc);
    void paint_about(HDC hdc, const RECT &client);
    void paint_button(const DRAWITEMSTRUCT *item);
    void draw_card(HDC hdc, const RECT &rect);
    void draw_field(HDC hdc, const RECT &rect, HWND edit);
    void draw_text(HDC hdc, const std::wstring &text, RECT rect, HFONT font, COLORREF color, UINT flags);
    void draw_round(HDC hdc, RECT rect, int radius, COLORREF fill, COLORREF border, int border_width = 1);
    void draw_chip(HDC hdc, RECT rect, const std::wstring &label, COLORREF color);
    void draw_toggle(HDC hdc, RECT rect, const std::wstring &label, bool checked, bool enabled);
    void draw_segment(HDC hdc, RECT rect, const std::wstring &label, bool selected, bool enabled,
                      bool hovered);
    void update_controls();
    void update_state(UiState state, const std::wstring &stage);
    void set_view(View view);
    void on_source_committed();
    void set_source(const std::wstring &path);
    void refresh_batch(const std::wstring &path);
    void set_output_kind(OutputKind kind);
    void browse_source_file();
    void browse_source_folder();
    void browse_output();
    void browse_calibration();
    void start();
    void cancel();
    void open_output();
    void append_log(const std::wstring &line);
    void clear_log();
    void run_jobs(std::vector<ConversionJob> jobs);
    void on_done(const DonePayload &done);
    void on_progress(const ProgressPayload &progress);
    bool calibration_applicable() const;
    bool color_applicable() const;
    bool verify_applicable() const;
    bool preserve_applicable() const;
    bool no_calibration_applicable() const;
    std::wstring detected_text() const;
    std::wstring start_label() const;
    std::wstring status_text() const;
    COLORREF status_color() const;
    void browse_for_output();
    std::wstring source_for_use() const;
    std::wstring output_for_use() const;
    void commit_source_from_edit();
    void refresh_hover(int x, int y);
    void on_click(int x, int y);
    bool point_in(const RECT &rect, int x, int y) const {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    }
};

StudioApp::~StudioApp() {
    cancel_requested_ = true;
    if (worker_.joinable()) worker_.join();
    destroy_fonts();
    if (edit_brush_) DeleteObject(edit_brush_);
    if (log_brush_) DeleteObject(log_brush_);
}

void StudioApp::create_fonts() {
    destroy_fonts();
    const int d = std::max(72, static_cast<int>(std::lround(96.0 * ui_scale_)));
    auto make = [d](int pt, int weight, const wchar_t *face) {
        return CreateFontW(-MulDiv(pt, d, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           face);
    };
    font_display_ = make(27, FW_BOLD, L"Segoe UI");
    font_title_ = make(20, FW_BOLD, L"Segoe UI");
    font_h2_ = make(12, FW_SEMIBOLD, L"Segoe UI");
    font_body_ = make(10, FW_NORMAL, L"Segoe UI");
    font_body_bold_ = make(10, FW_SEMIBOLD, L"Segoe UI");
    font_small_ = make(9, FW_NORMAL, L"Segoe UI");
    font_micro_ = make(8, FW_SEMIBOLD, L"Segoe UI");
    font_mono_ = make(9, FW_NORMAL, L"Cascadia Mono");
    if (!font_mono_) font_mono_ = make(9, FW_NORMAL, L"Consolas");
}

void StudioApp::apply_fonts() {
    for (HWND h : {source_edit_, output_edit_, calib_edit_, log_edit_}) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(h == log_edit_ ? font_mono_ : font_body_), TRUE);
    }
    for (HWND h : {source_file_, source_folder_, output_browse_, calib_browse_, start_, cancel_, open_, clear_}) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_bold_), TRUE);
    }
}

void StudioApp::destroy_fonts() {
    for (HFONT *font : {&font_display_, &font_title_, &font_h2_, &font_body_, &font_body_bold_, &font_small_,
                        &font_micro_, &font_mono_}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

bool StudioApp::create() {
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) dpi_ = 96;
    create_fonts();
    edit_brush_ = CreateSolidBrush(kField);
    log_brush_ = CreateSolidBrush(kField);
    create_controls();
    layout();
    DragAcceptFiles(hwnd_, TRUE);
    return true;
}

void StudioApp::create_controls() {
    auto edit = [this](int id, DWORD style) {
        HWND h = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(12), S(12)));
        SendMessageW(h, EM_SETLIMITTEXT, 32767, 0);
        SendMessageW(h, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(id == ID_SOURCE_EDIT
                                                  ? L"Drop a .bag, .mkv, or ROS 2 bag folder here"
                                                  : id == ID_OUTPUT_EDIT ? L"Choose a destination"
                                                                         : L"Optional calibration.json"));
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_), TRUE);
        return h;
    };
    source_edit_ = edit(ID_SOURCE_EDIT, ES_AUTOHSCROLL);
    output_edit_ = edit(ID_OUTPUT_EDIT, ES_AUTOHSCROLL);
    calib_edit_ = edit(ID_CALIB_EDIT, ES_AUTOHSCROLL);
    log_edit_ = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LOG)), nullptr,
                                nullptr);
    SendMessageW(log_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(12), S(10)));
    SendMessageW(log_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
    SendMessageW(log_edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Activity will appear here."));

    auto button = [this](int id, const wchar_t *text) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_bold_), TRUE);
        return h;
    };
    source_file_ = button(ID_SOURCE_FILE, L"File…");
    source_folder_ = button(ID_SOURCE_FOLDER, L"Folder…");
    output_browse_ = button(ID_OUTPUT_BROWSE, L"Browse");
    calib_browse_ = button(ID_CALIB_BROWSE, L"…");
    start_ = button(ID_START, L"Convert now");
    cancel_ = button(ID_CANCEL, L"Cancel");
    open_ = button(ID_OPEN, L"Open output");
    clear_ = button(ID_CLEAR, L"Clear");
}

void StudioApp::layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const double target_scale = std::clamp(
        std::min({static_cast<double>(dpi_) / 96.0, static_cast<double>(client.right) / 1240.0,
                  static_cast<double>(client.bottom) / 840.0}),
        0.60, 1.35);
    if (std::fabs(target_scale - ui_scale_) > 0.01) {
        ui_scale_ = target_scale;
        create_fonts();
        apply_fonts();
    }
    const int sidebar = S(248);
    const int left = sidebar + S(38);
    const int right = S(32);
    const int content_w = std::max(S(640), static_cast<int>(client.right) - left - right);

    const int nav_top = S(132);
    for (int i = 0; i < 3; ++i) {
        nav_rects_[i] = {S(18), nav_top + i * S(48), sidebar - S(18), nav_top + (i + 1) * S(48) - S(4)};
    }

    const int card_x = left;
    const int card_w = content_w;
    source_card_ = {card_x, S(112), card_x + card_w, S(268)};
    output_card_ = {card_x, S(280), card_x + card_w, S(448)};
    options_card_ = {card_x, S(460), card_x + card_w, S(600)};
    command_row_ = {card_x, S(612), card_x + card_w, S(650)};
    activity_card_ = {card_x, S(662), card_x + card_w,
                      std::max(S(720), static_cast<int>(client.bottom) - S(16))};

    // Source row.
    const int source_field_y = source_card_.top + S(64);
    const int source_field_h = S(38);
    int source_x = source_card_.left + S(24);
    const int source_right = source_card_.right - S(24);
    const int folder_w = S(92);
    const int file_w = S(84);
    const int gap = S(8);
    int edit_right = source_right;
    if (view_ == View::Single) edit_right -= file_w + gap;
    edit_right -= folder_w + gap;
    SetWindowPos(source_edit_, nullptr, source_x, source_field_y, std::max(S(100), edit_right - source_x),
                 source_field_h, SWP_NOZORDER);
    if (view_ == View::Single) {
        SetWindowPos(source_file_, nullptr, source_right - file_w - folder_w - gap * 2, source_field_y, file_w,
                     source_field_h, SWP_NOZORDER);
    }
    SetWindowPos(source_folder_, nullptr, source_right - folder_w, source_field_y, folder_w, source_field_h,
                 SWP_NOZORDER);

    // Output format segments.
    const int seg_y = output_card_.top + S(48);
    const int seg_h = S(40);
    const int seg_gap = S(8);
    const int seg_w = (card_w - S(48) - seg_gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        target_rects_[i] = {output_card_.left + S(24) + i * (seg_w + seg_gap), seg_y,
                            output_card_.left + S(24) + i * (seg_w + seg_gap) + seg_w, seg_y + seg_h};
    }
    const int output_y = output_card_.top + S(112);
    const int output_browse_w = S(92);
    SetWindowPos(output_edit_, nullptr, output_card_.left + S(24), output_y,
                 card_w - S(48) - output_browse_w - S(8), S(38), SWP_NOZORDER);
    SetWindowPos(output_browse_, nullptr, output_card_.right - S(24) - output_browse_w, output_y, output_browse_w,
                 S(38), SWP_NOZORDER);

    // Options.
    const int color_y = options_card_.top + S(48);
    color_rects_[0] = {options_card_.left + S(24), color_y, options_card_.left + S(100), color_y + S(32)};
    color_rects_[1] = {options_card_.left + S(100), color_y, options_card_.left + S(176), color_y + S(32)};
    const int toggle_y = color_y;
    toggle_rects_[0] = {options_card_.left + S(210), toggle_y, options_card_.left + S(310), toggle_y + S(32)};
    toggle_rects_[1] = {options_card_.left + S(326), toggle_y, options_card_.left + S(448), toggle_y + S(32)};
    toggle_rects_[2] = {options_card_.left + S(464), toggle_y, options_card_.left + S(612), toggle_y + S(32)};
    toggle_rects_[3] = {options_card_.left + S(628), toggle_y, options_card_.left + S(780), toggle_y + S(32)};

    const int calib_y = options_card_.top + S(102);
    const int calib_browse_w = S(40);
    SetWindowPos(calib_edit_, nullptr, options_card_.left + S(142), calib_y,
                 std::min(S(420), card_w - S(190) - calib_browse_w), S(34), SWP_NOZORDER);
    SetWindowPos(calib_browse_, nullptr,
                 options_card_.left + S(142) + std::min(S(420), card_w - S(190) - calib_browse_w) + S(8), calib_y,
                 calib_browse_w, S(34), SWP_NOZORDER);

    // Command row.
    const int button_h = S(38);
    const int start_w = S(156);
    const int cancel_w = S(96);
    const int open_w = S(118);
    const int bx = command_row_.right;
    SetWindowPos(start_, nullptr, bx - start_w, command_row_.top, start_w, button_h, SWP_NOZORDER);
    SetWindowPos(cancel_, nullptr, bx - start_w - S(8) - cancel_w, command_row_.top, cancel_w, button_h,
                 SWP_NOZORDER);
    SetWindowPos(open_, nullptr, bx - start_w - S(8) - cancel_w - S(8) - open_w, command_row_.top, open_w, button_h,
                 SWP_NOZORDER);

    // Activity.
    SetWindowPos(clear_, nullptr, activity_card_.right - S(24) - S(68), activity_card_.top + S(12), S(68), S(28),
                 SWP_NOZORDER);
    SetWindowPos(log_edit_, nullptr, activity_card_.left + S(24), activity_card_.top + S(48),
                 activity_card_.right - activity_card_.left - S(48),
                 std::max(S(80), static_cast<int>(activity_card_.bottom) - static_cast<int>(activity_card_.top) - S(64)),
                 SWP_NOZORDER);
    update_controls();
}

void StudioApp::draw_text(HDC hdc, const std::wstring &text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    if (!font) return;
    HGDIOBJ old = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(hdc, old);
}

void StudioApp::draw_round(HDC hdc, RECT rect, int radius, COLORREF fill, COLORREF border, int border_width) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, border_width, border);
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void StudioApp::draw_card(HDC hdc, const RECT &rect) {
    draw_round(hdc, rect, S(14), kCard, kBorderSoft);
}

void StudioApp::draw_field(HDC hdc, const RECT &rect, HWND edit) {
    RECT field = rect;
    InflateRect(&field, S(1), S(1));
    const bool focus = GetFocus() == edit;
    draw_round(hdc, field, S(8), kField, focus ? kAccent : kBorder, focus ? 2 : 1);
}

void StudioApp::draw_chip(HDC hdc, RECT rect, const std::wstring &label, COLORREF color) {
    draw_round(hdc, rect, rect.bottom - rect.top, kCardAlt, kBorderSoft);
    HBRUSH dot = CreateSolidBrush(color);
    HGDIOBJ old = SelectObject(hdc, dot);
    Ellipse(hdc, rect.left + S(11), rect.top + (rect.bottom - rect.top) / 2 - S(3),
            rect.left + S(17), rect.top + (rect.bottom - rect.top) / 2 + S(3));
    SelectObject(hdc, old);
    DeleteObject(dot);
    RECT text = rect;
    text.left += S(24);
    draw_text(hdc, label, text, font_small_, color, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void StudioApp::draw_segment(HDC hdc, RECT rect, const std::wstring &label, bool selected, bool enabled,
                             bool hovered) {
    COLORREF fill = selected ? kAccent : (hovered && enabled ? kCardAlt : kField);
    COLORREF border = selected ? kAccent : (hovered && enabled ? kAccentSoft : kBorderSoft);
    COLORREF color = selected ? kWhite : (enabled ? kMuted : kFaint);
    draw_round(hdc, rect, S(8), fill, border);
    draw_text(hdc, label, rect, selected ? font_body_bold_ : font_body_, color,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void StudioApp::draw_toggle(HDC hdc, RECT rect, const std::wstring &label, bool checked, bool enabled) {
    const int pill_w = S(34);
    const int pill_h = S(20);
    RECT pill{rect.left, rect.top + (rect.bottom - rect.top - pill_h) / 2,
              rect.left + pill_w, rect.top + (rect.bottom - rect.top + pill_h) / 2};
    COLORREF fill = checked ? (enabled ? kAccent : kAccentSoft) : kField;
    COLORREF border = checked ? fill : kBorder;
    draw_round(hdc, pill, pill_h, fill, border);
    const int knob = pill_h - S(4);
    RECT dot{checked ? pill.right - knob - S(2) : pill.left + S(2), pill.top + S(2),
             checked ? pill.right - S(2) : pill.left + knob + S(2), pill.top + S(2) + knob};
    HBRUSH brush = CreateSolidBrush(enabled ? kWhite : kMuted);
    HGDIOBJ old = SelectObject(hdc, brush);
    HGDIOBJ pen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(hdc, old);
    SelectObject(hdc, pen);
    DeleteObject(brush);
    RECT text = rect;
    text.left += pill_w + S(9);
    draw_text(hdc, label, text, font_small_, enabled ? (checked ? kText : kMuted) : kFaint,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void StudioApp::paint(HDC target) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ old = SelectObject(mem, bitmap);
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(mem, &client, bg);
    DeleteObject(bg);

    paint_sidebar(mem, client);
    if (view_ == View::About) {
        paint_about(mem, client);
    } else {
        paint_header(mem, client);
        paint_source(mem);
        paint_output(mem);
        paint_options(mem);
        paint_command(mem);
        paint_activity(mem);
    }

    BitBlt(target, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

void StudioApp::paint_sidebar(HDC hdc, const RECT &client) {
    RECT side{0, 0, S(248), client.bottom};
    HBRUSH b = CreateSolidBrush(kSidebar);
    FillRect(hdc, &side, b);
    DeleteObject(b);
    HBRUSH line = CreateSolidBrush(kBorderSoft);
    RECT edge{S(247), 0, S(248), client.bottom};
    FillRect(hdc, &edge, line);
    DeleteObject(line);

    draw_round(hdc, {S(22), S(24), S(62), S(64)}, S(11), kAccent, kAccent);
    draw_text(hdc, L"b2", {S(22), S(24), S(62), S(64)}, font_h2_, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    draw_text(hdc, L"bag2mkv", {S(76), S(24), S(220), S(46)}, font_title_, kText,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    draw_text(hdc, L"RGB-D  STUDIO", {S(77), S(45), S(220), S(62)}, font_micro_, kMuted,
              DT_SINGLELINE | DT_VCENTER);

    const wchar_t *labels[3] = {L"Single conversion", L"Batch workspace", L"About"};
    for (int i = 0; i < 3; ++i) {
        const bool selected = static_cast<int>(view_) == i;
        if (selected) {
            draw_round(hdc, nav_rects_[i], S(8), kCardAlt, kBorderSoft);
            HBRUSH accent = CreateSolidBrush(kAccent);
            RECT bar{nav_rects_[i].left, nav_rects_[i].top + S(10), nav_rects_[i].left + S(3),
                     nav_rects_[i].bottom - S(10)};
            FillRect(hdc, &bar, accent);
            DeleteObject(accent);
        } else if (hover_nav_ == i) {
            draw_round(hdc, nav_rects_[i], S(8), kCard, kBorderSoft);
        }
        RECT nr = nav_rects_[i];
        nr.left += S(18);
        draw_text(hdc, labels[i], nr, selected ? font_body_bold_ : font_body_, selected ? kText : kMuted,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    const int bottom = client.bottom - S(76);
    draw_text(hdc, L"v0.6.0", {S(24), bottom, S(120), bottom + S(20)}, font_small_, kMuted,
              DT_SINGLELINE | DT_VCENTER);
    draw_text(hdc, L"MIT License · Windows x64", {S(24), bottom + S(18), S(220), bottom + S(38)}, font_micro_,
              kFaint, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT status_dot{S(24), bottom + S(48), S(30), bottom + S(54)};
    HBRUSH dot = CreateSolidBrush(kTeal);
    HGDIOBJ old = SelectObject(hdc, dot);
    Ellipse(hdc, status_dot.left, status_dot.top, status_dot.right, status_dot.bottom);
    SelectObject(hdc, old);
    DeleteObject(dot);
    draw_text(hdc, L"Local · private · lossless", {S(38), bottom + S(42), S(230), bottom + S(62)}, font_micro_,
              kFaint, DT_SINGLELINE | DT_VCENTER);
}

void StudioApp::paint_header(HDC hdc, const RECT &) {
    const int x = S(286);
    const wchar_t *crumbs = view_ == View::Single ? L"WORKSPACE  /  SINGLE" : L"WORKSPACE  /  BATCH";
    draw_text(hdc, crumbs, {x, S(30), S(700), S(48)}, font_micro_, kFaint, DT_SINGLELINE | DT_VCENTER);
    draw_text(hdc, view_ == View::Single ? L"Conversion Studio" : L"Batch Workspace",
              {x, S(52), S(1000), S(88)}, font_display_, kText, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    draw_text(hdc,
              view_ == View::Single
                  ? L"Move RGB-D data between Orbbec, K4A and ROS — without losing a bit."
                  : L"Convert a whole recording session with one consistent target format.",
              {x, S(90), S(1000), S(112)}, font_small_, kMuted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT chip{S(1160), S(36), S(1230), S(66)};
    RECT client{};
    GetClientRect(hwnd_, &client);
    chip.right = client.right - S(32);
    chip.left = chip.right - S(164);
    draw_chip(hdc, chip, status_text(), status_color());
}

void StudioApp::paint_source(HDC hdc) {
    draw_card(hdc, source_card_);
    const int x = source_card_.left + S(24);
    const int y = source_card_.top;
    draw_text(hdc, view_ == View::Single ? L"SOURCE" : L"SOURCE FOLDER", {x, y + S(18), x + S(300), y + S(34)},
              font_micro_, kAccent, DT_SINGLELINE | DT_VCENTER);
    draw_text(hdc,
              view_ == View::Single ? L"One recording or a ROS 2 bag folder"
                                    : L"Every .bag, .mkv and ROS 2 bag below this folder",
              {x, y + S(36), source_card_.right - S(24), y + S(54)}, font_small_, kMuted,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT edit_rect{source_card_.left + S(24), source_card_.top + S(64),
                   source_card_.right - S(24) - (view_ == View::Single ? S(84 + 92 + 16) : S(92 + 8)),
                   source_card_.top + S(102)};
    draw_field(hdc, edit_rect, source_edit_);

    const int detected_y = source_card_.top + S(114);
    const std::wstring detected = detected_text();
    COLORREF color = input_kind_ == InputKind::None ? (batch_error_.empty() ? kMuted : kRed) : kTeal;
    RECT chip{x, detected_y, x + S(296), detected_y + S(26)};
    draw_chip(hdc, chip, detected, color);
    draw_text(hdc, view_ == View::Single ? L"Drag & drop supported" : L"Recursive scan · mirrored output",
              {x + S(308), detected_y, source_card_.right - S(24), detected_y + S(26)}, font_micro_, kFaint,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void StudioApp::paint_output(HDC hdc) {
    draw_card(hdc, output_card_);
    const int x = output_card_.left + S(24);
    draw_text(hdc, L"OUTPUT FORMAT", {x, output_card_.top + S(18), x + S(300), output_card_.top + S(34)},
              font_micro_, kAccent, DT_SINGLELINE | DT_VCENTER);
    draw_text(hdc, L"Unsupported combinations are disabled automatically.",
              {x, output_card_.top + S(36), output_card_.right - S(24), output_card_.top + S(52)}, font_small_,
              kMuted, DT_SINGLELINE | DT_VCENTER);

    const std::array<OutputKind, 4> kinds{OutputKind::K4aMkv, OutputKind::Ros1Bag, OutputKind::Ros2Bag,
                                         OutputKind::OrbbecBag};
    for (int i = 0; i < 4; ++i) {
        const bool enabled = output_supported(input_kind_, kinds[i]);
        draw_segment(hdc, target_rects_[i], output_kind_label(kinds[i]), output_kind_ == kinds[i] && enabled,
                     enabled, hover_target_ == i);
    }

    RECT output_field{output_card_.left + S(24), output_card_.top + S(112),
                      output_card_.right - S(24) - S(92) - S(8), output_card_.top + S(150)};
    draw_field(hdc, output_field, output_edit_);
    draw_text(hdc, output_kind_hint(output_kind_), {x, output_card_.top + S(156),
                                                    output_card_.right - S(24), output_card_.top + S(174)},
              font_micro_, kFaint, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void StudioApp::paint_options(HDC hdc) {
    draw_card(hdc, options_card_);
    const int x = options_card_.left + S(24);
    draw_text(hdc, L"OPTIONS", {x, options_card_.top + S(18), x + S(300), options_card_.top + S(34)}, font_micro_,
              kAccent, DT_SINGLELINE | DT_VCENTER);

    const bool color_ok = color_applicable();
    draw_segment(hdc, color_rects_[0], L"BGRA", !color_mjpg_ && color_ok, color_ok, hover_color_ == 0);
    draw_segment(hdc, color_rects_[1], L"MJPEG", color_mjpg_ && color_ok, color_ok, hover_color_ == 1);

    draw_toggle(hdc, toggle_rects_[0], L"Verify", verify_, verify_applicable());
    draw_toggle(hdc, toggle_rects_[1], L"Overwrite", overwrite_, true);
    draw_toggle(hdc, toggle_rects_[2], L"Preserve JPEG", preserve_, preserve_applicable());
    draw_toggle(hdc, toggle_rects_[3], L"No calibration", no_calibration_, no_calibration_applicable());
    draw_text(hdc, L"Calibration JSON", {x, options_card_.top + S(108), x + S(110), options_card_.top + S(130)},
              font_small_, calibration_applicable() ? kMuted : kFaint, DT_SINGLELINE | DT_VCENTER);
    if (calibration_applicable()) {
        RECT calib_field{options_card_.left + S(142), options_card_.top + S(102),
                         options_card_.left + S(142) +
                             std::min(S(420), static_cast<int>(output_card_.right - output_card_.left) - S(190)),
                         options_card_.top + S(136)};
        draw_field(hdc, calib_field, calib_edit_);
    } else {
        draw_text(hdc, L"Not used for this conversion path.", {options_card_.left + S(142), options_card_.top + S(108),
                                                               options_card_.right - S(24), options_card_.top + S(130)},
                  font_micro_, kFaint, DT_SINGLELINE | DT_VCENTER);
    }
}

void StudioApp::paint_command(HDC hdc) {
    const int x = command_row_.left;
    draw_text(hdc, stage_, {x, command_row_.top, command_row_.right - S(500), command_row_.top + S(20)},
              font_small_, state_ == UiState::Error ? kRed : kMuted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT bar{x, command_row_.top + S(24), command_row_.right - S(500), command_row_.top + S(30)};
    draw_round(hdc, bar, S(3), kField, kBorderSoft);
    if (progress_ > 0) {
        RECT fill = bar;
        fill.right = bar.left + (bar.right - bar.left) * progress_ / 100;
        draw_round(hdc, fill, S(3), state_ == UiState::Error ? kRed : kAccent, state_ == UiState::Error ? kRed : kAccent);
    }
    if (busy_) {
        wchar_t pct[16];
        swprintf_s(pct, L"%d%%", progress_);
        draw_text(hdc, pct, {bar.right + S(10), command_row_.top + S(18), bar.right + S(56), command_row_.top + S(36)},
                  font_micro_, kMuted, DT_SINGLELINE | DT_VCENTER);
    }
}

void StudioApp::paint_activity(HDC hdc) {
    draw_card(hdc, activity_card_);
    const int x = activity_card_.left + S(24);
    draw_text(hdc, L"ACTIVITY", {x, activity_card_.top + S(16), x + S(300), activity_card_.top + S(34)}, font_micro_,
              kAccent, DT_SINGLELINE | DT_VCENTER);
    RECT log_field{activity_card_.left + S(24), activity_card_.top + S(48), activity_card_.right - S(24),
                   activity_card_.bottom - S(16)};
    draw_round(hdc, log_field, S(8), kField, kBorderSoft);
}

void StudioApp::paint_about(HDC hdc, const RECT &client) {
    const int x = S(286);
    const int width = client.right - x - S(32);
    draw_text(hdc, L"ABOUT", {x, S(30), x + S(500), S(48)}, font_micro_, kAccent, DT_SINGLELINE | DT_VCENTER);
    draw_text(hdc, L"bag2mkv", {x, S(52), x + S(700), S(104)}, font_display_, kText,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    draw_text(hdc, L"v0.6.0  ·  MIT License  ·  Windows x64 / Linux x86_64 CLI",
              {x, S(108), x + S(850), S(130)}, font_small_, kMuted, DT_SINGLELINE | DT_VCENTER);
    draw_text(hdc,
              L"A deliberate, verifiable bridge between Orbbec ROSBAG, Azure Kinect MKV and ROS 1/2. "
              L"Color is never re-encoded. Depth is bit-exact. Every output can be verified.",
              {x, S(146), x + width, S(194)}, font_body_, kMuted, DT_WORDBREAK);

    const int card_w = (width - S(16) * 2) / 3;
    const int card_y = S(224);
    const int card_h = S(174);
    for (int i = 0; i < 3; ++i) {
        RECT card{x + i * (card_w + S(16)), card_y, x + i * (card_w + S(16)) + card_w, card_y + card_h};
        draw_card(hdc, card);
        const wchar_t *titles[3] = {L"LOSSLESS BY CONTRACT", L"EIGHT CONVERSION PATHS", L"VERIFY, DON'T TRUST"};
        const wchar_t *bodies[3] = {
            L"Original JPEG color, raw depth, timestamps and metadata are preserved. No hidden transcode.",
            L"Orbbec bag ↔ K4A MKV, plus ROS 1 and ROS 2 import/export. One consistent CLI and GUI.",
            L"Frame counts, timestamps and payload hashes can be checked independently after every conversion.",
        };
        draw_text(hdc, titles[i], {card.left + S(20), card.top + S(20), card.right - S(20), card.top + S(38)},
                  font_micro_, kAccent, DT_SINGLELINE | DT_VCENTER);
        draw_text(hdc, bodies[i], {card.left + S(20), card.top + S(52), card.right - S(20), card.bottom - S(20)},
                  font_small_, kMuted, DT_WORDBREAK);
    }

    RECT link_box{x, S(438), x + width, S(466)};
    draw_round(hdc, link_box, S(8), kCard, kBorderSoft);
    draw_text(hdc, L"github.com/Erik-Lin/bag2mkv", {x + S(16), S(438), x + width - S(16), S(466)}, font_body_bold_,
              kAccent, DT_SINGLELINE | DT_VCENTER);
    link_rect_ = {x, S(438), x + S(300), S(466)};
    draw_text(hdc, L"Built for RGB-D researchers, robotics engineers and anyone who refuses to guess.",
              {x, S(488), x + width, S(516)}, font_micro_, kFaint, DT_SINGLELINE | DT_VCENTER);
}

void StudioApp::paint_button(const DRAWITEMSTRUCT *item) {
    const bool primary = item->CtlID == ID_START;
    const bool danger = item->CtlID == ID_CANCEL;
    const bool ghost = item->CtlID == ID_OPEN || item->CtlID == ID_CLEAR;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    RECT r = item->rcItem;
    COLORREF fill = primary ? kAccent : (ghost ? kCard : kCardAlt);
    COLORREF border = primary ? kAccent : (danger ? RGB(91, 45, 53) : kBorder);
    COLORREF text = primary ? kWhite : (disabled ? kFaint : (danger ? kRed : kText));
    if (pressed && !disabled) fill = primary ? RGB(52, 108, 225) : kField;
    if (disabled) fill = kCard;
    HDC hdc = GetDC(item->hwndItem);
    draw_round(hdc, r, S(8), fill, border);
    draw_text(hdc, get_text(item->hwndItem), r, primary || danger ? font_body_bold_ : font_body_, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if ((item->itemState & ODS_FOCUS) && !disabled) {
        RECT focus = r;
        InflateRect(&focus, -S(4), -S(4));
        DrawFocusRect(hdc, &focus);
    }
    ReleaseDC(item->hwndItem, hdc);
}

void StudioApp::update_controls() {
    const bool show_content = view_ != View::About;
    for (HWND h : {source_edit_, output_edit_, calib_edit_, log_edit_, source_file_, source_folder_, output_browse_,
                   calib_browse_, start_, cancel_, open_, clear_}) {
        if (h) ShowWindow(h, show_content ? SW_SHOW : SW_HIDE);
    }
    if (view_ == View::Batch) ShowWindow(source_file_, SW_HIDE);
    if (!calibration_applicable()) {
        ShowWindow(calib_edit_, SW_HIDE);
        ShowWindow(calib_browse_, SW_HIDE);
    }
    if (view_ == View::About) return;
    SetWindowTextW(start_, start_label().c_str());
    EnableWindow(start_, !busy_ && input_kind_ != InputKind::None);
    EnableWindow(cancel_, busy_);
    EnableWindow(open_, !busy_ && !output_path_.empty());
    EnableWindow(source_edit_, !busy_);
    EnableWindow(output_edit_, !busy_);
    EnableWindow(calib_edit_, !busy_ && calibration_applicable());
    EnableWindow(source_file_, !busy_);
    EnableWindow(source_folder_, !busy_);
    EnableWindow(output_browse_, !busy_);
    EnableWindow(calib_browse_, !busy_ && calibration_applicable());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StudioApp::update_state(UiState state, const std::wstring &stage) {
    state_ = state;
    stage_ = stage;
    update_controls();
}

std::wstring StudioApp::detected_text() const {
    if (!batch_error_.empty() && view_ == View::Batch) return batch_error_;
    if (view_ == View::Batch) {
        if (input_kind_ == InputKind::None) return L"Choose a source folder";
        return std::to_wstring(batch_items_.size()) + L" recordings · " + input_kind_label(input_kind_);
    }
    if (input_kind_ == InputKind::None) return L"Choose a source to begin";
    return input_kind_label(input_kind_) + L" detected";
}

std::wstring StudioApp::start_label() const {
    if (view_ == View::Batch && input_kind_ != InputKind::None) {
        return L"Convert " + std::to_wstring(batch_items_.size()) + L" files";
    }
    return L"Convert now";
}

std::wstring StudioApp::status_text() const {
    switch (state_) {
    case UiState::Running: return L"RUNNING";
    case UiState::Success: return L"COMPLETE";
    case UiState::Error: return L"ATTENTION";
    case UiState::Cancelled: return L"CANCELLED";
    default: return L"READY";
    }
}

COLORREF StudioApp::status_color() const {
    switch (state_) {
    case UiState::Running: return kAccent;
    case UiState::Success: return kGreen;
    case UiState::Error: return kRed;
    case UiState::Cancelled: return kAmber;
    default: return kTeal;
    }
}

bool StudioApp::calibration_applicable() const {
    return (view_ == View::Batch ? batch_kind_ : input_kind_) == InputKind::OrbbecBag &&
           output_kind_ == OutputKind::K4aMkv;
}
bool StudioApp::color_applicable() const {
    return output_kind_ == OutputKind::K4aMkv &&
           (view_ == View::Batch ? batch_kind_ : input_kind_) != InputKind::None;
}
bool StudioApp::verify_applicable() const {
    return output_kind_ == OutputKind::K4aMkv &&
           (view_ == View::Batch ? batch_kind_ : input_kind_) == InputKind::OrbbecBag;
}
bool StudioApp::preserve_applicable() const {
    return output_kind_ == OutputKind::K4aMkv && !color_mjpg_ &&
           (view_ == View::Batch ? batch_kind_ : input_kind_) == InputKind::OrbbecBag;
}
bool StudioApp::no_calibration_applicable() const { return calibration_applicable(); }

std::wstring StudioApp::source_for_use() const {
    if (view_ == View::Batch) return batch_root_;
    return source_path_;
}
std::wstring StudioApp::output_for_use() const { return output_path_; }

void StudioApp::set_view(View view) {
    if (busy_) return;
    view_ = view;
    if (view == View::Single) {
        batch_items_.clear();
        batch_error_.clear();
        batch_scanned_ = false;
        batch_root_.clear();
        batch_kind_ = InputKind::None;
        if (!source_path_.empty()) set_source(source_path_);
    } else if (view == View::Batch) {
        if (!batch_root_.empty()) {
            set_text(source_edit_, batch_root_);
            refresh_batch(batch_root_);
        } else if (!source_path_.empty()) {
            set_text(source_edit_, source_path_);
            batch_root_ = source_path_;
            refresh_batch(source_path_);
        }
    }
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StudioApp::load_path(const std::wstring &path) {
    if (path.empty()) return;
    set_text(source_edit_, path);
    set_source(path);
}

void StudioApp::load_batch(const std::wstring &input_dir, const std::wstring &output_dir) {
    if (input_dir.empty()) return;
    set_view(View::Batch);
    set_text(source_edit_, input_dir);
    batch_root_ = input_dir;
    refresh_batch(input_dir);
    if (!output_dir.empty()) {
        output_path_ = output_dir;
        set_text(output_edit_, output_dir);
        update_controls();
    }
}

void StudioApp::select_target(const std::wstring &target) {
    if (target == L"mkv" || target == L"k4a") set_output_kind(OutputKind::K4aMkv);
    else if (target == L"ros1") set_output_kind(OutputKind::Ros1Bag);
    else if (target == L"ros2") set_output_kind(OutputKind::Ros2Bag);
    else if (target == L"orbbec" || target == L"bag") set_output_kind(OutputKind::OrbbecBag);
}

void StudioApp::set_output_path(const std::wstring &path) {
    if (path.empty()) return;
    output_path_ = path;
    set_text(output_edit_, path);
    update_controls();
}

void StudioApp::show_about() { set_view(View::About); }

void StudioApp::begin() { start(); }

void StudioApp::set_source(const std::wstring &path) {
    if (path.empty()) {
        source_path_.clear();
        input_kind_ = InputKind::None;
        batch_kind_ = InputKind::None;
        batch_items_.clear();
        batch_error_.clear();
        batch_scanned_ = false;
        batch_root_.clear();
        output_path_.clear();
        set_text(output_edit_, L"");
        message_ = L"Choose a source to begin.";
        update_state(UiState::Ready, L"Ready when you are");
        update_controls();
        return;
    }
    if (view_ == View::Batch) {
        batch_root_ = path;
        refresh_batch(path);
        return;
    }
    source_path_ = path;
    std::wstring error;
    input_kind_ = detect_input(fs::path(path), error);
    if (input_kind_ == InputKind::None) {
        message_ = error;
        output_path_.clear();
        set_text(output_edit_, L"");
        update_state(UiState::Error, error);
        update_controls();
        return;
    }
    if (!output_supported(input_kind_, output_kind_)) output_kind_ = default_output(input_kind_);
    output_path_ = suggested_output(fs::path(path), input_kind_, output_kind_).wstring();
    set_text(output_edit_, output_path_);
    message_ = L"Ready to convert.";
    update_state(UiState::Ready, L"Review the target, then convert");
}

void StudioApp::refresh_batch(const std::wstring &path) {
    batch_root_ = path;
    batch_items_.clear();
    batch_kind_ = InputKind::None;
    batch_error_.clear();
    batch_scanned_ = false;
    std::wstring error;
    batch_items_ = scan_batch(fs::path(path), error);
    if (!error.empty()) {
        batch_error_ = error;
        batch_kind_ = InputKind::None;
        input_kind_ = InputKind::None;
        message_ = error;
        update_state(UiState::Error, error);
        update_controls();
        return;
    }
    batch_scanned_ = true;
    batch_kind_ = batch_items_.empty() ? InputKind::None : batch_items_.front().kind;
    input_kind_ = batch_kind_;
    if (!output_supported(input_kind_, output_kind_)) output_kind_ = default_output(input_kind_);
    if (output_path_.empty()) {
        output_path_ = (fs::path(path).parent_path() / (fs::path(path).filename().wstring() + L"_converted")).wstring();
    }
    set_text(output_edit_, output_path_);
    update_state(UiState::Ready, L"Ready to convert " + std::to_wstring(batch_items_.size()) + L" files");
}

void StudioApp::set_output_kind(OutputKind kind) {
    if (!output_supported(input_kind_, kind)) return;
    output_kind_ = kind;
    if (view_ == View::Single && !source_path_.empty()) {
        output_path_ = suggested_output(fs::path(source_path_), input_kind_, output_kind_).wstring();
        set_text(output_edit_, output_path_);
    }
    update_controls();
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StudioApp::on_source_committed() {
    std::wstring path = get_text(source_edit_);
    if (path == (view_ == View::Batch ? batch_root_ : source_path_)) return;
    set_source(path);
}

void StudioApp::browse_source_file() {
    std::wstring path = pick_file(hwnd_, L"Choose a recording");
    if (path.empty()) return;
    set_text(source_edit_, path);
    source_path_ = path;
    set_source(path);
}

void StudioApp::browse_source_folder() {
    std::wstring path = pick_folder(hwnd_, view_ == View::Batch ? L"Choose a folder of recordings"
                                                                 : L"Choose a ROS 2 bag folder");
    if (path.empty()) return;
    set_text(source_edit_, path);
    source_path_ = path;
    set_source(path);
}

void StudioApp::browse_output() {
    if (view_ == View::Batch || output_kind_ == OutputKind::Ros2Bag) {
        std::wstring path = pick_folder(hwnd_, L"Choose an output folder");
        if (!path.empty()) {
            output_path_ = path;
            set_text(output_edit_, path);
            update_controls();
        }
        return;
    }
    const wchar_t *filter = L"K4A recording (*.mkv)\0*.mkv\0";
    const wchar_t *ext = L"mkv";
    if (output_kind_ == OutputKind::Ros1Bag) {
        filter = L"ROS 1 bag (*.bag)\0*.bag\0";
        ext = L"bag";
    } else if (output_kind_ == OutputKind::OrbbecBag) {
        filter = L"Orbbec bag (*.bag)\0*.bag\0";
        ext = L"bag";
    }
    std::wstring suggested = output_path_;
    if (suggested.empty() && !source_path_.empty()) {
        suggested = suggested_output(fs::path(source_path_), input_kind_, output_kind_).wstring();
    }
    std::wstring path = pick_save(hwnd_, L"Choose an output file", filter, ext);
    if (path.empty()) return;
    output_path_ = path;
    set_text(output_edit_, path);
    update_controls();
}

void StudioApp::browse_calibration() {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"K4A calibration (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose calibration.json";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    calib_path_ = path;
    set_text(calib_edit_, calib_path_);
}

void StudioApp::browse_for_output() { browse_output(); }

void StudioApp::start() {
    if (busy_) return;
    commit_source_from_edit();
    if (view_ == View::Batch && batch_root_ != get_text(source_edit_)) {
        refresh_batch(get_text(source_edit_));
    }
    if (input_kind_ == InputKind::None) {
        update_state(UiState::Error, message_.empty() ? L"Choose a valid source." : message_);
        return;
    }
    output_path_ = get_text(output_edit_);
    if (output_path_.empty()) {
        update_state(UiState::Error, L"Choose an output destination.");
        return;
    }

    std::vector<ConversionJob> jobs;
    if (view_ == View::Batch) {
        if (batch_items_.empty()) {
            update_state(UiState::Error, L"No recordings found in the source folder.");
            return;
        }
        if (is_subpath(fs::path(output_path_), fs::path(batch_root_))) {
            update_state(UiState::Error, L"Output folder must be outside the input folder.");
            return;
        }
        std::wstring error;
        jobs = build_batch_jobs(batch_items_, fs::path(batch_root_), fs::path(output_path_), output_kind_, overwrite_,
                                verify_, no_calibration_, preserve_, color_mjpg_, fs::path(calib_path_), error);
        if (!error.empty()) {
            update_state(UiState::Error, error);
            return;
        }
    } else {
        if (!output_supported(input_kind_, output_kind_)) {
            update_state(UiState::Error, L"This output format is not supported for this input.");
            return;
        }
        ConversionJob job;
        job.input = fs::path(source_path_);
        job.output = fs::path(output_path_);
        job.input_kind = input_kind_;
        job.output_kind = output_kind_;
        job.force = overwrite_;
        job.verify = verify_;
        job.no_calibration = no_calibration_;
        job.preserve_source_color = preserve_;
        job.color_mjpg = color_mjpg_;
        job.calibration_json = fs::path(calib_path_);
        jobs.push_back(std::move(job));
    }

    bool needs_overwrite = false;
    for (const auto &job : jobs) {
        std::error_code ec;
        if (fs::equivalent(job.input, job.output, ec)) {
            update_state(UiState::Error, L"Output must be different from the input.");
            return;
        }
        if (fs::exists(job.output, ec) && !overwrite_) {
            needs_overwrite = true;
            break;
        }
    }
    if (needs_overwrite) {
        int answer = MessageBoxW(hwnd_, L"One or more output destinations already exist.\n\nOverwrite them?",
                                 L"Overwrite output", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        for (auto &job : jobs) job.force = true;
        overwrite_ = true;
    }

    busy_ = true;
    cancel_requested_ = false;
    progress_ = 0;
    update_state(UiState::Running, L"Starting conversion…");
    append_log(L"◆ " + (jobs.size() == 1 ? L"Single conversion" : std::to_wstring(jobs.size()) + L" file batch"));
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, jobs = std::move(jobs)]() mutable { run_jobs(std::move(jobs)); });
}

void StudioApp::cancel() {
    if (!busy_) return;
    cancel_requested_ = true;
    stage_ = L"Cancelling…";
    update_state(UiState::Running, L"Cancelling…");
    append_log(L"… Cancel requested");
}

void StudioApp::open_output() {
    std::wstring target = output_path_;
    if (target.empty()) return;
    std::error_code ec;
    fs::path path(target);
    if (fs::is_regular_file(path, ec)) path = path.parent_path();
    if (!path.empty() && fs::exists(path, ec)) {
        ShellExecuteW(hwnd_, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void StudioApp::append_log(const std::wstring &line) {
    if (!log_edit_) return;
    std::wstring text = L"[" + now_time() + L"]  " + line + L"\r\n";
    int len = GetWindowTextLengthW(log_edit_);
    SendMessageW(log_edit_, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(log_edit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(log_edit_, EM_SCROLLCARET, 0, 0);
}

void StudioApp::clear_log() {
    if (log_edit_) SetWindowTextW(log_edit_, L"");
}

void StudioApp::run_jobs(std::vector<ConversionJob> jobs) {
    int succeeded = 0;
    int failed = 0;
    bool cancelled = false;
    const int total = static_cast<int>(jobs.size());
    for (int i = 0; i < total; ++i) {
        if (cancel_requested_) {
            cancelled = true;
            break;
        }
        const ConversionJob &job = jobs[static_cast<size_t>(i)];
        auto *header = new std::wstring(L"→ " + file_name(job.input) + L"  →  " + file_name(job.output));
        if (!PostMessageW(hwnd_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(header))) delete header;
        JobResult result = run_job(
            job,
            [this](const std::wstring &line) {
                auto *copy = new std::wstring(line);
                if (!PostMessageW(hwnd_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
            },
            [this, i, total](int percent, const std::wstring &stage) {
                if (cancel_requested_) return false;
                auto *payload = new ProgressPayload;
                payload->percent = total > 0 ? (i * 100 + percent) / total : percent;
                payload->current = i + 1;
                payload->total = total;
                payload->stage = stage;
                if (!PostMessageW(hwnd_, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
                return true;
            });
        if (result.cancelled) {
            cancelled = true;
            break;
        }
        if (result.ok) {
            ++succeeded;
            auto *copy = new std::wstring(L"✓ " + result.message);
            if (!PostMessageW(hwnd_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        } else {
            ++failed;
            auto *copy = new std::wstring(L"✕ " + result.message);
            if (!PostMessageW(hwnd_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
            if (total == 1) break;
        }
    }
    auto *done = new DonePayload;
    done->succeeded = succeeded;
    done->failed = failed;
    done->cancelled = cancelled;
    if (cancelled) done->message = L"Cancelled";
    else if (failed == 0) done->message = L"Complete";
    else done->message = L"Finished with errors";
    if (!PostMessageW(hwnd_, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(done))) delete done;
}

void StudioApp::on_progress(const ProgressPayload &progress) {
    progress_ = progress.percent;
    stage_ = progress.stage;
    if (progress.total > 1) {
        stage_ += L"  ·  " + std::to_wstring(progress.current) + L" / " + std::to_wstring(progress.total);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StudioApp::on_done(const DonePayload &done) {
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    cancel_requested_ = false;
    if (done.cancelled) {
        update_state(UiState::Cancelled, L"Cancelled");
    } else if (done.failed == 0) {
        progress_ = 100;
        update_state(UiState::Success, L"Complete  ·  " + std::to_wstring(done.succeeded) + L" succeeded");
        append_log(L"✓ " + done.message);
    } else {
        update_state(UiState::Error, L"Finished with errors  ·  " + std::to_wstring(done.failed) + L" failed");
        append_log(L"✕ " + done.message);
    }
    update_controls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StudioApp::commit_source_from_edit() {
    std::wstring path = get_text(source_edit_);
    if (view_ == View::Batch) {
        if (path != batch_root_) refresh_batch(path);
    } else if (path != source_path_) {
        set_source(path);
    }
}

void StudioApp::refresh_hover(int x, int y) {
    int target = -1;
    for (int i = 0; i < 4; ++i) {
        if (point_in(target_rects_[i], x, y)) target = i;
    }
    int color = -1;
    for (int i = 0; i < 2; ++i) {
        if (point_in(color_rects_[i], x, y)) color = i;
    }
    int toggle = -1;
    for (int i = 0; i < 4; ++i) {
        if (point_in(toggle_rects_[i], x, y)) toggle = i;
    }
    int nav = -1;
    for (int i = 0; i < 3; ++i) {
        if (point_in(nav_rects_[i], x, y)) nav = i;
    }
    const bool link = view_ == View::About && point_in(link_rect_, x, y);
    if (target != hover_target_ || color != hover_color_ || toggle != hover_toggle_ || nav != hover_nav_ ||
        link != hover_link_) {
        hover_target_ = target;
        hover_color_ = color;
        hover_toggle_ = toggle;
        hover_nav_ = nav;
        hover_link_ = link;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void StudioApp::on_click(int x, int y) {
    if (view_ == View::About) {
        if (point_in(link_rect_, x, y)) {
            ShellExecuteW(hwnd_, L"open", L"https://github.com/Erik-Lin/bag2mkv", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    for (int i = 0; i < 3; ++i) {
        if (point_in(nav_rects_[i], x, y)) {
            set_view(static_cast<View>(i));
            return;
        }
    }
    const std::array<OutputKind, 4> kinds{OutputKind::K4aMkv, OutputKind::Ros1Bag, OutputKind::Ros2Bag,
                                         OutputKind::OrbbecBag};
    for (int i = 0; i < 4; ++i) {
        if (point_in(target_rects_[i], x, y) && output_supported(input_kind_, kinds[i])) {
            set_output_kind(kinds[i]);
            return;
        }
    }
    if (point_in(color_rects_[0], x, y) && color_applicable()) {
        color_mjpg_ = false;
        update_controls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (point_in(color_rects_[1], x, y) && color_applicable()) {
        color_mjpg_ = true;
        update_controls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (point_in(toggle_rects_[0], x, y) && verify_applicable()) {
        verify_ = !verify_;
    } else if (point_in(toggle_rects_[1], x, y)) {
        overwrite_ = !overwrite_;
    } else if (point_in(toggle_rects_[2], x, y) && preserve_applicable()) {
        preserve_ = !preserve_;
    } else if (point_in(toggle_rects_[3], x, y) && no_calibration_applicable()) {
        no_calibration_ = !no_calibration_;
    } else {
        return;
    }
    update_controls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT StudioApp::handle(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_SIZE:
        layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        paint(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        SetTextColor(hdc, kText);
        SetBkColor(hdc, kField);
        return reinterpret_cast<LRESULT>(control == log_edit_ ? log_brush_ : edit_brush_);
    }
    case WM_DRAWITEM:
        paint_button(reinterpret_cast<const DRAWITEMSTRUCT *>(lparam));
        return TRUE;
    case WM_MOUSEMOVE: {
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            tracking_mouse_ = true;
        }
        refresh_hover(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        hover_target_ = hover_color_ = hover_toggle_ = hover_nav_ = -1;
        hover_link_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && (hover_link_ || hover_nav_ >= 0 || hover_target_ >= 0 ||
                                           hover_color_ >= 0 || hover_toggle_ >= 0)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        on_click(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        if (id == ID_SOURCE_EDIT && code == EN_CHANGE) {
            SetTimer(hwnd_, 1, 650, nullptr);
            return 0;
        }
        if (id == ID_SOURCE_EDIT && code == EN_KILLFOCUS) {
            KillTimer(hwnd_, 1);
            on_source_committed();
        }
        if (id == ID_SOURCE_EDIT && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) InvalidateRect(hwnd_, nullptr, FALSE);
        if (id == ID_OUTPUT_EDIT && (code == EN_SETFOCUS || code == EN_KILLFOCUS))
            InvalidateRect(hwnd_, nullptr, FALSE);
        if (id == ID_CALIB_EDIT && (code == EN_SETFOCUS || code == EN_KILLFOCUS))
            InvalidateRect(hwnd_, nullptr, FALSE);
        if (code == BN_CLICKED) {
            if (id == ID_SOURCE_FILE) browse_source_file();
            else if (id == ID_SOURCE_FOLDER) browse_source_folder();
            else if (id == ID_OUTPUT_BROWSE) browse_output();
            else if (id == ID_CALIB_BROWSE) browse_calibration();
            else if (id == ID_START) start();
            else if (id == ID_CANCEL) cancel();
            else if (id == ID_OPEN) open_output();
            else if (id == ID_CLEAR) clear_log();
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0) {
            wchar_t path[MAX_PATH] = {};
            if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
                set_text(source_edit_, path);
                if (view_ == View::Single) {
                    source_path_ = path;
                    set_source(path);
                } else {
                    set_source(path);
                }
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_TIMER:
        if (wparam == 1) {
            KillTimer(hwnd_, 1);
            on_source_committed();
        }
        return 0;
    case WM_APP_PROGRESS: {        std::unique_ptr<ProgressPayload> payload(reinterpret_cast<ProgressPayload *>(lparam));
        if (payload) on_progress(*payload);
        return 0;
    }
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring *>(lparam));
        if (line) append_log(*line);
        return 0;
    }
    case WM_APP_DONE: {
        std::unique_ptr<DonePayload> done(reinterpret_cast<DonePayload *>(lparam));
        if (done) on_done(*done);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        create_fonts();
        RECT *suggested = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
        info->ptMinTrackSize.x = S(1024);
        info->ptMinTrackSize.y = S(720);
        return 0;
    }
    case WM_CLOSE:
        if (busy_) {
            int answer = MessageBoxW(hwnd_, L"A conversion is still running.\n\nCancel it and close?",
                                     L"Conversion running", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return 0;
            cancel_requested_ = true;
            if (worker_.joinable()) worker_.join();
        }
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

StudioApp *g_app = nullptr;

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CREATE) {
        g_app = new StudioApp(hwnd);
        if (!g_app->create()) {
            delete g_app;
            g_app = nullptr;
            return -1;
        }
        return 0;
    }
    if (!g_app) return DefWindowProcW(hwnd, msg, wparam, lparam);
    LRESULT result = g_app->handle(msg, wparam, lparam);
    if (msg == WM_NCDESTROY) {
        delete g_app;
        g_app = nullptr;
    }
    return result;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"Bag2MkvStudio";
    if (!RegisterClassExW(&wc)) return 1;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(1240, static_cast<int>(work.right - work.left - 24));
    const int height = std::min(840, static_cast<int>(work.bottom - work.top - 24));
    const int x = work.left + (static_cast<int>(work.right - work.left) - width) / 2;
    const int y = work.top + (static_cast<int>(work.bottom - work.top) - height) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"bag2mkv v0.6.0 — RGB-D Conversion Studio",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    const bool compact_desktop = (work.right - work.left) < 1400 || (work.bottom - work.top) < 900;
    ShowWindow(hwnd, compact_desktop ? SW_MAXIMIZE : SW_SHOW);
    UpdateWindow(hwnd);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        bool start_now = false;
        std::wstring single_path;
        std::wstring batch_input;
        std::wstring output_dir;
        std::wstring target;
        bool show_about = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--start") {
                start_now = true;
            } else if (arg == L"--about") {
                show_about = true;
            } else if (arg == L"--batch" && i + 1 < argc) {
                batch_input = argv[++i];
            } else if (arg == L"--output" && i + 1 < argc) {
                output_dir = argv[++i];
            } else if (arg == L"--target" && i + 1 < argc) {
                target = argv[++i];
            } else if (!arg.empty() && arg[0] == L'-') {
                continue;
            } else if (single_path.empty()) {
                single_path = arg;
            }
        }
        if (g_app != nullptr) {
            if (!batch_input.empty()) g_app->load_batch(batch_input, output_dir);
            else if (!single_path.empty()) g_app->load_path(single_path);
            if (!target.empty()) g_app->select_target(target);
            if (!output_dir.empty() && batch_input.empty()) g_app->set_output_path(output_dir);
            if (show_about) g_app->show_about();
            if (start_now) g_app->begin();
        }
        LocalFree(argv);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
