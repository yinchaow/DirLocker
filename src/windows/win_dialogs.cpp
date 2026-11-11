#include "win_dialogs.hpp"

#include "../core/path_guard.hpp"
#include "../core/version.hpp"
#include "resource.h"

#include <commctrl.h>
#include <objbase.h>
#include <gdiplus.h>
#include <richedit.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace dirlocker {

namespace {

HWND g_active_modal_hwnd = nullptr;

std::wstring get_window_text_w(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len + 1), L'\0');
    int got = GetWindowTextW(h, &buf[0], len + 1);
    buf.resize(static_cast<size_t>(got > 0 ? got : 0));
    return buf;
}

constexpr COLORREF kColorMuted = RGB(95, 99, 104);
constexpr COLORREF kColorText = RGB(31, 35, 40);
constexpr COLORREF kColorAccent = RGB(13, 71, 161);
constexpr COLORREF kColorWarning = RGB(180, 83, 9);

void dlg_add_round_rect_path(Gdiplus::GraphicsPath& path, Gdiplus::REAL x, Gdiplus::REAL y,
                             Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL r) {
    Gdiplus::REAL d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

HMODULE ensure_msftedit_loaded() {
    static HMODULE mod = LoadLibraryW(L"msftedit.dll");
    return mod;
}

class ModalDialogBase {
public:
    struct LabelStyle {
        HWND hwnd = nullptr;
        COLORREF color = kColorText;
    };
    struct RoundedCard {
        RECT logical_rc{};
        COLORREF bg = RGB(246, 248, 250);
        COLORREF border = RGB(208, 215, 222);
        int radius = 6;
        bool visible = true;
        HBRUSH brush = nullptr;
    };
    struct RoundedPill {
        RECT logical_rc{};
        std::wstring text;
        COLORREF bg = RGB(235, 243, 254);
        COLORREF border = RGB(144, 202, 249);
        COLORREF fg = kColorAccent;
        int radius = 9;
    };
    struct PwEyeControl {
        HWND edit = nullptr;
        HWND eye_btn = nullptr;
        int btn_id = 0;
        wchar_t mask_char = L'\u25CF';
        bool visible = false;
    };

    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HFONT bold_font = nullptr;
    HFONT small_bold_font = nullptr;
    HFONT title_font = nullptr;
    HICON header_icon = nullptr;
    int dpi = 96;
    bool accepted = false;
    bool done = false;
    std::vector<LabelStyle> label_styles;
    std::vector<RoundedCard> cards;
    std::vector<RoundedPill> pills;
    std::vector<std::pair<HWND, size_t>> ctrl_card_map;
    std::vector<PwEyeControl> pw_eyes;
    std::vector<HWND> syslinks;
    HWND req_resize_target = nullptr;
    int req_resize_px_h = 0;
    std::function<bool()> on_validate;
    std::function<void(int id)> on_command;

    int S(int px) const { return scale_dpi(px, dpi); }

    int measure_text_logical_w(const std::wstring& text, HFONT custom_f) const {
        HDC hdc = GetDC(hwnd);
        if (!hdc) return 80;
        HFONT f = custom_f ? custom_f : font;
        HFONT old_f = f ? static_cast<HFONT>(SelectObject(hdc, f)) : nullptr;
        RECT rc{0, 0, 0, 0};
        DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rc,
                  DT_LEFT | DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        if (old_f) SelectObject(hdc, old_f);
        ReleaseDC(hwnd, hdc);
        int d = dpi > 0 ? dpi : 96;
        return ((rc.right - rc.left) * 96 + d - 1) / d;
    }

    int measure_label_logical_h(const std::wstring& text, int logical_w, bool bold) const {
        HDC hdc = GetDC(hwnd);
        if (!hdc) return 58;
        HFONT f = bold ? bold_font : font;
        HFONT old_f = f ? static_cast<HFONT>(SelectObject(hdc, f)) : nullptr;
        RECT rc{0, 0, S(logical_w), 0};
        DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rc,
                  DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        if (old_f) SelectObject(hdc, old_f);
        ReleaseDC(hwnd, hdc);
        int px_h = rc.bottom - rc.top;
        int d = dpi > 0 ? dpi : 96;
        int logical_h = (px_h * 96 + d - 1) / d;
        return (logical_h > 0 ? logical_h : 18) + 8;
    }

    size_t add_card(int x, int y, int w, int h, COLORREF bg, COLORREF border, int radius = 6, bool visible = true) {
        RoundedCard c;
        c.logical_rc = RECT{x, y, x + w, y + h};
        c.bg = bg;
        c.border = border;
        c.radius = radius;
        c.visible = visible;
        c.brush = CreateSolidBrush(bg);
        cards.push_back(c);
        return cards.size() - 1;
    }

    void set_card_visible(size_t idx, bool visible) {
        if (idx < cards.size()) {
            cards[idx].visible = visible;
        }
    }

    void set_card_height(size_t idx, int logical_h) {
        if (idx < cards.size()) {
            cards[idx].logical_rc.bottom = cards[idx].logical_rc.top + logical_h;
        }
    }

    int measure_rich_box_logical_h(HWND re) {
        if (!re) return 0;
        req_resize_target = re;
        req_resize_px_h = 0;
        LRESULT old_mask = SendMessageW(re, EM_GETEVENTMASK, 0, 0);
        SendMessageW(re, EM_SETEVENTMASK, 0, old_mask | ENM_REQUESTRESIZE);
        SendMessageW(re, EM_REQUESTRESIZE, 0, 0);
        SendMessageW(re, EM_SETEVENTMASK, 0, old_mask);
        req_resize_target = nullptr;

        int d = dpi > 0 ? dpi : 96;
        int by_req = req_resize_px_h > 0 ? (((req_resize_px_h * 96 + d - 1) / d) + 8) : 0;
        LRESULT lines = SendMessageW(re, EM_GETLINECOUNT, 0, 0);
        int by_lines = (lines > 0 ? static_cast<int>(lines) * 21 : 0) + 10;
        return by_req > by_lines ? by_req : by_lines;
    }

    void bind_ctrl_card(HWND ctrl, size_t card_idx) {
        if (ctrl && card_idx < cards.size()) {
            ctrl_card_map.push_back({ctrl, card_idx});
        }
    }

    void add_pill(int x, int y, int w, int h, const std::wstring& text,
                  COLORREF bg, COLORREF border, COLORREF fg, int radius = 9) {
        RoundedPill p;
        p.logical_rc = RECT{x, y, x + w, y + h};
        p.text = text;
        p.bg = bg;
        p.border = border;
        p.fg = fg;
        p.radius = radius;
        pills.push_back(std::move(p));
    }

    void set_edit_enabled(HWND edit, bool enabled) const {
        if (edit) EnableWindow(edit, enabled ? TRUE : FALSE);
        for (const auto& pe : pw_eyes) {
            if (pe.edit == edit && pe.eye_btn) {
                EnableWindow(pe.eye_btn, enabled ? TRUE : FALSE);
                InvalidateRect(pe.eye_btn, nullptr, FALSE);
                break;
            }
        }
    }

    void move_ctrl(HWND ctrl, int logical_x, int logical_y, int logical_w, int logical_h) const {
        if (!ctrl) return;
        MoveWindow(ctrl, S(logical_x), S(logical_y), S(logical_w), S(logical_h), TRUE);
    }

    void resize_client(int logical_w, int logical_h, HWND parent, bool recenter) const {
        if (!hwnd) return;
        DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        RECT rc{0, 0, S(logical_w), S(logical_h)};
        AdjustWindowRectEx(&rc, style, FALSE, ex_style);
        int win_w = rc.right - rc.left;
        int win_h = rc.bottom - rc.top;
        if (!recenter) {
            SetWindowPos(hwnd, nullptr, 0, 0, win_w, win_h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
        bool use_parent_rect = (parent && IsWindow(parent) && IsWindowVisible(parent) && !IsIconic(parent));
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        if (use_parent_rect) {
            RECT prc{};
            GetWindowRect(parent, &prc);
            x = prc.left + ((prc.right - prc.left) - win_w) / 2;
            y = prc.top + ((prc.bottom - prc.top) - win_h) / 2;
        } else {
            POINT pt{};
            GetCursorPos(&pt);
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
            if (mon && GetMonitorInfoW(mon, &mi)) {
                work = mi.rcWork;
            }
            x = work.left + ((work.right - work.left) - win_w) / 2;
            y = work.top + ((work.bottom - work.top) - win_h) / 2;
        }
        SetWindowPos(hwnd, nullptr, x, y, win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HWND add_styled_label(const std::wstring& text, int x, int y, int w, int h,
                          COLORREF color, bool bold = false, int id = -1, HFONT custom_font = nullptr) {
        HWND c = CreateWindowExW(0, L"STATIC", text.c_str(),
                                 WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                 S(x), S(y), S(w), S(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        HFONT f = custom_font ? custom_font : (bold ? bold_font : font);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        label_styles.push_back({c, color});
        return c;
    }

    HWND add_label(const std::wstring& text, int x, int y, int w, int h, int id = -1) {
        return add_styled_label(text, x, y, w, h, kColorText, false, id);
    }

    HWND add_syslink(const std::wstring& markup, int x, int y, int w, int h, int id) {
        HWND c = CreateWindowExW(0, WC_LINK, markup.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 S(x), S(y), S(w), S(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        if (c) {
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            syslinks.push_back(c);
        }
        return c;
    }

    void add_app_icon(int x, int y, int size_px) {
        int px = S(size_px);
        header_icon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
        if (!header_icon) {
            header_icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
        }
        HWND c = CreateWindowExW(0, L"STATIC", nullptr,
                                 WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE,
                                 S(x), S(y), px, px, hwnd, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, STM_SETICON, reinterpret_cast<WPARAM>(header_icon), 0);
    }

    HWND add_edit(int x, int y, int w, int h, int id, bool password = false, bool numeric = false) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        if (password) style |= ES_PASSWORD;
        if (numeric) style |= ES_NUMBER;

        int edit_w = (password && w > 36) ? (w - 28) : w;
        HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style,
                                 S(x), S(y), S(edit_w), S(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        if (password && w > 36) {
            int btn_id = 8100 + static_cast<int>(pw_eyes.size());
            HWND eye = CreateWindowExW(0, L"BUTTON", L"",
                                       WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                       S(x + w - 26), S(y), S(26), S(h), hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(btn_id)),
                                       GetModuleHandleW(nullptr), nullptr);
            wchar_t mask = static_cast<wchar_t>(SendMessageW(c, EM_GETPASSWORDCHAR, 0, 0));
            if (!mask) mask = L'\u25CF';
            pw_eyes.push_back({c, eye, btn_id, mask, false});
        }
        return c;
    }

    HWND add_rich_box(int x, int y, int w, int h, int id, COLORREF bg, bool border, bool vscroll) {
        HMODULE mod = ensure_msftedit_loaded();
        if (!mod) return nullptr;
        DWORD style = WS_CHILD | WS_VISIBLE | ES_MULTILINE;
        if (vscroll) style |= WS_VSCROLL | ES_AUTOVSCROLL | WS_TABSTOP;
        DWORD ex_style = border ? WS_EX_CLIENTEDGE : 0;
        HWND re = CreateWindowExW(
            ex_style, MSFTEDIT_CLASS, L"", style,
            S(x), S(y), S(w), S(h), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        if (re) {
            SendMessageW(re, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(bg));
            if (border) {
                RECT inner{};
                GetClientRect(re, &inner);
                InflateRect(&inner, -S(12), -S(10));
                SendMessageW(re, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&inner));
            }
        }
        return re;
    }

    static void append_rich_span(HWND re, const wchar_t* text, const wchar_t* face, int pt_x20,
                                 COLORREF fg, COLORREF bg, bool bold, bool has_bg,
                                 int indent_twips, int space_after_twips) {
        if (!re) return;
        SendMessageW(re, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        LONG s0 = 0, s1 = 0;
        SendMessageW(re, EM_GETSEL, reinterpret_cast<WPARAM>(&s0), reinterpret_cast<LPARAM>(&s1));
        SendMessageW(re, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));
        LONG e0 = 0, e1 = 0;
        SendMessageW(re, EM_GETSEL, reinterpret_cast<WPARAM>(&e0), reinterpret_cast<LPARAM>(&e1));
        SendMessageW(re, EM_SETSEL, static_cast<WPARAM>(s0), static_cast<LPARAM>(e1));

        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_BACKCOLOR;
        cf.yHeight = pt_x20;
        cf.crTextColor = fg;
        cf.dwEffects = bold ? CFE_BOLD : 0;
        if (has_bg) {
            cf.crBackColor = bg;
        } else {
            cf.dwEffects |= CFE_AUTOBACKCOLOR;
        }
        lstrcpynW(cf.szFaceName, face, LF_FACESIZE);
        SendMessageW(re, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));

        PARAFORMAT2 pf{};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_STARTINDENT | PFM_SPACEAFTER;
        pf.dxStartIndent = indent_twips;
        pf.dySpaceAfter = space_after_twips;
        SendMessageW(re, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&pf));
    }

    HWND add_radio(const std::wstring& text, int x, int y, int w, int h, int id, bool group = false) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
        if (group) style |= WS_GROUP;
        HWND c = CreateWindowExW(0, L"BUTTON", text.c_str(), style,
                                 S(x), S(y), S(w), S(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    }

    HWND add_checkbox(const std::wstring& text, int x, int y, int w, int h, int id, bool checked) {
        HWND c = CreateWindowExW(0, L"BUTTON", text.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE,
                                 S(x), S(y), S(w), S(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        return c;
    }

    HWND add_button(const std::wstring& text, int x, int y, int w, int h, int id, bool def = false) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
        HWND c = CreateWindowExW(0, L"BUTTON", text.c_str(), style,
                                 S(x), S(y), S(w), S(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    }

    HWND add_readonly_text(const std::wstring& text, int x, int y, int w, int h, int id) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;
        HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), style,
                                 S(x), S(y), S(w), S(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(c, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(S(8), S(8)));
        return c;
    }

    void draw_eye_button(const DRAWITEMSTRUCT* dis, const PwEyeControl& pe) const {
        if (!dis) return;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

        HBRUSH clear_br = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        FillRect(hdc, &rc, clear_br);
        DeleteObject(clear_br);

        COLORREF bg = disabled ? RGB(248, 250, 252)
                               : (pressed ? RGB(235, 243, 254) : RGB(255, 255, 255));
        COLORREF bd = disabled ? RGB(226, 232, 240)
                               : (pe.visible ? RGB(25, 118, 210) : RGB(203, 213, 225));
        COLORREF fg = disabled ? RGB(148, 163, 184)
                               : (pe.visible ? kColorAccent : kColorMuted);

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::REAL s = static_cast<Gdiplus::REAL>(dpi) / 96.0f;

        Gdiplus::GraphicsPath path;
        dlg_add_round_rect_path(path,
                                static_cast<Gdiplus::REAL>(rc.left) + 0.5f,
                                static_cast<Gdiplus::REAL>(rc.top) + 0.5f,
                                static_cast<Gdiplus::REAL>(rc.right - rc.left) - 1.5f,
                                static_cast<Gdiplus::REAL>(rc.bottom - rc.top) - 1.5f,
                                4.0f * s);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
        Gdiplus::Pen bpen(Gdiplus::Color(255, GetRValue(bd), GetGValue(bd), GetBValue(bd)), 1.0f);
        g.FillPath(&fill, &path);
        g.DrawPath(&bpen, &path);

        Gdiplus::REAL cx = (rc.left + rc.right) * 0.5f;
        Gdiplus::REAL cy = (rc.top + rc.bottom) * 0.5f;
        Gdiplus::Pen epen(Gdiplus::Color(255, GetRValue(fg), GetGValue(fg), GetBValue(fg)), 1.3f * s);
        epen.SetStartCap(Gdiplus::LineCapRound);
        epen.SetEndCap(Gdiplus::LineCapRound);
        epen.SetLineJoin(Gdiplus::LineJoinRound);

        Gdiplus::GraphicsPath eye;
        eye.AddBezier(cx - 5.8f * s, cy,
                      cx - 2.4f * s, cy - 4.4f * s,
                      cx + 2.4f * s, cy - 4.4f * s,
                      cx + 5.8f * s, cy);
        eye.AddBezier(cx + 5.8f * s, cy,
                      cx + 2.4f * s, cy + 4.4f * s,
                      cx - 2.4f * s, cy + 4.4f * s,
                      cx - 5.8f * s, cy);
        g.DrawPath(&epen, &eye);
        Gdiplus::REAL pr = 1.9f * s;
        g.DrawEllipse(&epen, cx - pr, cy - pr, pr * 2.0f, pr * 2.0f);
        if (pe.visible) {
            g.DrawLine(&epen, cx - 4.5f * s, cy + 4.5f * s, cx + 4.5f * s, cy - 4.5f * s);
        }
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        ModalDialogBase* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<ModalDialogBase*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = h;
        } else {
            self = reinterpret_cast<ModalDialogBase*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }

        if (self) {
            switch (msg) {
                case WM_PAINT: {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(h, &ps);
                    if (!self->cards.empty() || !self->pills.empty()) {
                        Gdiplus::Graphics g(hdc);
                        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                        Gdiplus::REAL s = static_cast<Gdiplus::REAL>(self->dpi) / 96.0f;

                        for (const auto& c : self->cards) {
                            if (!c.visible) continue;
                            Gdiplus::GraphicsPath path;
                            dlg_add_round_rect_path(
                                path,
                                static_cast<Gdiplus::REAL>(self->S(c.logical_rc.left)) + 0.5f,
                                static_cast<Gdiplus::REAL>(self->S(c.logical_rc.top)) + 0.5f,
                                static_cast<Gdiplus::REAL>(self->S(c.logical_rc.right - c.logical_rc.left)) - 1.5f,
                                static_cast<Gdiplus::REAL>(self->S(c.logical_rc.bottom - c.logical_rc.top)) - 1.5f,
                                static_cast<Gdiplus::REAL>(c.radius) * s);
                            Gdiplus::SolidBrush fbr(Gdiplus::Color(255, GetRValue(c.bg), GetGValue(c.bg), GetBValue(c.bg)));
                            Gdiplus::Pen bpen(Gdiplus::Color(255, GetRValue(c.border), GetGValue(c.border), GetBValue(c.border)), 1.0f);
                            g.FillPath(&fbr, &path);
                            g.DrawPath(&bpen, &path);
                        }

                        for (const auto& p : self->pills) {
                            RECT prc{self->S(p.logical_rc.left), self->S(p.logical_rc.top),
                                     self->S(p.logical_rc.right), self->S(p.logical_rc.bottom)};
                            Gdiplus::GraphicsPath path;
                            dlg_add_round_rect_path(
                                path,
                                static_cast<Gdiplus::REAL>(prc.left) + 0.5f,
                                static_cast<Gdiplus::REAL>(prc.top) + 0.5f,
                                static_cast<Gdiplus::REAL>(prc.right - prc.left) - 1.5f,
                                static_cast<Gdiplus::REAL>(prc.bottom - prc.top) - 1.5f,
                                static_cast<Gdiplus::REAL>(p.radius) * s);
                            Gdiplus::SolidBrush fbr(Gdiplus::Color(255, GetRValue(p.bg), GetGValue(p.bg), GetBValue(p.bg)));
                            Gdiplus::Pen bpen(Gdiplus::Color(255, GetRValue(p.border), GetGValue(p.border), GetBValue(p.border)), 1.0f);
                            g.FillPath(&fbr, &path);
                            g.DrawPath(&bpen, &path);

                            SetBkMode(hdc, TRANSPARENT);
                            SetTextColor(hdc, p.fg);
                            HFONT pf = self->small_bold_font ? self->small_bold_font : self->bold_font;
                            HFONT old_f = static_cast<HFONT>(SelectObject(hdc, pf));
                            DrawTextW(hdc, p.text.c_str(), static_cast<int>(p.text.size()), &prc,
                                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                            SelectObject(hdc, old_f);
                        }
                    }
                    EndPaint(h, &ps);
                    return 0;
                }
                case WM_DRAWITEM: {
                    auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                    if (dis && dis->CtlType == ODT_BUTTON) {
                        for (const auto& pe : self->pw_eyes) {
                            if (pe.eye_btn == dis->hwndItem) {
                                self->draw_eye_button(dis, pe);
                                return TRUE;
                            }
                        }
                    }
                    break;
                }
                case WM_CTLCOLORSTATIC: {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    HWND ctrl = reinterpret_cast<HWND>(lp);
                    wchar_t cls[32] = {};
                    GetClassNameW(ctrl, cls, 32);
                    if (_wcsicmp(cls, L"Edit") == 0) {
                        SetBkMode(hdc, OPAQUE);
                        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                        SetTextColor(hdc, kColorText);
                        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
                    }
                    for (const auto& m : self->ctrl_card_map) {
                        if (m.first == ctrl && m.second < self->cards.size()) {
                            const auto& c = self->cards[m.second];
                            SetTextColor(hdc, kColorText);
                            SetBkMode(hdc, TRANSPARENT);
                            if (c.brush) return reinterpret_cast<LRESULT>(c.brush);
                        }
                    }
                    COLORREF fg = kColorText;
                    for (const auto& s : self->label_styles) {
                        if (s.hwnd == ctrl) {
                            fg = s.color;
                            break;
                        }
                    }
                    SetTextColor(hdc, fg);
                    SetBkMode(hdc, TRANSPARENT);
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
                }
                case WM_NOTIFY: {
                    auto* nm = reinterpret_cast<NMHDR*>(lp);
                    if (nm && nm->hwndFrom == self->req_resize_target && nm->code == EN_REQUESTRESIZE) {
                        auto* rr = reinterpret_cast<REQRESIZE*>(lp);
                        if (rr) self->req_resize_px_h = rr->rc.bottom - rr->rc.top;
                        return 0;
                    }
                    if (nm && nm->hwndFrom && (nm->code == NM_CLICK || nm->code == NM_RETURN)) {
                        for (HWND lh : self->syslinks) {
                            if (nm->hwndFrom == lh) {
                                auto* nml = reinterpret_cast<NMLINK*>(lp);
                                const wchar_t* url = (nml && nml->item.szUrl[0])
                                    ? nml->item.szUrl
                                    : L"https://github.com/yinchaow/dirlocker";
                                ShellExecuteW(self->hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
                                return 0;
                            }
                        }
                    }
                    break;
                }
                case WM_COMMAND: {
                    int id = LOWORD(wp);
                    for (auto& pe : self->pw_eyes) {
                        if (pe.btn_id == id) {
                            pe.visible = !pe.visible;
                            SendMessageW(pe.edit, EM_SETPASSWORDCHAR,
                                         pe.visible ? 0 : static_cast<WPARAM>(pe.mask_char), 0);
                            InvalidateRect(pe.edit, nullptr, TRUE);
                            InvalidateRect(pe.eye_btn, nullptr, FALSE);
                            SetFocus(pe.edit);
                            int len = GetWindowTextLengthW(pe.edit);
                            SendMessageW(pe.edit, EM_SETSEL, len, len);
                            return 0;
                        }
                    }
                    if (id == IDOK) {
                        if (!self->on_validate || self->on_validate()) {
                            self->accepted = true;
                            self->done = true;
                        }
                        return 0;
                    }
                    if (id == IDCANCEL) {
                        self->accepted = false;
                        self->done = true;
                        return 0;
                    }
                    if (self->on_command) self->on_command(id);
                    break;
                }
                case WM_CLOSE:
                    self->accepted = false;
                    self->done = true;
                    return 0;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    bool create(HWND parent, const std::wstring& title, int logical_w, int logical_h) {
        static bool cls_registered = false;
        const wchar_t* cls_name = L"DirLockerModalDlg";
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        if (!cls_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hinst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = cls_name;
            wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON));
            RegisterClassExW(&wc);
            cls_registered = true;
        }

        if (parent && IsWindow(parent) && IsIconic(parent)) {
            ShowWindow(parent, SW_RESTORE);
        }
        bool use_parent_rect = (parent && IsWindow(parent) && IsWindowVisible(parent) && !IsIconic(parent));
        dpi = get_window_dpi(use_parent_rect ? parent : GetForegroundWindow());
        font = create_ui_font(dpi, 9, false);
        bold_font = create_ui_font(dpi, 9, true);
        small_bold_font = create_ui_font(dpi, 8, true);
        title_font = create_ui_font(dpi, 13, true);

        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        DWORD ex_style = WS_EX_DLGMODALFRAME;
        if (!parent) {
            ex_style |= WS_EX_TOPMOST | WS_EX_APPWINDOW;
        }

        int pw = S(logical_w);
        int ph = S(logical_h);
        RECT rc{0, 0, pw, ph};
        AdjustWindowRectEx(&rc, style, FALSE, ex_style);
        int win_w = rc.right - rc.left;
        int win_h = rc.bottom - rc.top;

        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        if (use_parent_rect) {
            RECT prc{};
            GetWindowRect(parent, &prc);
            x = prc.left + ((prc.right - prc.left) - win_w) / 2;
            y = prc.top + ((prc.bottom - prc.top) - win_h) / 2;
        } else {
            POINT pt{};
            GetCursorPos(&pt);
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
            if (mon && GetMonitorInfoW(mon, &mi)) {
                work = mi.rcWork;
            }
            x = work.left + ((work.right - work.left) - win_w) / 2;
            y = work.top + ((work.bottom - work.top) - win_h) / 2;
        }

        hwnd = CreateWindowExW(ex_style, cls_name, title.c_str(), style,
                               x, y, win_w, win_h, parent, nullptr, hinst, this);
        return hwnd != nullptr;
    }

    bool run_modal(HWND parent, HWND focus_ctrl = nullptr) {
        if (parent && IsWindow(parent)) EnableWindow(parent, FALSE);
        HWND prev_modal = g_active_modal_hwnd;
        g_active_modal_hwnd = hwnd;

        win_bring_window_to_front(hwnd, parent == nullptr);
        if (focus_ctrl) SetFocus(focus_ctrl);

        MSG msg{};
        while (!done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(hwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        g_active_modal_hwnd = prev_modal;
        if (parent && IsWindow(parent)) {
            EnableWindow(parent, TRUE);
            SetForegroundWindow(parent);
        }
        DestroyWindow(hwnd);
        for (auto& c : cards) {
            if (c.brush) DeleteObject(c.brush);
        }
        if (header_icon) DestroyIcon(header_icon);
        if (font) DeleteObject(font);
        if (bold_font) DeleteObject(bold_font);
        if (small_bold_font) DeleteObject(small_bold_font);
        if (title_font) DeleteObject(title_font);
        return accepted;
    }
};

}  // namespace

HWND win_get_active_modal_dialog() {
    return g_active_modal_hwnd;
}

void win_bring_window_to_front(HWND hwnd, bool topmost) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else ShowWindow(hwnd, SW_SHOW);

    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
}

int get_window_dpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d > 0) return static_cast<int>(d);
    }
    HDC hdc = GetDC(hwnd);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(hwnd, hdc);
    return dpi > 0 ? dpi : 96;
}

int scale_dpi(int px, int dpi) {
    return MulDiv(px, dpi, 96);
}

HFONT create_ui_font(int dpi, int pt, bool bold) {
    int height = -MulDiv(pt, dpi, 72);
    return CreateFontW(height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
}

std::string win_pick_folder(HWND parent) {
    std::string out;
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"选择要加锁的目录");
        if (SUCCEEDED(dlg->Show(parent))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out = wide_to_utf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

struct DisclaimerBlock {
    size_t card_idx = 0;
    HWND label = nullptr;
    HWND checkbox = nullptr;
    int label_h = 54;
    int total_h = 102;
};

DisclaimerBlock add_password_disclaimer(ModalDialogBase& dlg, int x, int y, int w, int cb_id) {
    static constexpr wchar_t kWarnPrefix[] = L"重要提醒：";
    static constexpr wchar_t kWarnBody[] =
        L"本软件不提供找回密码功能，请务必妥善牢记密码；若遗忘密码将无法通过程序直接解锁，由此造成的访问不便与相关风险由使用者自行承担。";
    static constexpr wchar_t kWarnFull[] =
        L"重要提醒：本软件不提供找回密码功能，请务必妥善牢记密码；若遗忘密码将无法通过程序直接解锁，由此造成的访问不便与相关风险由使用者自行承担。";

    const int inner_w = w - 20;
    const COLORREF kCardBg = RGB(255, 251, 235);
    const COLORREF kCardBorder = RGB(246, 196, 83);

    DisclaimerBlock blk;
    blk.card_idx = dlg.add_card(x, y, w, 120, kCardBg, kCardBorder, 6, true);

    int text_h = dlg.measure_label_logical_h(kWarnFull, inner_w, true);
    if (text_h < 62) text_h = 62;

    HWND re = dlg.add_rich_box(x + 10, y + 8, inner_w, 220, cb_id + 500, kCardBg, false, false);
    if (re) {
        ModalDialogBase::append_rich_span(re, kWarnPrefix, L"Microsoft YaHei UI", 180,
                                          kColorWarning, 0, true, false, 0, 0);
        ModalDialogBase::append_rich_span(re, kWarnBody, L"Microsoft YaHei UI", 180,
                                          RGB(120, 53, 15), 0, false, false, 0, 0);
        SendMessageW(re, EM_SETREADONLY, TRUE, 0);
        SendMessageW(re, EM_EMPTYUNDOBUFFER, 0, 0);
        SendMessageW(re, EM_SETSEL, 0, 0);

        int measured_re_h = dlg.measure_rich_box_logical_h(re);
        if (measured_re_h > text_h) text_h = measured_re_h;
        dlg.move_ctrl(re, x + 10, y + 8, inner_w, text_h);
        blk.label = re;
    } else {
        blk.label = dlg.add_styled_label(kWarnFull, x + 10, y + 8, inner_w, text_h, kColorWarning, true);
        dlg.bind_ctrl_card(blk.label, blk.card_idx);
    }

    const int total_h = 8 + text_h + 4 + 26 + 8;
    blk.label_h = text_h;
    blk.total_h = total_h;
    dlg.set_card_height(blk.card_idx, total_h);

    blk.checkbox = dlg.add_checkbox(
        L"我已牢记上述密码，并知晓忘记密码无法找回的相关风险",
        x + 10, y + 8 + text_h + 4, inner_w, 26, cb_id, false);
    dlg.bind_ctrl_card(blk.checkbox, blk.card_idx);
    return blk;
}

bool ensure_win_disclaimer_checked(HWND dlg_hwnd, HWND cb) {
    if (cb && SendMessageW(cb, BM_GETCHECK, 0, 0) == BST_CHECKED) return true;
    MessageBoxW(dlg_hwnd,
                L"请先阅读重要提醒并勾选确认“已牢记密码并知晓忘记密码的风险”。",
                L"请先确认", MB_ICONWARNING);
    if (cb) SetFocus(cb);
    return false;
}

bool win_show_master_password_dialog(HWND parent, bool require_old,
                                     std::string& old_pw, std::string& new_pw) {
    ModalDialogBase dlg;
    if (!dlg.create(parent, require_old ? L"修改主密码" : L"设置主密码", 450, 290)) {
        return false;
    }

    const std::wstring intro = require_old
        ? L"修改后，所有使用主密码的目录都将自动适用新密码。"
        : L"首次使用目录密匣，请先设置主密码（至少 4 位），作为默认解锁密码。";
    int intro_h = dlg.measure_label_logical_h(intro, 418, false);
    if (intro_h < 36) intro_h = 36;
    dlg.add_label(intro, 16, 14, 418, intro_h);

    int y = 14 + intro_h + 8;
    HWND old_edit = nullptr;
    if (require_old) {
        dlg.add_label(L"原主密码：", 16, y + 4, 86, 20);
        old_edit = dlg.add_edit(106, y, 326, 24, 1001, true);
        y += 34;
    }
    dlg.add_label(L"新主密码：", 16, y + 4, 86, 20);
    HWND new_edit = dlg.add_edit(106, y, 326, 24, 1002, true);
    y += 34;

    dlg.add_label(L"确认密码：", 16, y + 4, 86, 20);
    HWND confirm_edit = dlg.add_edit(106, y, 326, 24, 1003, true);
    y += 34;

    DisclaimerBlock disc = add_password_disclaimer(dlg, 16, y, 418, 1004);
    y += disc.total_h + 14;

    dlg.add_button(L"确定", 252, y, 86, 28, IDOK, true);
    dlg.add_button(L"取消", 346, y, 86, 28, IDCANCEL);
    dlg.resize_client(450, y + 28 + 16, parent, true);

    dlg.on_validate = [&]() {
        std::wstring o = old_edit ? get_window_text_w(old_edit) : L"";
        std::wstring n = get_window_text_w(new_edit);
        std::wstring c = get_window_text_w(confirm_edit);
        if (require_old && o.empty()) {
            MessageBoxW(dlg.hwnd, L"请输入原主密码。", L"提示", MB_ICONWARNING);
            SetFocus(old_edit);
            return false;
        }
        if (n.size() < 4) {
            MessageBoxW(dlg.hwnd, L"新密码长度至少需要 4 个字符。", L"提示", MB_ICONWARNING);
            SetFocus(new_edit);
            return false;
        }
        if (n != c) {
            MessageBoxW(dlg.hwnd, L"两次输入的新密码不一致。", L"提示", MB_ICONWARNING);
            SetFocus(confirm_edit);
            return false;
        }
        if (!ensure_win_disclaimer_checked(dlg.hwnd, disc.checkbox)) return false;
        old_pw = wide_to_utf8(o);
        new_pw = wide_to_utf8(n);
        return true;
    };

    return dlg.run_modal(parent, require_old ? old_edit : new_edit);
}

bool win_show_lock_dir_dialog(HWND parent, const std::string& target_summary,
                              bool& use_custom, std::string& custom_pw) {
    ModalDialogBase dlg;
    if (!dlg.create(parent, L"锁定目录", 470, 240)) return false;

    std::wstring wtarget = utf8_to_wide(target_summary);
    bool is_batch = target_summary.find('\n') != std::string::npos;
    int target_h = dlg.measure_label_logical_h(wtarget, 438, false);
    int min_target_h = is_batch ? 72 : 36;
    if (target_h < min_target_h) target_h = min_target_h;
    int y_off = target_h - 36;

    dlg.add_label(wtarget, 16, 14, 438, target_h);
    HWND r_global = dlg.add_radio(L"使用主密码（默认，直接锁定）", 16, 56 + y_off, 430, 22, 2001, true);
    HWND r_custom = dlg.add_radio(L"设置独立密码", 16, 82 + y_off, 430, 22, 2002);
    SendMessageW(r_global, BM_SETCHECK, BST_CHECKED, 0);

    dlg.add_label(L"独立密码：", 34, 114 + y_off, 80, 20);
    HWND pw1 = dlg.add_edit(118, 110 + y_off, 332, 24, 2003, true);
    dlg.add_label(L"确认密码：", 34, 148 + y_off, 80, 20);
    HWND pw2 = dlg.add_edit(118, 144 + y_off, 332, 24, 2004, true);

    const int disc_y = 178 + y_off;
    DisclaimerBlock disc = add_password_disclaimer(dlg, 34, disc_y, 416, 2005);

    const int btn_y_compact = 184 + y_off;
    const int btn_y_custom = disc_y + disc.total_h + 14;
    HWND btn_ok = dlg.add_button(L"锁定", 268, btn_y_compact, 86, 28, IDOK, true);
    HWND btn_cancel = dlg.add_button(L"取消", 364, btn_y_compact, 86, 28, IDCANCEL);

    bool cur_custom_state = true;  // force initial apply_mode(false)
    auto apply_mode = [&](bool custom, bool live_toggle) {
        if (live_toggle && custom == cur_custom_state) return;
        cur_custom_state = custom;
        dlg.set_edit_enabled(pw1, custom);
        dlg.set_edit_enabled(pw2, custom);
        dlg.set_card_visible(disc.card_idx, custom);
        ShowWindow(disc.label, custom ? SW_SHOW : SW_HIDE);
        ShowWindow(disc.checkbox, custom ? SW_SHOW : SW_HIDE);
        EnableWindow(disc.checkbox, custom ? TRUE : FALSE);
        int btn_y = custom ? btn_y_custom : btn_y_compact;
        dlg.move_ctrl(btn_ok, 268, btn_y, 86, 28);
        dlg.move_ctrl(btn_cancel, 364, btn_y, 86, 28);
        dlg.resize_client(470, btn_y + 28 + 16, parent, !live_toggle);
        if (!custom) {
            SetWindowTextW(pw1, L"");
            SetWindowTextW(pw2, L"");
            SendMessageW(disc.checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
        } else if (live_toggle) {
            SetFocus(pw1);
        }
        if (live_toggle) {
            RedrawWindow(dlg.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
    };
    apply_mode(false, false);

    dlg.on_command = [&](int id) {
        if (id == 2001 || id == 2002) {
            bool custom = SendMessageW(r_custom, BM_GETCHECK, 0, 0) == BST_CHECKED;
            apply_mode(custom, true);
        }
    };

    dlg.on_validate = [&]() {
        bool custom = SendMessageW(r_custom, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (custom) {
            std::wstring p1 = get_window_text_w(pw1);
            std::wstring p2 = get_window_text_w(pw2);
            if (p1.size() < 4) {
                MessageBoxW(dlg.hwnd, L"独立密码长度至少需要 4 个字符。", L"提示", MB_ICONWARNING);
                SetFocus(pw1);
                return false;
            }
            if (p1 != p2) {
                MessageBoxW(dlg.hwnd, L"两次输入的独立密码不一致。", L"提示", MB_ICONWARNING);
                SetFocus(pw2);
                return false;
            }
            if (!ensure_win_disclaimer_checked(dlg.hwnd, disc.checkbox)) return false;
            custom_pw = wide_to_utf8(p1);
        } else {
            custom_pw.clear();
        }
        use_custom = custom;
        return true;
    };

    return dlg.run_modal(parent);
}

bool win_show_unlock_dir_dialog(HWND parent, const std::string& dir_path,
                                bool use_custom, bool show_open_option,
                                const std::wstring& title_override,
                                const std::wstring& subtitle_override,
                                std::string& password, bool& open_after) {
    ModalDialogBase dlg;
    std::wstring title = title_override.empty() ? L"目录已加密码 — 请输入密码解锁" : title_override;
    if (!dlg.create(parent, title, 470, 210)) return false;

    int y = 14;
    if (!subtitle_override.empty()) {
        int sub_h = dlg.measure_label_logical_h(subtitle_override, 438, true);
        if (sub_h < 22) sub_h = 22;
        dlg.add_styled_label(subtitle_override, 16, y, 438, sub_h, kColorText, true);
        y += sub_h + 4;
    }
    std::wstring wdir = utf8_to_wide(dir_path);
    int dir_h = dlg.measure_label_logical_h(wdir, 366, false);
    if (dir_h < 34) dir_h = 34;
    dlg.add_styled_label(L"目标目录：", 16, y, 70, 20, kColorMuted, false);
    dlg.add_styled_label(wdir, 88, y, 366, dir_h, kColorText, false);
    y += dir_h + 6;
    dlg.add_styled_label(L"密码类型：", 16, y, 70, 20, kColorMuted, false);
    dlg.add_styled_label(use_custom ? L"独立密码" : L"主密码", 88, y, 200, 20, kColorAccent, true);
    y += 28;
    dlg.add_styled_label(L"密码：", 16, y + 4, 70, 20, kColorText, false);
    HWND pw = dlg.add_edit(88, y, 366, 24, 3001, true);
    y += 34;

    HWND open_cb = nullptr;
    if (show_open_option) {
        open_cb = dlg.add_checkbox(L"解锁后自动在资源管理器中打开该目录", 16, y, 360, 22, 3002, true);
        y += 34;
    } else {
        y += 10;
    }

    dlg.add_button(L"解锁", 272, y, 86, 28, IDOK, true);
    dlg.add_button(L"取消", 368, y, 86, 28, IDCANCEL);
    dlg.resize_client(470, y + 28 + 16, parent, true);

    dlg.on_validate = [&]() {
        std::wstring p = get_window_text_w(pw);
        if (p.empty()) {
            MessageBoxW(dlg.hwnd, L"请输入密码。", L"提示", MB_ICONWARNING);
            SetFocus(pw);
            return false;
        }
        password = wide_to_utf8(p);
        open_after = open_cb ? (SendMessageW(open_cb, BM_GETCHECK, 0, 0) == BST_CHECKED) : false;
        return true;
    };

    return dlg.run_modal(parent, pw);
}

bool win_show_verify_master_dialog(HWND parent, const std::wstring& title,
                                   const std::wstring& prompt, std::string& password) {
    ModalDialogBase dlg;
    if (!dlg.create(parent, title, 430, 175)) return false;

    int prompt_h = dlg.measure_label_logical_h(prompt, 398, false);
    if (prompt_h < 34) prompt_h = 34;
    dlg.add_label(prompt, 16, 14, 398, prompt_h);
    int y = 14 + prompt_h + 10;
    dlg.add_label(L"主密码：", 16, y + 4, 76, 20);
    HWND pw = dlg.add_edit(96, y, 316, 24, 3501, true);
    int btn_y = y + 46;

    dlg.add_button(L"验证", 232, btn_y, 86, 28, IDOK, true);
    dlg.add_button(L"取消", 326, btn_y, 86, 28, IDCANCEL);
    dlg.resize_client(430, btn_y + 28 + 16, parent, true);

    dlg.on_validate = [&]() {
        std::wstring p = get_window_text_w(pw);
        if (p.empty()) {
            MessageBoxW(dlg.hwnd, L"请输入主密码。", L"提示", MB_ICONWARNING);
            SetFocus(pw);
            return false;
        }
        password = wide_to_utf8(p);
        return true;
    };

    return dlg.run_modal(parent, pw);
}

bool win_show_change_dir_pw_dialog(HWND parent, const std::string& dir_path,
                                     bool currently_custom,
                                     std::string& cur_pw, std::string& new_pw) {
    ModalDialogBase dlg;
    int h = currently_custom ? 212 : 360;
    if (!dlg.create(parent, L"切换密码方式", 470, h)) return false;

    std::wstring wdir = utf8_to_wide(dir_path);
    int dir_h = dlg.measure_label_logical_h(wdir, 366, false);
    if (dir_h < 32) dir_h = 32;

    dlg.add_styled_label(L"目标目录：", 16, 14, 70, 20, kColorMuted, false);
    dlg.add_styled_label(wdir, 88, 14, 366, dir_h, kColorText, false);

    int y = 14 + dir_h + 6;
    dlg.add_styled_label(L"变更方式：", 16, y, 70, 20, kColorMuted, false);
    if (currently_custom) {
        dlg.add_styled_label(L"独立密码", 88, y, 62, 20, kColorText, true);
        dlg.add_styled_label(L"→", 152, y, 20, 20, kColorMuted, false);
        dlg.add_styled_label(L"主密码", 174, y, 70, 20, kColorAccent, true);
        y += 26;

        const std::wstring note = L"切换后原独立密码将失效，之后需使用主密码解锁。";
        int note_h = dlg.measure_label_logical_h(note, 438, false);
        if (note_h < 22) note_h = 22;
        dlg.add_styled_label(note, 16, y, 438, note_h, kColorMuted, false);
        y += note_h + 10;

        dlg.add_label(L"当前独立密码：", 16, y + 4, 98, 20);
        HWND cur_edit = dlg.add_edit(118, y, 336, 24, 4001, true);
        int btn_y = y + 46;

        dlg.add_button(L"切换为主密码", 234, btn_y, 124, 28, IDOK, true);
        dlg.add_button(L"取消", 368, btn_y, 86, 28, IDCANCEL);
        dlg.resize_client(470, btn_y + 28 + 16, parent, true);

        dlg.on_validate = [&]() {
            std::wstring cp = get_window_text_w(cur_edit);
            if (cp.empty()) {
                MessageBoxW(dlg.hwnd, L"请先输入当前独立密码进行验证。", L"提示", MB_ICONWARNING);
                SetFocus(cur_edit);
                return false;
            }
            cur_pw = wide_to_utf8(cp);
            new_pw.clear();
            return true;
        };

        return dlg.run_modal(parent, cur_edit);
    }

    dlg.add_styled_label(L"主密码", 88, y, 48, 20, kColorText, true);
    dlg.add_styled_label(L"→", 138, y, 20, 20, kColorMuted, false);
    dlg.add_styled_label(L"独立密码", 160, y, 76, 20, kColorAccent, true);
    y += 32;

    dlg.add_label(L"当前主密码：", 16, y + 4, 98, 20);
    HWND cur_edit = dlg.add_edit(118, y, 336, 24, 4001, true);
    y += 36;

    dlg.add_label(L"新独立密码：", 16, y + 4, 98, 20);
    HWND pw1 = dlg.add_edit(118, y, 336, 24, 4004, true);
    y += 36;

    dlg.add_label(L"确认新密码：", 16, y + 4, 98, 20);
    HWND pw2 = dlg.add_edit(118, y, 336, 24, 4005, true);
    y += 36;

    DisclaimerBlock disc = add_password_disclaimer(dlg, 16, y, 438, 4006);
    int chg_btn_y = y + disc.total_h + 14;

    dlg.add_button(L"切换为独立密码", 228, chg_btn_y, 130, 28, IDOK, true);
    dlg.add_button(L"取消", 368, chg_btn_y, 86, 28, IDCANCEL);
    dlg.resize_client(470, chg_btn_y + 28 + 16, parent, true);

    dlg.on_validate = [&]() {
        std::wstring cp = get_window_text_w(cur_edit);
        if (cp.empty()) {
            MessageBoxW(dlg.hwnd, L"请先输入当前主密码进行验证。", L"提示", MB_ICONWARNING);
            SetFocus(cur_edit);
            return false;
        }
        std::wstring p1 = get_window_text_w(pw1);
        std::wstring p2 = get_window_text_w(pw2);
        if (p1.size() < 4) {
            MessageBoxW(dlg.hwnd, L"新独立密码长度至少需要 4 个字符。", L"提示", MB_ICONWARNING);
            SetFocus(pw1);
            return false;
        }
        if (p1 != p2) {
            MessageBoxW(dlg.hwnd, L"两次输入的新独立密码不一致。", L"提示", MB_ICONWARNING);
            SetFocus(pw2);
            return false;
        }
        if (!ensure_win_disclaimer_checked(dlg.hwnd, disc.checkbox)) return false;
        cur_pw = wide_to_utf8(cp);
        new_pw = wide_to_utf8(p1);
        return true;
    };

    return dlg.run_modal(parent, cur_edit);
}

bool win_show_edit_custom_pw_dialog(HWND parent, const std::string& dir_path,
                                    std::string& cur_pw, std::string& new_pw) {
    ModalDialogBase dlg;
    if (!dlg.create(parent, L"修改独立密码", 470, 356)) return false;

    std::wstring wdir = utf8_to_wide(dir_path);
    int dir_h = dlg.measure_label_logical_h(wdir, 366, false);
    if (dir_h < 32) dir_h = 32;

    dlg.add_styled_label(L"目标目录：", 16, 14, 70, 20, kColorMuted, false);
    dlg.add_styled_label(wdir, 88, 14, 366, dir_h, kColorText, false);
    int y = 14 + dir_h + 4;
    dlg.add_styled_label(L"正在修改该目录的独立密码，修改后不影响主密码。", 16, y, 438, 22, kColorMuted, false);
    y += 30;

    dlg.add_label(L"当前独立密码：", 16, y + 4, 98, 20);
    HWND cur_edit = dlg.add_edit(118, y, 336, 24, 4501, true);
    y += 36;

    dlg.add_label(L"新独立密码：", 16, y + 4, 98, 20);
    HWND pw1 = dlg.add_edit(118, y, 336, 24, 4502, true);
    y += 36;

    dlg.add_label(L"确认新密码：", 16, y + 4, 98, 20);
    HWND pw2 = dlg.add_edit(118, y, 336, 24, 4503, true);
    y += 36;

    DisclaimerBlock disc = add_password_disclaimer(dlg, 16, y, 438, 4504);
    int btn_y = y + disc.total_h + 14;

    dlg.add_button(L"保存新密码", 244, btn_y, 114, 28, IDOK, true);
    dlg.add_button(L"取消", 368, btn_y, 86, 28, IDCANCEL);
    dlg.resize_client(470, btn_y + 28 + 16, parent, true);

    dlg.on_validate = [&]() {
        std::wstring cp = get_window_text_w(cur_edit);
        if (cp.empty()) {
            MessageBoxW(dlg.hwnd, L"请输入当前独立密码进行验证。", L"提示", MB_ICONWARNING);
            SetFocus(cur_edit);
            return false;
        }
        std::wstring p1 = get_window_text_w(pw1);
        std::wstring p2 = get_window_text_w(pw2);
        if (p1.size() < 4) {
            MessageBoxW(dlg.hwnd, L"新独立密码长度至少需要 4 个字符。", L"提示", MB_ICONWARNING);
            SetFocus(pw1);
            return false;
        }
        if (p1 != p2) {
            MessageBoxW(dlg.hwnd, L"两次输入的新密码不一致。", L"提示", MB_ICONWARNING);
            SetFocus(pw2);
            return false;
        }
        if (!ensure_win_disclaimer_checked(dlg.hwnd, disc.checkbox)) return false;
        cur_pw = wide_to_utf8(cp);
        new_pw = wide_to_utf8(p1);
        return true;
    };

    return dlg.run_modal(parent, cur_edit);
}

bool win_show_settings_dialog(HWND parent, int& auto_relock_minutes,
                              bool& relock_on_screen_lock, bool hotkey_ok,
                              std::string& master_pw) {
    ModalDialogBase dlg;
    if (!dlg.create(parent, L"安全与自动回锁设置", 450, 235)) return false;

    dlg.add_label(L"无操作自动回锁（分钟，0 表示关闭）：", 16, 18, 250, 20);
    HWND min_edit = dlg.add_edit(276, 14, 154, 24, 5001, false, true);
    SetWindowTextW(min_edit, std::to_wstring(auto_relock_minutes).c_str());

    HWND lock_cb = dlg.add_checkbox(L"Windows 锁屏（Win+L）或休眠时自动锁定所有目录",
                                    16, 52, 414, 22, 5002, relock_on_screen_lock);

    const std::wstring hk_txt = hotkey_ok
        ? L"一键全锁热键：Ctrl + Alt + L（已注册生效）"
        : L"一键全锁热键：Ctrl + Alt + L（热键被其他软件占用，可使用托盘锁定）";
    int hk_h = dlg.measure_label_logical_h(hk_txt, 414, false);
    if (hk_h < 22) hk_h = 22;
    dlg.add_label(hk_txt, 16, 86, 414, hk_h);

    int pw_y = 86 + hk_h + 10;
    dlg.add_label(L"验证主密码：", 16, pw_y + 4, 100, 20);
    HWND pw = dlg.add_edit(120, pw_y, 310, 24, 5003, true);

    int btn_y = pw_y + 52;
    dlg.add_button(L"保存", 252, btn_y, 86, 28, IDOK, true);
    dlg.add_button(L"取消", 346, btn_y, 86, 28, IDCANCEL);
    dlg.resize_client(450, btn_y + 28 + 16, parent, true);

    dlg.on_validate = [&]() {
        std::wstring mstr = get_window_text_w(min_edit);
        int mins = 0;
        try {
            mins = mstr.empty() ? 0 : std::stoi(mstr);
        } catch (...) {
            MessageBoxW(dlg.hwnd, L"请输入有效的分钟数 (0~1440)。", L"提示", MB_ICONWARNING);
            SetFocus(min_edit);
            return false;
        }
        if (mins < 0) mins = 0;
        if (mins > 1440) mins = 1440;

        std::wstring p = get_window_text_w(pw);
        if (p.empty()) {
            MessageBoxW(dlg.hwnd, L"请输入主密码以确认保存设置。", L"提示", MB_ICONWARNING);
            SetFocus(pw);
            return false;
        }
        auto_relock_minutes = mins;
        relock_on_screen_lock = SendMessageW(lock_cb, BM_GETCHECK, 0, 0) == BST_CHECKED;
        master_pw = wide_to_utf8(p);
        return true;
    };

    return dlg.run_modal(parent, pw);
}

void copy_text_to_clipboard(HWND owner, const std::wstring& raw) {
    std::wstring norm;
    norm.reserve(raw.size() + 32);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r' && (i + 1 >= raw.size() || raw[i + 1] != L'\n')) {
            norm += L"\r\n";
        } else {
            norm.push_back(raw[i]);
        }
    }
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (norm.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        void* dst = GlobalLock(hg);
        if (dst) {
            std::memcpy(dst, norm.c_str(), bytes);
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
        } else {
            GlobalFree(hg);
        }
    }
    CloseClipboard();
}

void win_show_recovery_dialog(HWND parent) {
    ModalDialogBase dlg;
    if (!dlg.create(parent, L"应急恢复说明", 660, 560)) return;

    const std::wstring top_tip =
        L"目录密匣不会修改或加密目录内任何文件数据，所有原始文件均完整保留。请按实际看到的现象选择对应恢复方式：";
    int top_h = dlg.measure_label_logical_h(top_tip, 628, true);
    if (top_h < 24) top_h = 24;
    dlg.add_styled_label(top_tip, 16, 14, 628, top_h, kColorText, true);

    const int box_y = 14 + top_h + 4;
    const int box_w = 628;
    const int box_h = 448;

    HWND content_ctrl = dlg.add_rich_box(16, box_y, box_w, box_h, 6001, RGB(255, 255, 255), true, true);
    if (content_ctrl) {
        const wchar_t* kUiFont = L"Microsoft YaHei UI";
        const wchar_t* kMonoFont = L"Consolas";
        const COLORREF kCodeFg = RGB(12, 74, 110);
        const COLORREF kCodeBg = RGB(241, 245, 249);
        const COLORREF kPathFg = RGB(180, 83, 9);
        const COLORREF kWarnFg = RGB(185, 28, 28);

        auto add_h = [&](const wchar_t* title) {
            ModalDialogBase::append_rich_span(content_ctrl, title, kUiFont, 195, kColorAccent, 0, true, false, 0, 50);
            ModalDialogBase::append_rich_span(content_ctrl, L"\r", kUiFont, 195, kColorAccent, 0, true, false, 0, 50);
        };
        auto add_span = [&](const wchar_t* t, int indent, int space, bool bold = false,
                            COLORREF fg = kColorText) {
            ModalDialogBase::append_rich_span(content_ctrl, t, kUiFont, 180, fg, 0, bold, false, indent, space);
        };
        auto add_path = [&](const wchar_t* p, int indent, int space) {
            ModalDialogBase::append_rich_span(content_ctrl, p, kMonoFont, 180, kPathFg, kCodeBg, false, true, indent, space);
        };
        auto add_cmd = [&](const wchar_t* cmd, int space = 35) {
            ModalDialogBase::append_rich_span(content_ctrl, cmd, kMonoFont, 180, kCodeFg, kCodeBg, false, true, 360, space);
            ModalDialogBase::append_rich_span(content_ctrl, L"\r", kUiFont, 180, kColorText, 0, false, false, 360, space);
        };

        add_h(L"1. 情形一：原目录不见了、原地只剩 .dlock 锁标入口（最常见：忘记密码 / 误删状态文件 / 卸载后）");
        add_span(L"真实目录完整存放在以下隐藏系统保护区之一：\r", 140, 30);
        add_span(L"• 系统盘（与 %LOCALAPPDATA% 同盘）：", 240, 25);
        add_path(L" %LOCALAPPDATA%\\DirLocker\\Vault ", 240, 25);
        add_span(L"\r", 240, 25);
        add_span(L"• 其他盘（以 D 盘为例）：", 240, 45);
        add_path(L" D:\\$目录密匣_重要数据保护区_切勿删除 ", 240, 45);
        add_span(L"\r", 240, 45);

        add_span(L"请打开 ", 140, 35);
        add_span(L"命令提示符（CMD，切勿用 PowerShell）", 140, 35, true, kWarnFg);
        add_span(L" 依次执行：\r", 140, 35);

        add_span(L"(0) 查看 .dlock 中的 id 与原路径（先执行 ", 180, 20, true);
        add_path(L" chcp 65001 ", 180, 20);
        add_span(L" 防中文乱码，或直接 notepad 打开）：\r", 180, 20);
        add_cmd(L" chcp 65001 ", 15);
        add_cmd(L" type \"原目录完整路径.dlock\" ", 45);

        add_span(L"(1) 解除保护区访问限制并列出其中的 <id> 子目录：\r", 180, 20, true);
        add_cmd(L" icacls \"保护区路径\" /remove:d *S-1-1-0 ", 15);
        add_cmd(L" dir /a \"保护区路径\" ", 45);

        add_span(L"(2) 解除目标 <id> 子目录限制、去掉隐藏/系统属性并移回原处恢复原名：\r", 180, 20, true);
        add_cmd(L" icacls \"保护区路径\\<id>\" /remove:d *S-1-1-0 ", 15);
        add_cmd(L" attrib -s -h \"保护区路径\\<id>\" ", 15);
        add_cmd(L" move \"保护区路径\\<id>\" \"原目录完整路径\" ", 45);

        add_span(L"(3) 删除残余 .dlock 入口（", 180, 20, true);
        add_span(L"须先去防删权限与只读属性", 180, 20, true, kWarnFg);
        add_span(L"，否则 del 会提示拒绝访问）：\r", 180, 20, true);
        add_cmd(L" icacls \"原目录完整路径.dlock\" /remove:d *S-1-1-0 ", 15);
        add_cmd(L" attrib -r \"原目录完整路径.dlock\" ", 15);
        add_cmd(L" del \"原目录完整路径.dlock\" ", 110);

        add_h(L"2. 情形二：原文件夹仍在原地（不是 .dlock 文件），仅提示无法打开/拒绝访问（少数原位锁定）");
        add_span(L"直接在 CMD 中执行（若提示无权，请以 ", 140, 25);
        add_span(L"管理员身份运行 CMD", 140, 25, true, kWarnFg);
        add_span(L" 并先执行 ", 140, 25);
        add_path(L" takeown /f \"目标目录路径\" ", 140, 25);
        add_span(L"）：\r", 140, 25);
        add_cmd(L" icacls \"目标目录路径\" /remove:d *S-1-1-0 ", 20);

        SendMessageW(content_ctrl, EM_SETREADONLY, TRUE, 0);
        SendMessageW(content_ctrl, EM_EMPTYUNDOBUFFER, 0, 0);
        SendMessageW(content_ctrl, EM_SETSEL, 0, 0);
        SendMessageW(content_ctrl, EM_SCROLLCARET, 0, 0);
    } else {
        std::wstring fallback =
            L"1. 情形一：原目录不见了、原地只剩 .dlock 锁标入口（最常见：忘记密码 / 误删状态文件 / 卸载后）\r\n"
            L"   保护区路径：%LOCALAPPDATA%\\DirLocker\\Vault 或 D:\\$目录密匣_重要数据保护区_切勿删除\r\n"
            L"   在 CMD 中依次执行：chcp 65001 -> type \"原目录.dlock\" -> icacls \"保护区\" /remove:d *S-1-1-0 -> "
            L"dir /a -> icacls \"保护区\\<id>\" /remove:d *S-1-1-0 -> attrib -s -h -> move 回原路径 -> "
            L"icacls \".dlock\" /remove:d *S-1-1-0 -> attrib -r -> del \".dlock\"\r\n\r\n"
            L"2. 情形二：原文件夹仍在原地（不是 .dlock 文件），仅提示无法打开\r\n"
            L"   icacls \"目标目录路径\" /remove:d *S-1-1-0";
        content_ctrl = dlg.add_readonly_text(fallback, 16, box_y, box_w, box_h, 6001);
    }

    const int btn_y = box_y + box_h + 14;
    HWND copy_btn = dlg.add_button(L"复制全部说明", 16, btn_y, 126, 28, 6002, false);
    HWND ok_btn = dlg.add_button(L"确定", 558, btn_y, 86, 28, IDOK, true);
    dlg.on_command = [&](int id) {
        if (id == 6002 && content_ctrl) {
            copy_text_to_clipboard(dlg.hwnd, get_window_text_w(content_ctrl));
            SetWindowTextW(copy_btn, L"✓ 已复制到剪贴板");
        }
    };
    dlg.resize_client(660, btn_y + 28 + 16, parent, true);
    dlg.run_modal(parent, ok_btn);
}

void win_show_about_dialog(HWND parent) {
    ModalDialogBase dlg;
    const int dlg_w = 556;
    if (!dlg.create(parent, L"关于 目录密匣", dlg_w, 430)) return;

    dlg.add_app_icon(20, 18, 48);
    int title_w = dlg.measure_text_logical_w(L"目录密匣", dlg.title_font);
    if (title_w < 60) title_w = 72;
    dlg.add_styled_label(L"目录密匣", 80, 17, title_w + 6, 28, kColorAccent, true, -1, dlg.title_font);

    std::wstring ver_txt = L"v" DIRLOCKER_VERSION_WSTR;
    int pill_w = dlg.measure_text_logical_w(ver_txt, dlg.small_bold_font) + 18;
    if (pill_w < 50) pill_w = 50;
    dlg.add_pill(80 + title_w + 10, 20, pill_w, 22, ver_txt,
                 RGB(235, 243, 254), RGB(144, 202, 249), kColorAccent, 9);

    dlg.add_styled_label(L"轻量级本地目录密码保护工具", 80, 46, 440, 20, kColorMuted, false);

    dlg.add_styled_label(L"适用平台：", 20, 82, 74, 20, kColorMuted, false);
    dlg.add_styled_label(L"Windows 10 / 11 (x64)", 96, 82, 420, 20, kColorText, false);

    dlg.add_styled_label(L"开发人员：", 20, 106, 74, 20, kColorMuted, false);
    dlg.add_styled_label(L"yinchaow & Gemini", 96, 106, 420, 20, kColorText, false);

    dlg.add_styled_label(L"开源协议：", 20, 130, 74, 20, kColorMuted, false);
    dlg.add_styled_label(L"MIT License", 96, 130, 420, 20, kColorText, false);

    dlg.add_styled_label(L"项目主页：", 20, 154, 74, 20, kColorMuted, false);
    HWND link = dlg.add_syslink(
        L"<a href=\"https://github.com/yinchaow/dirlocker\">https://github.com/yinchaow/dirlocker</a>",
        96, 154, 420, 20, 7001);
    if (!link) {
        dlg.add_styled_label(L"https://github.com/yinchaow/dirlocker", 96, 154, 420, 20, kColorAccent, false);
    }

    const COLORREF kCardBg = RGB(246, 248, 250);
    const COLORREF kCardBd = RGB(208, 215, 222);
    const int card_y = 186;
    const int card_w = dlg_w - 40;
    const int inner_w = card_w - 24;
    int feat_h = 148;
    size_t card_idx = dlg.add_card(20, card_y, card_w, feat_h + 20, kCardBg, kCardBd, 6, true);

    HWND feat_re = dlg.add_rich_box(32, card_y + 10, inner_w, 280, 7002, kCardBg, false, false);
    if (feat_re) {
        const wchar_t* kUi = L"Microsoft YaHei UI";
        const wchar_t* kMono = L"Consolas";
        const COLORREF kKeyBg = RGB(226, 232, 240);
        const COLORREF kKeyFg = RGB(12, 74, 110);

        ModalDialogBase::append_rich_span(feat_re, L"核心功能\r", kUi, 185, kColorAccent, 0, true, false, 0, 50);

        auto add_feat = [&](const wchar_t* title, const wchar_t* desc, bool trailing_cr = true) {
            ModalDialogBase::append_rich_span(feat_re, L"• ", kUi, 180, kColorAccent, 0, true, false, 40, 35);
            ModalDialogBase::append_rich_span(feat_re, title, kUi, 180, kColorText, 0, true, false, 40, 35);
            ModalDialogBase::append_rich_span(feat_re, desc, kUi, 180, kColorText, 0, false, false, 40, 35);
            if (trailing_cr) {
                ModalDialogBase::append_rich_span(feat_re, L"\r", kUi, 180, kColorText, 0, false, false, 40, 35);
            }
        };

        add_feat(L"双重密码体系：", L"支持统一主密码与单目录独立密码，随时自由切换与改密");
        add_feat(L"快捷验证解锁：", L"已加密目录独立标识，双击即可输入密码解锁进入");
        add_feat(L"批量拖拽加锁：", L"支持将单个或多个目录直接拖入窗口批量锁定");

        ModalDialogBase::append_rich_span(feat_re, L"• ", kUi, 180, kColorAccent, 0, true, false, 40, 35);
        ModalDialogBase::append_rich_span(feat_re, L"全锁与自动回锁：", kUi, 180, kColorText, 0, true, false, 40, 35);
        ModalDialogBase::append_rich_span(feat_re, L"支持 ", kUi, 180, kColorText, 0, false, false, 40, 35);
        ModalDialogBase::append_rich_span(feat_re, L" Ctrl+Alt+L ", kMono, 175, kKeyFg, kKeyBg, false, true, 40, 35);
        ModalDialogBase::append_rich_span(feat_re, L" 一键全锁，及闲置、锁屏自动锁定\r", kUi, 180, kColorText, 0, false, false, 40, 35);

        add_feat(L"零文件内容改动：", L"绝不加密或修改目录内原始文件，安全锁定与无损还原", false);

        SendMessageW(feat_re, EM_SETREADONLY, TRUE, 0);
        SendMessageW(feat_re, EM_EMPTYUNDOBUFFER, 0, 0);
        SendMessageW(feat_re, EM_SETSEL, 0, 0);

        int measured = dlg.measure_rich_box_logical_h(feat_re);
        if (measured > feat_h) feat_h = measured;
        dlg.move_ctrl(feat_re, 32, card_y + 10, inner_w, feat_h);
    } else {
        const std::wstring fallback_feat =
            L"核心功能：\n"
            L"• 双重密码体系：支持统一主密码与单目录独立密码，随时自由切换与改密\n"
            L"• 快捷验证解锁：已加密目录独立标识，双击直接输入密码即可进入\n"
            L"• 批量拖拽加锁：支持将单个或多个目录直接拖入窗口批量锁定\n"
            L"• 全锁与自动回锁：支持 Ctrl+Alt+L 一键全锁，及闲置、锁屏自动锁定\n"
            L"• 零文件内容改动：绝不加密或修改目录内原始文件，安全锁定与无损还原";
        int measured = dlg.measure_label_logical_h(fallback_feat, inner_w, false);
        if (measured > feat_h) feat_h = measured;
        HWND lbl = dlg.add_label(fallback_feat, 32, card_y + 10, inner_w, feat_h);
        dlg.bind_ctrl_card(lbl, card_idx);
    }

    const int card_h = feat_h + 20;
    dlg.set_card_height(card_idx, card_h);

    const int ok_y = card_y + card_h + 14;
    HWND ok_btn = dlg.add_button(L"确定", dlg_w - 20 - 86, ok_y, 86, 28, IDOK, true);
    dlg.resize_client(dlg_w, ok_y + 28 + 16, parent, true);
    dlg.run_modal(parent, ok_btn);
}

}  // namespace dirlocker
