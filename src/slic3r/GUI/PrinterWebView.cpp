#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/MultiTaskManagerPage.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceCore/DevLamp.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <wx/filename.h>
#include <wx/dcbuffer.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/dialog.h>

#include <string>
#include <wx/graphics.h>
#include <wx/dcgraph.h>
#include <wx/event.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

namespace {

std::vector<wxString> moonraker_camera_stream_urls(MachineObject *obj)
{
    if (obj == nullptr || !obj->is_online())
        return {};

    const std::string ip = obj->get_dev_ip();
    if (ip.empty())
        return {};

    // Common Mainsail/Crowsnest webcam endpoints. Mainsail often proxies the
    // stream under /webcam, while some installs expose mjpg-streamer directly.
    return {
        wxString::Format("http://%s/webcam/?action=stream", ip),
        wxString::Format("http://%s/webcam?action=stream", ip),
        wxString::Format("http://%s/webcam/stream", ip),
        wxString::Format("http://%s/webcam/video", ip),
        wxString::Format("http://%s:8080/?action=stream", ip),
        wxString::Format("http://%s:8080/webcam/?action=stream", ip)
    };
}

wxString html_escape(wxString text)
{
    text.Replace("&", "&amp;");
    text.Replace("\"", "&quot;");
    text.Replace("<", "&lt;");
    text.Replace(">", "&gt;");
    return text;
}

wxString js_escape(wxString text)
{
    text.Replace("\\", "\\\\");
    text.Replace("'", "\\'");
    text.Replace("\r", "");
    text.Replace("\n", "");
    return text;
}

wxImage image_from_thumbnail_data(const ThumbnailData &data)
{
    if (!data.is_valid())
        return wxImage();

    wxImage image(data.width, data.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < data.height; ++r) {
        const unsigned int rr = (data.height - 1 - r) * data.width;
        for (unsigned int c = 0; c < data.width; ++c) {
            const unsigned char *px = data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }
    return image;
}

wxString normalize_camera_stream_url(wxString source, MachineObject *obj)
{
    source.Trim(true);
    source.Trim(false);
    if (source.IsEmpty())
        return wxString();

    if (source.StartsWith("/")) {
        const std::string ip = obj != nullptr ? obj->get_dev_ip() : std::string();
        return ip.empty() ? wxString() : wxString::Format("http://%s%s", ip, source);
    }

    if (source.Find("://") != wxNOT_FOUND)
        return source;

    if (source.Find("/") == wxNOT_FOUND)
        return "http://" + source + "/webcam/?action=stream";

    return "http://" + source;
}

bool is_http_url(const wxString &source)
{
    return source.StartsWith("http://") || source.StartsWith("https://");
}

wxString normalize_local_file_url(wxString source)
{
    if (source.StartsWith("file://"))
        source = source.Mid(7);
#ifdef _WIN32
    if (source.StartsWith("/") && source.length() > 2 && source[2] == ':')
        source = source.Mid(1);
#endif
    source.Replace("%20", " ");
    return source;
}

wxImage scale_preview_thumbnail(wxImage image, int max_width, int max_height)
{
    if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return {};

    const double width_ratio = static_cast<double>(max_width) / static_cast<double>(image.GetWidth());
    const double height_ratio = static_cast<double>(max_height) / static_cast<double>(image.GetHeight());
    const double scale_ratio = std::min(width_ratio, height_ratio);
    const int scaled_width = std::max(1, static_cast<int>(image.GetWidth() * scale_ratio));
    const int scaled_height = std::max(1, static_cast<int>(image.GetHeight() * scale_ratio));
    return image.Scale(scaled_width, scaled_height, wxIMAGE_QUALITY_HIGH);
}

std::vector<wxString> configured_camera_stream_urls(MachineObject *obj)
{
    std::vector<wxString> urls;
    if (obj != nullptr) {
        for (const std::string &stream_url : obj->camera_stream_urls) {
            const wxString url = normalize_camera_stream_url(from_u8(stream_url), obj);
            if (!url.IsEmpty() && std::find(urls.begin(), urls.end(), url) == urls.end())
                urls.push_back(url);
        }
    }

    if (wxGetApp().app_config != nullptr && wxGetApp().app_config->get("camera", "enable_custom_source") == "true") {
        const wxString custom_source = from_u8(wxGetApp().app_config->get("camera", "custom_source"));
        const wxString custom_url = normalize_camera_stream_url(custom_source, obj);
        if (!custom_url.IsEmpty() && std::find(urls.begin(), urls.end(), custom_url) == urls.end())
            urls.push_back(custom_url);
    }

    if (urls.empty()) {
        for (const wxString &url : moonraker_camera_stream_urls(obj)) {
            if (std::find(urls.begin(), urls.end(), url) == urls.end())
                urls.push_back(url);
        }
    }
    return urls;
}

wxString camera_stream_page(const std::vector<wxString> &stream_urls)
{
    wxString urls_js;
    for (const wxString &url : stream_urls) {
        if (url.IsEmpty())
            continue;
        if (!urls_js.IsEmpty())
            urls_js += ",";
        urls_js += "'" + js_escape(url) + "'";
    }

    return "<!doctype html><html><head><meta charset='utf-8'>"
           "<style>"
           "html,body{margin:0;width:100%;height:100%;background:#000;overflow:hidden;}"
           "body{display:flex;align-items:center;justify-content:center;color:#b8bec8;font-family:Arial,sans-serif;}"
           "#camera-stream{width:100%;height:100%;object-fit:contain;background:#000;}"
           ".message{display:none;position:absolute;inset:0;align-items:center;justify-content:center;background:#000;}"
           "body.loading .message,body.failed .message{display:flex;}"
           "</style></head><body class='loading'>"
           "<img id='camera-stream' alt='Camera stream'>"
           "<div class='message'>Camera stream unavailable</div>"
           "<script>"
           "const urls=[" + urls_js + "];"
           "let index=0;"
           "const img=document.getElementById('camera-stream');"
           "const msg=document.querySelector('.message');"
           "function withCacheBuster(url){return url+(url.indexOf('?')>=0?'&':'?')+'_='+(Date.now());}"
           "function fail(){document.body.className='failed';msg.textContent='Camera stream unavailable';}"
           "function next(){"
           "if(index>=urls.length){fail();return;}"
           "document.body.className='loading';"
           "msg.textContent='Trying camera stream...';"
           "img.src=withCacheBuster(urls[index++]);"
           "}"
           "img.onload=function(){document.body.className='';};"
           "img.onerror=function(){setTimeout(next,250);};"
           "if(urls.length){next();}else{fail();}"
           "</script></body></html>";
}

enum class AxisControlAction {
    None,
    XMinus,
    XPlus,
    YMinus,
    YPlus
};

// Printer status mini-cards: dark header — rounded top corners only; bottom edge straight (separator).
class PsCardHeaderPanel : public wxPanel
{
public:
    PsCardHeaderPanel(wxWindow *parent, const wxColour &fill, double corner_radius)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_fill(fill)
        , m_corner_radius(corner_radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(fill);
        SetDoubleBuffered(true);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
        Bind(wxEVT_PAINT, &PsCardHeaderPanel::on_paint, this);
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        const wxRect rect = GetClientRect();
        if (rect.width <= 0 || rect.height <= 0)
            return;

        const double x = rect.x;
        const double y = rect.y;
        const double w = rect.width;
        const double h = rect.height;
        const double r = std::min(m_corner_radius, std::min(w * 0.5, h * 0.5));

        dc.SetBackground(wxBrush(m_fill));
        dc.Clear();

#if defined(__WXMSW__)
        // wxGCDC / Direct2D path has been associated with rare startup paint crashes; use plain wxDC on Windows.
        (void)0;
#else
        wxGCDC gdc(dc);
        wxGraphicsContext *gctx = gdc.GetGraphicsContext();
        if (gctx) {
            gctx->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gctx->SetBrush(wxBrush(m_fill));
            gctx->SetPen(wxPen(m_fill, 1));
            const double cap_h = std::min(2.0 * r, h);
            gctx->DrawRoundedRectangle(x, y, w, cap_h, r);
            if (h > cap_h)
                gctx->DrawRectangle(x, y + cap_h, w, h - cap_h);
            return;
        }
#endif

        const int ri = std::max(1, static_cast<int>(r + 0.5));
        const int cap_hi = std::min(ri * 2, rect.height);
        dc.SetBrush(wxBrush(m_fill));
        dc.SetPen(wxPen(m_fill, 1));
        dc.DrawRoundedRectangle(rect.x, rect.y, rect.width, cap_hi, ri);
        if (rect.height > cap_hi)
            dc.DrawRectangle(rect.x, rect.y + cap_hi, rect.width, rect.height - cap_hi);
    }

    wxColour m_fill;
    double   m_corner_radius;
};

// Single-panel D-pad: draws all 4 directional arrows in one wxPanel to avoid
// the panel-overlap artifact that occurs when 4 separate child panels are used.
class AxisJoystickPanel : public wxPanel
{
public:
    AxisJoystickPanel(wxWindow *parent, int square, int center_sz, int center_gap, int button_gap,
                      int, int, int, int)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(square, square))
        , m_square(square), m_center(center_sz)
        , m_cp((square - center_sz) / 2), m_center_gap(center_gap), m_button_gap(button_gap)
    {
        SetMinSize(wxSize(square, square));
        SetMaxSize(wxSize(square, square));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(28, 30, 34));
        Bind(wxEVT_PAINT, &AxisJoystickPanel::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &AxisJoystickPanel::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &AxisJoystickPanel::on_left_up, this);
        Bind(wxEVT_LEAVE_WINDOW, &AxisJoystickPanel::on_mouse_leave, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &AxisJoystickPanel::on_mouse_capture_lost, this);
    }

    void set_action_handler(std::function<void(AxisControlAction)> handler) { m_action_handler = std::move(handler); }

private:
    struct AxisPiece {
        std::vector<wxPoint2DDouble> points;
        wxString label;
        wxPoint2DDouble label_center;
        AxisControlAction action;
    };

    wxPoint2DDouble p(double x, double y) const
    {
        const double scale = (double)m_square / 300.0;
        return {x * scale, y * scale};
    }

    wxGraphicsPath rounded_path(wxGraphicsContext *gc, const std::vector<wxPoint2DDouble> &points, double radius) const
    {
        wxGraphicsPath path = gc->CreatePath();
        const int n = (int)points.size();
        if (n == 0)
            return path;

        auto len = [](const wxPoint2DDouble &a, const wxPoint2DDouble &b) {
            const double dx = b.m_x - a.m_x;
            const double dy = b.m_y - a.m_y;
            return std::sqrt(dx * dx + dy * dy);
        };

        std::vector<wxPoint2DDouble> before(n), after(n);
        for (int i = 0; i < n; i++) {
            const auto &prev = points[(i - 1 + n) % n];
            const auto &curr = points[i];
            const auto &next = points[(i + 1) % n];
            const double lin = len(prev, curr);
            const double lout = len(curr, next);
            const double ri = std::min(radius, lin * 0.45);
            const double ro = std::min(radius, lout * 0.45);

            before[i] = {curr.m_x - (curr.m_x - prev.m_x) / lin * ri, curr.m_y - (curr.m_y - prev.m_y) / lin * ri};
            after[i]  = {curr.m_x + (next.m_x - curr.m_x) / lout * ro, curr.m_y + (next.m_y - curr.m_y) / lout * ro};
        }

        path.MoveToPoint(before[0]);
        for (int i = 0; i < n; i++) {
            path.AddQuadCurveToPoint(points[i].m_x, points[i].m_y, after[i].m_x, after[i].m_y);
            path.AddLineToPoint(before[(i + 1) % n]);
        }
        path.CloseSubpath();
        return path;
    }

    std::vector<AxisPiece> pieces() const
    {
        const double center_left   = (double)m_cp;
        const double center_top    = (double)m_cp;
        const double center_right  = (double)(m_cp + m_center);
        const double center_bottom = (double)(m_cp + m_center);
        const double cgap = (double)m_center_gap;
        const double diagonal_gap = (double)m_button_gap / std::sqrt(2.0);
        const double left_inner = center_left - cgap;
        const double top_inner = center_top - cgap;
        const double right_inner = center_right + cgap;
        const double bottom_inner = center_bottom + cgap;

        return {
            {{{p(54, 22), p(246, 22), p(260, 36), {right_inner - diagonal_gap, top_inner}, {left_inner + diagonal_gap, top_inner}, p(40, 36)}},
             "Y+", p(150, 74), AxisControlAction::YPlus},
            {{{p(22, 54), p(36, 40), {left_inner, top_inner + diagonal_gap}, {left_inner, bottom_inner - diagonal_gap}, p(36, 260), p(22, 246)}},
             "X-", p(68, 150), AxisControlAction::XMinus},
            {{{p(278, 54), p(264, 40), {right_inner, top_inner + diagonal_gap}, {right_inner, bottom_inner - diagonal_gap}, p(264, 260), p(278, 246)}},
             "X+", p(232, 150), AxisControlAction::XPlus},
            {{{p(54, 278), p(246, 278), p(260, 264), {right_inner - diagonal_gap, bottom_inner}, {left_inner + diagonal_gap, bottom_inner}, p(40, 264)}},
             "Y-", p(150, 226), AxisControlAction::YMinus},
        };
    }

    static bool contains_point(const std::vector<wxPoint2DDouble> &poly, const wxPoint &point)
    {
        bool inside = false;
        const double x = point.x;
        const double y = point.y;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const double xi = poly[i].m_x;
            const double yi = poly[i].m_y;
            const double xj = poly[j].m_x;
            const double yj = poly[j].m_y;
            const bool intersect = ((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi);
            if (intersect)
                inside = !inside;
        }
        return inside;
    }

    AxisControlAction hit_test(const wxPoint &point) const
    {
        for (const auto &piece : pieces())
            if (contains_point(piece.points, point))
                return piece.action;
        return AxisControlAction::None;
    }

    void on_left_down(wxMouseEvent &event)
    {
        m_pressed_action = hit_test(event.GetPosition());
        if (m_pressed_action != AxisControlAction::None) {
            if (!HasCapture())
                CaptureMouse();
            Refresh();
            return;
        }
        event.Skip();
    }

    void on_left_up(wxMouseEvent &event)
    {
        if (HasCapture())
            ReleaseMouse();

        const AxisControlAction pressed_action = m_pressed_action;
        const AxisControlAction released_action = hit_test(event.GetPosition());
        m_pressed_action = AxisControlAction::None;
        Refresh();

        if (m_action_handler && pressed_action != AxisControlAction::None && pressed_action == released_action)
            m_action_handler(pressed_action);
        else
            event.Skip();
    }

    void on_mouse_leave(wxMouseEvent &event)
    {
        if (!HasCapture() && m_pressed_action != AxisControlAction::None) {
            m_pressed_action = AxisControlAction::None;
            Refresh();
        }
        event.Skip();
    }

    void on_mouse_capture_lost(wxMouseCaptureLostEvent &)
    {
        m_pressed_action = AxisControlAction::None;
        Refresh();
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->SetPen(*wxTRANSPARENT_PEN);

        const auto axis_pieces = pieces();
        for (const auto &piece : axis_pieces) {
            const bool pressed = piece.action == m_pressed_action;
            gc->SetBrush(wxBrush(pressed ? wxColour(185, 185, 185) : wxColour(217, 217, 217)));
            gc->FillPath(rounded_path(gc.get(), piece.points, FromDIP(18)));
        }

        gc->SetFont(wxFont(FromDIP(18), wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD), wxColour(45, 48, 55));
        for (const auto &piece : axis_pieces) {
            wxDouble tw, th;
            gc->GetTextExtent(piece.label, &tw, &th);
            gc->SetFont(wxFont(FromDIP(18), wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD),
                piece.action == m_pressed_action ? wxColour(25, 27, 31) : wxColour(45, 48, 55));
            gc->DrawText(piece.label, piece.label_center.m_x - tw / 2.0, piece.label_center.m_y - th / 2.0);
        }
    }

    int m_square, m_center, m_cp, m_center_gap, m_button_gap;
    AxisControlAction m_pressed_action{ AxisControlAction::None };
    std::function<void(AxisControlAction)> m_action_handler;
};

wxString layer_value_text(int layer)
{
    return layer < 0 ? wxString("N/A") : wxString::Format("%d", layer);
}

wxString active_file_name_text(const MachineObject *obj)
{
    if (obj == nullptr)
        return "N/A";

    if (!obj->subtask_name.empty())
        return from_u8(obj->subtask_name);

    if (!obj->m_gcode_file.empty())
        return from_u8(wxFileName(obj->m_gcode_file).GetFullName().utf8_string());

    return "N/A";
}

wxString remaining_minutes_text(int remaining_seconds)
{
    if (remaining_seconds < 0)
        return "N/A";
    const int minutes = (remaining_seconds + 59) / 60;
    return wxString::Format("%dm", minutes);
}
} // namespace

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {
    SetBackgroundColour(wxColour(28, 30, 34));

    auto *main_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *preview_menu_panel = new wxPanel(this, wxID_ANY);
    preview_menu_panel->SetBackgroundColour(wxColour(28, 30, 34));
    preview_menu_panel->SetMinSize(wxSize(FromDIP(259), FromDIP(813)));
    preview_menu_panel->SetMaxSize(wxSize(FromDIP(259), -1));
    auto *preview_menu_sizer = new wxBoxSizer(wxVERTICAL);

    auto add_preview_menu_item = [this, preview_menu_panel, preview_menu_sizer](const wxString &text, int height, PrinterWebViewTab tab, bool clickable = false) {
        auto *item_panel = new wxPanel(preview_menu_panel, wxID_ANY);
        item_panel->SetBackgroundColour(wxColour(28, 30, 34));
        item_panel->SetMinSize(wxSize(-1, FromDIP(height)));
        item_panel->SetMaxSize(wxSize(-1, FromDIP(height)));
        item_panel->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *item_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *active_strip = new wxPanel(item_panel, wxID_ANY);
        active_strip->SetMinSize(wxSize(FromDIP(3), -1));
        active_strip->SetMaxSize(wxSize(FromDIP(3), -1));
        active_strip->SetBackgroundColour(wxColour(28, 30, 34));
        item_sizer->Add(active_strip, 0, wxEXPAND);
        item_sizer->AddSpacer(FromDIP(16));

        auto *label = new wxStaticText(item_panel, wxID_ANY, text);
        label->SetForegroundColour(wxColour(235, 235, 235));
        label->SetCursor(wxCursor(wxCURSOR_HAND));
        item_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        item_sizer->AddStretchSpacer(1);

        auto *chevron = new wxStaticText(item_panel, wxID_ANY, ">");
        chevron->SetForegroundColour(wxColour(130, 130, 130));
        chevron->SetCursor(wxCursor(wxCURSOR_HAND));
        item_sizer->Add(chevron, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));

        item_panel->SetSizer(item_sizer);
        if (clickable) {
            m_preview_printers_button = item_panel;
            item_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { toggle_printers_popup(); });
            label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { toggle_printers_popup(); });
            chevron->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { toggle_printers_popup(); });
        } else {
            m_sidebar_items.push_back({ tab, item_panel, active_strip, label, chevron });
            item_panel->Bind(wxEVT_LEFT_DOWN, [this, tab](wxMouseEvent &) { select_tab(tab); });
            label->Bind(wxEVT_LEFT_DOWN, [this, tab](wxMouseEvent &) { select_tab(tab); });
            chevron->Bind(wxEVT_LEFT_DOWN, [this, tab](wxMouseEvent &) { select_tab(tab); });
        }
        preview_menu_sizer->Add(item_panel, 0, wxEXPAND);
    };

    preview_menu_sizer->AddSpacer(FromDIP(18));

    m_connected_printer_panel = new wxPanel(preview_menu_panel, wxID_ANY);
    m_connected_printer_panel->SetBackgroundColour(wxColour(28, 30, 34));
    auto *connected_printer_sizer = new wxBoxSizer(wxVERTICAL);
    m_connected_printer_status_label = new wxStaticText(m_connected_printer_panel, wxID_ANY, wxString::FromUTF8("Ba\xC4\x9Fl\xC4\xB1 yaz\xC4\xB1""c\xC4\xB1 yok"));
    m_connected_printer_status_label->SetForegroundColour(wxColour(150, 156, 166));
    m_connected_printer_name_label = new wxStaticText(m_connected_printer_panel, wxID_ANY, "N/A", wxDefaultPosition, wxSize(FromDIP(185), -1), wxST_ELLIPSIZE_END);
    m_connected_printer_name_label->SetForegroundColour(wxColour(235, 235, 235));
    wxFont connected_name_font = m_connected_printer_name_label->GetFont();
    connected_name_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_connected_printer_name_label->SetFont(connected_name_font);
    m_connected_printer_logout_label = new wxStaticText(m_connected_printer_panel, wxID_ANY, wxString::FromUTF8("\xC3\x87\xC4\xB1k\xC4\xB1\xC5\x9F yap"));
    m_connected_printer_logout_label->SetForegroundColour(wxColour(150, 156, 166));
    m_connected_printer_logout_label->SetCursor(wxCursor(wxCURSOR_HAND));
    m_connected_printer_logout_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
        dismiss_printers_popup();
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj != nullptr) {
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: disconnect selected printer dev_id =" << obj->get_dev_id();
            obj->disconnect();
            obj->set_online_state(false);
            obj->reset();
        }
        m_has_active_printer_connection = false;
        if (dev_manager != nullptr)
            dev_manager->set_selected_machine("");
        rebuild_printers_popup();
        refresh_layer_info_from_selected_machine();
    });
    auto *connected_name_row = new wxBoxSizer(wxHORIZONTAL);
    connected_name_row->Add(m_connected_printer_name_label, 1, wxALIGN_CENTER_VERTICAL);
    connected_name_row->AddSpacer(FromDIP(8));
    connected_name_row->Add(m_connected_printer_logout_label, 0, wxALIGN_CENTER_VERTICAL);
    connected_printer_sizer->Add(m_connected_printer_status_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(19));
    connected_printer_sizer->Add(connected_name_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(19));
    connected_printer_sizer->AddSpacer(FromDIP(10));
    m_connected_printer_panel->SetSizer(connected_printer_sizer);
    m_connected_printer_name_label->Hide();
    m_connected_printer_logout_label->Hide();
    preview_menu_sizer->Add(m_connected_printer_panel, 0, wxEXPAND);

    add_preview_menu_item("Printers", 50, PrinterWebViewTab::Status, true);
    add_preview_menu_item("Durum", 40, PrinterWebViewTab::Status);
    add_preview_menu_item("Depolama", 40, PrinterWebViewTab::Storage);
    add_preview_menu_item("Guncelle", 40, PrinterWebViewTab::Update);
    add_preview_menu_item("Asistan", 40, PrinterWebViewTab::Assistant);
    preview_menu_sizer->AddStretchSpacer(1);
    preview_menu_panel->SetSizer(preview_menu_sizer);

    m_printers_popup = new wxPopupTransientWindow(this, wxBORDER_NONE);
    m_printers_popup_panel = new wxPanel(m_printers_popup, wxID_ANY);
    m_printers_popup_panel->SetBackgroundColour(wxColour(245, 245, 245));
    rebuild_printers_popup();

    m_extruder_popup = new wxPopupTransientWindow(this, wxBORDER_NONE);
    m_extruder_popup_panel = new wxPanel(m_extruder_popup, wxID_ANY);
    m_extruder_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));
    rebuild_extruder_popup();

    m_fan_popup = new wxPopupTransientWindow(this, wxBORDER_NONE);
    m_fan_popup_panel = new wxPanel(m_fan_popup, wxID_ANY);
    m_fan_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));
    rebuild_fan_popup();

    m_speed_popup = new wxPopupTransientWindow(this, wxBORDER_NONE);
    m_speed_popup_panel = new wxPanel(m_speed_popup, wxID_ANY);
    m_speed_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));
    rebuild_speed_popup();

    auto *content_host = new wxPanel(this, wxID_ANY);
    content_host->SetBackgroundColour(wxColour(28, 30, 34));
    auto *content_host_sizer = new wxBoxSizer(wxVERTICAL);

    m_status_page = new wxPanel(content_host, wxID_ANY);
    m_status_page->SetBackgroundColour(wxColour(28, 30, 34));
    auto *status_page_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto *left_container = new wxPanel(m_status_page, wxID_ANY);
    left_container->SetBackgroundColour(wxColour(28, 30, 34));
    auto *left_sizer = new wxBoxSizer(wxVERTICAL);
    auto *top_row = new wxBoxSizer(wxHORIZONTAL);
    auto *preview_box = new StaticBox(left_container, wxID_ANY);
    preview_box->SetCornerRadius(FromDIP(12));
    preview_box->SetBorderWidth(1);
    preview_box->SetBorderColorNormal(wxColour(55, 58, 64));
    preview_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    preview_box->SetBackgroundColour(wxColour(28, 30, 34));
    preview_box->SetMinSize(wxSize(FromDIP(640), -1));
    auto *preview_box_sizer = new wxBoxSizer(wxVERTICAL);
    preview_box_sizer->AddSpacer(FromDIP(12));
    auto *camera_title_row = new wxBoxSizer(wxHORIZONTAL);
    auto *camera_icon = new wxStaticBitmap(preview_box, wxID_ANY, create_scaled_bitmap("monitor_camera_white", this, 16));
    auto *camera_label = new wxStaticText(preview_box, wxID_ANY, "Camera");
    camera_label->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = camera_label->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        camera_label->SetFont(f);
    }
    camera_title_row->Add(camera_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    camera_title_row->AddSpacer(FromDIP(6));
    camera_title_row->Add(camera_label, 0, wxALIGN_CENTER_VERTICAL);
    camera_title_row->AddStretchSpacer(1);
    auto *camera_refresh_btn = new wxStaticBitmap(preview_box, wxID_ANY, create_scaled_bitmap("camera_refresh_white", this, 18));
    camera_refresh_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    camera_refresh_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_camera_webview == nullptr)
            return;
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        auto urls = configured_camera_stream_urls(obj);
        m_camera_webview->SetPage(camera_stream_page(urls), m_camera_stream_url.BeforeLast('/'));
    });
    camera_title_row->Add(camera_refresh_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(15));
    preview_box_sizer->Add(camera_title_row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    // Host only until the panel is actually shown. Creating WebView2 while MainFrame / tabs are still
    // constructing has been observed to crash (ACCESS_VIOLATION in ntdll); defer to wxEVT_SHOW.
    m_camera_webview_host = new wxPanel(preview_box, wxID_ANY);
    m_camera_webview_host->SetMinSize(wxSize(FromDIP(580), FromDIP(454)));
    m_camera_webview_host->SetBackgroundColour(wxColour(0, 0, 0));
    preview_box_sizer->Add(m_camera_webview_host, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));
    preview_box->SetSizer(preview_box_sizer);
    top_row->Add(preview_box, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));

    auto *progress_box = new StaticBox(left_container, wxID_ANY);
    progress_box->SetCornerRadius(FromDIP(10));
    progress_box->SetBorderWidth(1);
    progress_box->SetBorderColorNormal(wxColour(55, 58, 64));
    progress_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    progress_box->SetBackgroundColour(wxColour(28, 30, 34));
    progress_box->SetMinSize(wxSize(FromDIP(640), FromDIP(325)));
    auto *progress_box_sizer = new wxBoxSizer(wxVERTICAL);
    auto *progress_title_row = new wxBoxSizer(wxHORIZONTAL);
    auto *progress_title_icon = new wxStaticBitmap(progress_box, wxID_ANY, create_scaled_bitmap("monitor_tasklist_print_white", this, 16));
    auto *progress_title = new wxStaticText(progress_box, wxID_ANY, "Print Status");
    progress_title->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = progress_title->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        progress_title->SetFont(f);
    }
    progress_title_row->Add(progress_title_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    progress_title_row->AddSpacer(FromDIP(6));
    progress_title_row->Add(progress_title, 0, wxALIGN_CENTER_VERTICAL);
    progress_box_sizer->Add(progress_title_row, 0, wxEXPAND | wxTOP, FromDIP(12));
    progress_box_sizer->AddSpacer(FromDIP(10));
    auto *progress_top_line = new wxPanel(progress_box, wxID_ANY);
    progress_top_line->SetMinSize(wxSize(-1, FromDIP(1)));
    progress_top_line->SetMaxSize(wxSize(-1, FromDIP(1)));
    progress_top_line->SetBackgroundColour(wxColour(96, 100, 108));
    progress_box_sizer->Add(progress_top_line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    progress_box_sizer->AddSpacer(FromDIP(15));
    auto *progress_content_row = new wxBoxSizer(wxHORIZONTAL);
    auto *progress_thumb_box = new StaticBox(progress_box, wxID_ANY);
    progress_thumb_box->SetMinSize(wxSize(FromDIP(240), FromDIP(200)));
    progress_thumb_box->SetMaxSize(wxSize(FromDIP(240), FromDIP(200)));
    progress_thumb_box->SetCornerRadius(FromDIP(12));
    progress_thumb_box->SetBorderWidth(0);
    progress_thumb_box->SetBackgroundColorNormal(wxColour(0, 0, 0));
    progress_thumb_box->SetBackgroundColour(wxColour(0, 0, 0));
    auto *progress_thumb_sizer = new wxBoxSizer(wxVERTICAL);
    m_preview_thumbnail = new wxStaticBitmap(progress_thumb_box, wxID_ANY, wxNullBitmap);
    m_preview_thumbnail->SetBackgroundColour(wxColour(0, 0, 0));
    m_preview_thumbnail->SetMinSize(wxSize(FromDIP(220), FromDIP(180)));
    m_preview_thumbnail->SetMaxSize(wxSize(FromDIP(220), FromDIP(180)));
    set_fallback_preview_thumbnail();
    progress_thumb_sizer->AddStretchSpacer(1);
    progress_thumb_sizer->Add(m_preview_thumbnail, 0, wxALIGN_CENTER_HORIZONTAL);
    progress_thumb_sizer->AddStretchSpacer(1);
    progress_thumb_box->SetSizer(progress_thumb_sizer);
    progress_content_row->Add(progress_thumb_box, 0, wxLEFT, FromDIP(15));
    progress_content_row->AddSpacer(FromDIP(15));

    auto *controls_col = new wxBoxSizer(wxVERTICAL);
    controls_col->AddSpacer(FromDIP(15));
    auto *printing_file_label = new wxStaticText(progress_box, wxID_ANY, _L("Printing File:"));
    printing_file_label->SetForegroundColour(wxColour(97, 211, 124));
    {
        wxFont f = printing_file_label->GetFont();
        if (f.GetPointSize() > 1) f.SetPointSize(f.GetPointSize() - 1);
        printing_file_label->SetFont(f);
    }
    controls_col->Add(printing_file_label, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
    m_active_file_name_value = new wxStaticText(progress_box, wxID_ANY, "N/A", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_active_file_name_value->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = m_active_file_name_value->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        m_active_file_name_value->SetFont(f);
    }
    controls_col->Add(m_active_file_name_value, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    auto *total_time_row = new wxBoxSizer(wxHORIZONTAL);
    auto *total_time_icon = new wxStaticText(progress_box, wxID_ANY, wxString::FromUTF8("\xE2\x8F\xB1"));
    total_time_icon->SetForegroundColour(wxColour(150, 156, 166));
    auto *total_time_label = new wxStaticText(progress_box, wxID_ANY, " Total:");
    total_time_label->SetForegroundColour(wxColour(150, 156, 166));
    m_total_time_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_total_time_value->SetForegroundColour(wxColour(220, 220, 220));
    total_time_row->Add(total_time_icon, 0, wxALIGN_CENTER_VERTICAL);
    total_time_row->Add(total_time_label, 0, wxALIGN_CENTER_VERTICAL);
    total_time_row->AddSpacer(FromDIP(4));
    total_time_row->Add(m_total_time_value, 0, wxALIGN_CENTER_VERTICAL);
    controls_col->Add(total_time_row, 0, wxBOTTOM, FromDIP(6));

    auto *progress_controls_row = new wxBoxSizer(wxHORIZONTAL);
    const int print_progress_bar_h = FromDIP(24);
    m_print_progress_bar = new ProgressBar(progress_box, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, print_progress_bar_h));
    m_print_progress_bar->SetMinSize(wxSize(-1, print_progress_bar_h));
    m_print_progress_bar->SetBackgroundColour(wxColour(28, 30, 34));
    // Full capsule ends: radius must not exceed half the bar height (larger values distort wx rounded rects).
    m_print_progress_bar->SetRadius(print_progress_bar_h / 2.0);
    m_print_progress_bar->SetProgressForedColour(wxColour(40, 44, 52));
    m_print_progress_bar->SetProgressBackgroundColour(wxColour(97, 114, 143));
    m_print_progress_bar->SetValue(0);
    m_print_progress_bar->ShowNumber(true);
    progress_controls_row->Add(m_print_progress_bar, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    m_progress_percent_label = new wxStaticText(progress_box, wxID_ANY, "0%");
    m_progress_percent_label->Hide();
    m_pause_resume_icon = new wxStaticBitmap(progress_box, wxID_ANY, create_scaled_bitmap("pause", this, 20));
    m_pause_resume_icon->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    m_pause_resume_icon->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online())
            return;
        if (obj->can_resume()) {
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: resume current print task dev_id =" << obj->get_dev_id();
            obj->command_task_resume();
        } else {
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: pause current print task dev_id =" << obj->get_dev_id();
            obj->command_task_pause();
        }
    });
    m_pause_resume_icon->Hide();
    controls_col->Add(progress_controls_row, 0, wxEXPAND | wxTOP, FromDIP(6));

    auto *layer_info_row = new wxBoxSizer(wxHORIZONTAL);
    m_layer_label = new wxStaticText(progress_box, wxID_ANY, "Layer:");
    m_layer_label->SetForegroundColour(wxColour(150, 156, 166));
    m_layer_printer_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_layer_printer_value->SetForegroundColour(wxColour(220, 220, 220));
    auto *layer_sep = new wxStaticText(progress_box, wxID_ANY, "/");
    layer_sep->SetForegroundColour(wxColour(150, 156, 166));
    m_layer_file_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_layer_file_value->SetForegroundColour(wxColour(220, 220, 220));
    layer_info_row->Add(m_layer_label, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddSpacer(FromDIP(4));
    layer_info_row->Add(m_layer_printer_value, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->Add(layer_sep, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->Add(m_layer_file_value, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddStretchSpacer(1);
    m_estimated_finish_label = new wxStaticText(progress_box, wxID_ANY, "Remaining:");
    m_estimated_finish_label->SetForegroundColour(wxColour(150, 156, 166));
    m_estimated_finish_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_estimated_finish_value->SetForegroundColour(wxColour(220, 220, 220));
    layer_info_row->Add(m_estimated_finish_label, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddSpacer(FromDIP(4));
    layer_info_row->Add(m_estimated_finish_value, 0, wxALIGN_CENTER_VERTICAL);
    controls_col->Add(layer_info_row, 0, wxEXPAND | wxTOP, FromDIP(6));

    controls_col->AddStretchSpacer(1);

    auto *action_row = new wxBoxSizer(wxHORIZONTAL);
    auto *pause_btn = new Button(progress_box, _L("Pause"), "print_control_pause_amber", 0, 14);
    pause_btn->SetMinSize(wxSize(FromDIP(80), FromDIP(40)));
    pause_btn->SetMaxSize(wxSize(FromDIP(80), FromDIP(40)));
    pause_btn->SetCornerRadius(FromDIP(8));
    pause_btn->SetBackgroundColorNormal(wxColour(0xFF, 0xF9, 0xF1));
    pause_btn->SetBorderColorNormal(wxColour(0xD7, 0xA4, 0x6D));
    pause_btn->SetTextColorNormal(wxColour(0xD7, 0xA4, 0x6D));
    pause_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online()) return;
        if (obj->can_resume()) {
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: resume task dev_id=" << obj->get_dev_id();
            obj->command_task_resume();
        } else {
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: pause task dev_id=" << obj->get_dev_id();
            obj->command_task_pause();
        }
    });
    action_row->Add(pause_btn, 0, wxRIGHT, FromDIP(10));

    auto *stop_btn = new Button(progress_box, _L("Stop"), "print_control_stop_red", 0, 14);
    stop_btn->SetMinSize(wxSize(FromDIP(80), FromDIP(40)));
    stop_btn->SetMaxSize(wxSize(FromDIP(80), FromDIP(40)));
    stop_btn->SetCornerRadius(FromDIP(8));
    stop_btn->SetBackgroundColorNormal(wxColour(0xFF, 0xF9, 0xF9));
    stop_btn->SetBorderColorNormal(wxColour(0xFF, 0x7D, 0x72));
    stop_btn->SetTextColorNormal(wxColour(0xFF, 0x7D, 0x72));
    stop_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj != nullptr && obj->is_online()) {
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: stop task dev_id=" << obj->get_dev_id();
            obj->command_task_abort();
        }
    });
    action_row->Add(stop_btn, 0);
    controls_col->Add(action_row, 0, wxTOP | wxBOTTOM, FromDIP(5));

    progress_content_row->Add(controls_col, 1, wxEXPAND | wxRIGHT, FromDIP(15));

    progress_box_sizer->Add(progress_content_row, 1, wxEXPAND);
    progress_box_sizer->AddStretchSpacer(1);
    progress_box->SetSizer(progress_box_sizer);

    auto make_upper_placeholder_box = [this, left_container]() {
        auto *box = new StaticBox(left_container, wxID_ANY);
        box->SetMinSize(wxSize(FromDIP(775), FromDIP(340)));
        box->SetCornerRadius(FromDIP(12));
        box->SetBorderWidth(1);
        box->SetBorderColorNormal(wxColour(55, 58, 64));
        box->SetBackgroundColorNormal(wxColour(22, 24, 29));
        box->SetBackgroundColour(wxColour(28, 30, 34));
        return box;
    };

    auto *upper_placeholder_box = make_upper_placeholder_box();

    auto *upper_placeholder_sizer = new wxBoxSizer(wxVERTICAL);
    auto *upper_header_row = new wxBoxSizer(wxHORIZONTAL);
    const int left_shell_width = FromDIP(206);
    const int right_shell_width = FromDIP(136);
    const int tool_row_gap = FromDIP(16);
    const int arrow_column_width = FromDIP(24);
    const int color_strip_vertical_inset = FromDIP(1);

    auto *model_colors_label = new wxStaticText(upper_placeholder_box, wxID_ANY, "Model Colors");
    model_colors_label->SetForegroundColour(wxColour(151, 151, 151));
    auto *model_colors_slot = new wxBoxSizer(wxHORIZONTAL);
    model_colors_slot->Add(model_colors_label, 0, wxALIGN_CENTER_VERTICAL);

    upper_header_row->AddStretchSpacer(1);
    upper_header_row->Add(model_colors_slot, 0, wxALIGN_CENTER_VERTICAL | wxFIXED_MINSIZE);
    upper_header_row->SetItemMinSize(model_colors_slot, left_shell_width, -1);
    upper_header_row->AddSpacer(tool_row_gap);
    upper_header_row->AddSpacer(arrow_column_width);
    upper_header_row->AddSpacer(tool_row_gap);

    auto *assigned_tools_label = new wxStaticText(upper_placeholder_box, wxID_ANY, "Assigned Tools");
    assigned_tools_label->SetForegroundColour(wxColour(151, 151, 151));
    auto *assigned_tools_slot = new wxBoxSizer(wxHORIZONTAL);
    assigned_tools_slot->Add(assigned_tools_label, 0, wxALIGN_CENTER_VERTICAL);
    upper_header_row->Add(assigned_tools_slot, 0, wxALIGN_CENTER_VERTICAL | wxFIXED_MINSIZE);
    upper_header_row->SetItemMinSize(assigned_tools_slot, right_shell_width, -1);
    upper_header_row->AddStretchSpacer(1);

    upper_placeholder_sizer->Add(upper_header_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
    upper_placeholder_sizer->AddSpacer(FromDIP(10));

    auto *upper_header_divider = new wxPanel(upper_placeholder_box, wxID_ANY);
    upper_header_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    upper_header_divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    upper_header_divider->SetBackgroundColour(wxColour(44, 129, 255));
    upper_placeholder_sizer->Add(upper_header_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    upper_placeholder_sizer->AddSpacer(FromDIP(8));

    const std::array<wxColour, 4> filament_colors = {
        wxColour(214, 181, 46),
        wxColour(57, 145, 212),
        wxColour(217, 101, 43),
        wxColour(164, 207, 42)
    };

    for (int i = 0; i < 4; ++i) {
        auto *tool_row = new wxBoxSizer(wxHORIZONTAL);
        tool_row->AddStretchSpacer(1);

        auto *left_shell = new StaticBox(upper_placeholder_box, wxID_ANY);
        left_shell->SetMinSize(wxSize(FromDIP(206), FromDIP(44)));
        left_shell->SetMaxSize(wxSize(FromDIP(206), FromDIP(44)));
        left_shell->SetCornerRadius(FromDIP(12));
        left_shell->SetBorderWidth(0);
        left_shell->SetBackgroundColorNormal(wxColour(43, 46, 52));
        left_shell->SetBackgroundColour(wxColour(43, 46, 52));
        auto *left_shell_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *left_color = new StaticBox(left_shell, wxID_ANY);
        left_color->SetMinSize(wxSize(FromDIP(36), FromDIP(44)));
        left_color->SetMaxSize(wxSize(FromDIP(36), FromDIP(44)));
        left_color->SetCornerRadius(FromDIP(12));
        left_color->SetBorderWidth(0);
        left_color->SetBackgroundColorNormal(filament_colors[i]);
        left_color->SetBackgroundColour(filament_colors[i]);
        left_shell_sizer->Add(left_color, 0, wxEXPAND | wxTOP | wxBOTTOM, color_strip_vertical_inset);
        left_shell_sizer->AddSpacer(FromDIP(14));

        auto *material_label = new wxStaticText(left_shell, wxID_ANY, "PLA");
        material_label->SetForegroundColour(wxColour(235, 235, 235));
        wxFont material_font = material_label->GetFont();
        material_font.SetWeight(wxFONTWEIGHT_BOLD);
        material_label->SetFont(material_font);
        left_shell_sizer->Add(material_label, 0, wxALIGN_CENTER_VERTICAL);
        left_shell_sizer->AddStretchSpacer(1);

        auto *weight_label = new wxStaticText(left_shell, wxID_ANY, "14.3g");
        weight_label->SetForegroundColour(wxColour(220, 220, 220));
        left_shell_sizer->Add(weight_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
        left_shell->SetSizer(left_shell_sizer);
        tool_row->Add(left_shell, 0, wxALIGN_CENTER_VERTICAL);

        tool_row->AddSpacer(FromDIP(16));
        auto *arrow_label = new wxStaticText(upper_placeholder_box, wxID_ANY, ">");
        arrow_label->SetForegroundColour(wxColour(200, 200, 200));
        wxFont arrow_font = arrow_label->GetFont();
        arrow_font.SetPointSize(arrow_font.GetPointSize() + 4);
        arrow_label->SetFont(arrow_font);
        tool_row->Add(arrow_label, 0, wxALIGN_CENTER_VERTICAL);
        tool_row->AddSpacer(FromDIP(16));

        auto *right_shell = new StaticBox(upper_placeholder_box, wxID_ANY);
        right_shell->SetMinSize(wxSize(FromDIP(136), FromDIP(44)));
        right_shell->SetMaxSize(wxSize(FromDIP(136), FromDIP(44)));
        right_shell->SetCornerRadius(FromDIP(12));
        right_shell->SetBorderWidth(0);
        right_shell->SetBackgroundColorNormal(wxColour(43, 46, 52));
        right_shell->SetBackgroundColour(wxColour(43, 46, 52));
        auto *right_shell_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *right_color = new StaticBox(right_shell, wxID_ANY);
        right_color->SetMinSize(wxSize(FromDIP(18), FromDIP(44)));
        right_color->SetMaxSize(wxSize(FromDIP(18), FromDIP(44)));
        right_color->SetCornerRadius(FromDIP(12));
        right_color->SetBorderWidth(0);
        right_color->SetBackgroundColorNormal(filament_colors[i]);
        right_color->SetBackgroundColour(filament_colors[i]);
        right_shell_sizer->Add(right_color, 0, wxEXPAND | wxTOP | wxBOTTOM, color_strip_vertical_inset);
        right_shell_sizer->AddSpacer(FromDIP(16));

        auto *tool_label = new wxStaticText(right_shell, wxID_ANY, wxString::Format("T%d", i + 1));
        tool_label->SetForegroundColour(wxColour(235, 235, 235));
        wxFont tool_font = tool_label->GetFont();
        tool_font.SetWeight(wxFONTWEIGHT_BOLD);
        tool_label->SetFont(tool_font);
        right_shell_sizer->Add(tool_label, 0, wxALIGN_CENTER_VERTICAL);
        right_shell_sizer->AddStretchSpacer(1);

        auto *refresh_label = new wxStaticText(right_shell, wxID_ANY, "<>");
        refresh_label->SetForegroundColour(wxColour(200, 200, 200));
        right_shell_sizer->Add(refresh_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
        right_shell->SetSizer(right_shell_sizer);
        tool_row->Add(right_shell, 0, wxALIGN_CENTER_VERTICAL);
        tool_row->AddStretchSpacer(1);

        upper_placeholder_sizer->Add(tool_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    }

    upper_placeholder_sizer->AddStretchSpacer(1);

    auto *filament_qt_sizer = new wxBoxSizer(wxHORIZONTAL);
    filament_qt_sizer->Add(upper_placeholder_sizer, 550, wxEXPAND);

    auto *mf_divider = new wxPanel(upper_placeholder_box, wxID_ANY);
    mf_divider->SetMinSize(wxSize(FromDIP(1), -1));
    mf_divider->SetMaxSize(wxSize(FromDIP(1), -1));
    mf_divider->SetBackgroundColour(wxColour(55, 58, 64));
    filament_qt_sizer->Add(mf_divider, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));

    auto *mf_sizer = new wxBoxSizer(wxVERTICAL);

    auto *mf_title = new wxStaticText(upper_placeholder_box, wxID_ANY, "Manage Filament");
    mf_title->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = mf_title->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        mf_title->SetFont(f);
    }
    mf_sizer->Add(mf_title, 0, wxLEFT | wxTOP, FromDIP(12));
    m_manage_filament_title = mf_title;
    mf_sizer->AddSpacer(FromDIP(6));

    auto *mf_sep = new wxPanel(upper_placeholder_box, wxID_ANY);
    mf_sep->SetMinSize(wxSize(-1, FromDIP(1)));
    mf_sep->SetMaxSize(wxSize(-1, FromDIP(1)));
    mf_sep->SetBackgroundColour(wxColour(55, 58, 64));
    mf_sizer->Add(mf_sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    mf_sizer->AddSpacer(FromDIP(12));

    // Tool selector
    auto *tool_selector = new StaticBox(upper_placeholder_box, wxID_ANY);
    tool_selector->SetMinSize(wxSize(-1, FromDIP(44)));
    tool_selector->SetMaxSize(wxSize(-1, FromDIP(44)));
    tool_selector->SetCornerRadius(FromDIP(8));
    tool_selector->SetBorderWidth(1);
    tool_selector->SetBorderColorNormal(wxColour(70, 73, 80));
    tool_selector->SetBackgroundColorNormal(wxColour(43, 46, 52));
    tool_selector->SetBackgroundColour(wxColour(43, 46, 52));
    tool_selector->SetCursor(wxCursor(wxCURSOR_ARROW));
    auto *tool_sel_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *tool_color_dot = new StaticBox(tool_selector, wxID_ANY);
    tool_color_dot->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    tool_color_dot->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    tool_color_dot->SetCornerRadius(FromDIP(8));
    tool_color_dot->SetBorderWidth(0);
    tool_color_dot->SetBackgroundColorNormal(filament_colors[0]);
    tool_color_dot->SetBackgroundColour(filament_colors[0]);
    tool_sel_sizer->Add(tool_color_dot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    tool_sel_sizer->AddSpacer(FromDIP(8));
    auto *tool_name_lbl = new wxStaticText(tool_selector, wxID_ANY, "Tool 1");
    tool_name_lbl->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = tool_name_lbl->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        tool_name_lbl->SetFont(f);
    }
    tool_sel_sizer->Add(tool_name_lbl, 1, wxALIGN_CENTER_VERTICAL);
    auto *dropdown_arrow = new wxStaticText(tool_selector, wxID_ANY, wxString::FromUTF8("\xE2\x8C\x84"));
    dropdown_arrow->SetForegroundColour(wxColour(150, 155, 165));
    tool_sel_sizer->Add(dropdown_arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    tool_selector->SetSizer(tool_sel_sizer);
    mf_sizer->Add(tool_selector, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    mf_sizer->AddSpacer(FromDIP(10));

    m_filament_tool_color_dot = tool_color_dot;
    m_filament_tool_name_lbl  = tool_name_lbl;
    m_filament_tool_selector   = tool_selector;
    m_selected_filament_tool  = 0;

    // Load button
    auto *load_btn = new Button(upper_placeholder_box, _L("Load"));
    load_btn->SetMinSize(wxSize(-1, FromDIP(40)));
    load_btn->SetCornerRadius(FromDIP(8));
    load_btn->SetBorderWidth(0);
    load_btn->SetBackgroundColorNormal(wxColour(65, 68, 75));
    load_btn->SetTextColorNormal(wxColour(220, 220, 220));
    load_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { show_filament_load_wizard(); });
    mf_sizer->Add(load_btn, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    mf_sizer->AddSpacer(FromDIP(8));

    // Unload button
    auto *unload_btn = new Button(upper_placeholder_box, _L("Unload"));
    unload_btn->SetMinSize(wxSize(-1, FromDIP(40)));
    unload_btn->SetCornerRadius(FromDIP(8));
    unload_btn->SetBorderWidth(0);
    unload_btn->SetBackgroundColorNormal(wxColour(65, 68, 75));
    unload_btn->SetTextColorNormal(wxColour(220, 220, 220));
    unload_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online() || obj->is_in_printing()) return;
        const std::string slot = std::to_string(m_selected_filament_tool);
        BOOST_LOG_TRIVIAL(info) << "PrinterWebView: unload filament slot=" << slot;
        obj->command_ams_change_filament(false, "0", slot);
    });
    mf_sizer->Add(unload_btn, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_filament_load_btn   = load_btn;
    m_filament_unload_btn = unload_btn;

    mf_sizer->AddStretchSpacer(1);
    filament_qt_sizer->Add(mf_sizer, 224, wxEXPAND);
    upper_placeholder_box->SetSizer(filament_qt_sizer);

    auto *right_container = new StaticBox(left_container, wxID_ANY);
    right_container->SetCornerRadius(FromDIP(12));
    right_container->SetBorderWidth(1);
    right_container->SetBorderColorNormal(wxColour(55, 58, 64));
    right_container->SetBackgroundColorNormal(wxColour(22, 24, 29));
    right_container->SetBackgroundColour(wxColour(22, 24, 29));
    const int right_container_width = FromDIP(775);
    const int right_container_height = FromDIP(392);
    right_container->SetMinSize(wxSize(right_container_width, right_container_height));
    right_container->SetMaxSize(wxSize(-1, right_container_height));
    auto *right_sizer = new wxBoxSizer(wxVERTICAL);

    auto make_btn = [this, right_container](const wxString &txt, int w, int h, bool active = false) {
        auto *btn = new wxButton(right_container, wxID_ANY, txt, wxDefaultPosition, wxSize(this->FromDIP(w), this->FromDIP(h)));
        btn->SetBackgroundColour(active ? wxColour(210, 210, 210) : wxColour(61, 64, 68));
        btn->SetForegroundColour(active ? wxColour(40, 40, 40) : wxColour(215, 215, 215));
        btn->SetWindowStyleFlag(wxBORDER_NONE);
        return btn;
    };

    auto make_step_btn = [this](wxWindow *parent, const wxString &txt, bool active = false) {
        auto *btn = new Button(parent, txt);
        btn->SetMinSize(wxSize(this->FromDIP(73), this->FromDIP(45)));
        btn->SetCornerRadius(this->FromDIP(8));
        btn->SetBorderWidth(1);
        btn->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
        btn->SetBackgroundColorNormal(wxColour(43, 46, 52));
        btn->SetTextColorNormal(active ? wxColour(44, 182, 125) : wxColour(185, 190, 200));
        return btn;
    };

    auto *step_container = new StaticBox(right_container, wxID_ANY);
    step_container->SetCornerRadius(FromDIP(8));
    step_container->SetBorderWidth(1);
    step_container->SetBorderColorNormal(wxColour(55, 58, 64));
    step_container->SetBackgroundColorNormal(wxColour(22, 24, 29));
    step_container->SetBackgroundColour(wxColour(22, 24, 29));
    auto *step_container_sizer = new wxBoxSizer(wxVERTICAL);
    auto *motion_lbl   = new wxStaticText(step_container, wxID_ANY, "Motion");
    auto *distance_lbl = new wxStaticText(step_container, wxID_ANY, "Distance");
    for (auto *l : { motion_lbl, distance_lbl }) {
        l->SetForegroundColour(wxColour(150, 155, 165));
        wxFont f = l->GetFont(); if (f.GetPointSize() > 1) f.SetPointSize(f.GetPointSize() - 1); l->SetFont(f);
    }
    step_container_sizer->Add(motion_lbl,   0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    step_container_sizer->Add(distance_lbl, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(1));
    step_container_sizer->AddSpacer(FromDIP(8));

    auto *step_1_btn = make_step_btn(step_container, "1mm", true);
    auto *step_5_btn = make_step_btn(step_container, "5mm");
    auto *step_10_btn = make_step_btn(step_container, "10mm");
    auto *step_20_btn = make_step_btn(step_container, "20mm");
    std::vector<Button *> step_buttons { step_1_btn, step_5_btn, step_10_btn, step_20_btn };
    auto select_axis_step = [this, step_buttons](double step, Button *active_btn) {
        m_axis_move_step = step;
        for (auto *btn : step_buttons) {
            const bool active = btn == active_btn;
            btn->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
            btn->SetTextColorNormal(active ? wxColour(44, 182, 125) : wxColour(185, 190, 200));
            btn->Refresh();
        }
    };
    step_1_btn->Bind(wxEVT_BUTTON, [select_axis_step, step_1_btn](wxCommandEvent &) { select_axis_step(1.0, step_1_btn); });
    step_5_btn->Bind(wxEVT_BUTTON, [select_axis_step, step_5_btn](wxCommandEvent &) { select_axis_step(5.0, step_5_btn); });
    step_10_btn->Bind(wxEVT_BUTTON, [select_axis_step, step_10_btn](wxCommandEvent &) { select_axis_step(10.0, step_10_btn); });
    step_20_btn->Bind(wxEVT_BUTTON, [select_axis_step, step_20_btn](wxCommandEvent &) { select_axis_step(20.0, step_20_btn); });
    step_container_sizer->Add(step_1_btn,  0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
    step_container_sizer->AddSpacer(FromDIP(6));
    step_container_sizer->Add(step_5_btn,  0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
    step_container_sizer->AddSpacer(FromDIP(6));
    step_container_sizer->Add(step_10_btn, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
    step_container_sizer->AddSpacer(FromDIP(6));
    step_container_sizer->Add(step_20_btn, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    step_container->SetSizer(step_container_sizer);

    auto *content_row = new wxBoxSizer(wxHORIZONTAL);

    auto make_tool_btn = [this](wxWindow *parent, const wxString &txt, bool active = false) {
        auto *btn = new Button(parent, txt);
        const wxSize button_size(this->FromDIP(92), this->FromDIP(58));
        btn->SetMinSize(button_size);
        btn->SetMaxSize(button_size);
        btn->SetSize(button_size);
        btn->SetCornerRadius(this->FromDIP(10));
        btn->SetBorderWidth(0);
        btn->SetBackgroundColorNormal(active ? wxColour(210, 210, 210) : wxColour(61, 64, 68));
        btn->SetTextColorNormal(active ? wxColour(40, 40, 40) : wxColour(215, 215, 215));
        return btn;
    };

    auto *tool_col = new wxBoxSizer(wxVERTICAL);
    auto *t1_btn = make_tool_btn(right_container, "T1", true);
    auto *t2_btn = make_tool_btn(right_container, "T2");
    auto *t3_btn = make_tool_btn(right_container, "T3");
    auto *t4_btn = make_tool_btn(right_container, "T4");
    t1_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    t1_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_selected_extruder = "T1";
        if (m_extruder_display_label != nullptr)
            m_extruder_display_label->SetLabelText(m_selected_extruder);
        m_selected_fan = "T1";
        if (m_fan_display_label != nullptr)
            m_fan_display_label->SetLabelText(m_selected_fan);
        refresh_fan_value_display();
    });
    for (auto *disabled_btn : { t2_btn, t3_btn, t4_btn }) {
        disabled_btn->Enable(false);
        disabled_btn->SetBackgroundColorNormal(wxColour(43, 46, 52));
        disabled_btn->SetTextColorNormal(wxColour(115, 118, 124));
    }
    tool_col->Add(t1_btn, 0, wxBOTTOM, FromDIP(10));
    tool_col->Add(t2_btn, 0, wxBOTTOM, FromDIP(10));
    tool_col->Add(t3_btn, 0, wxBOTTOM, FromDIP(10));
    tool_col->Add(t4_btn, 0);
    content_row->Add(tool_col, 0, wxTOP, FromDIP(18));

    auto icon_exists = [](const std::string &icon_name) {
        return wxFileName::FileExists(from_u8(Slic3r::var(icon_name + ".png"))) ||
               wxFileName::FileExists(from_u8(Slic3r::var(icon_name + ".svg")));
    };

    auto resolve_icon = [icon_exists](const std::string &primary_icon, const std::string &secondary_icon) {
        if (icon_exists(primary_icon))
            return primary_icon;
        if (!secondary_icon.empty() && icon_exists(secondary_icon))
            return secondary_icon;
        return std::string();
    };

    const int xy_square = FromDIP(300);
    const int center_size = FromDIP(85);
    const int center_pos = (xy_square - center_size) / 2;
    const int axis_center_gap = FromDIP(4);
    const int axis_button_gap = FromDIP(3);
    const int horizontal_icon_width = FromDIP(210);
    const int horizontal_icon_height = FromDIP(76);
    const int vertical_icon_width = FromDIP(76);
    const int vertical_icon_height = FromDIP(210);

    auto *xy_area = new AxisJoystickPanel(
        right_container,
        xy_square, center_size, axis_center_gap, axis_button_gap,
        horizontal_icon_width, horizontal_icon_height,
        vertical_icon_width,   vertical_icon_height);
    xy_area->SetCursor(wxCursor(wxCURSOR_HAND));
    xy_area->SetBackgroundColour(wxColour(22, 24, 29));
    auto send_axis_action = [this](AxisControlAction action) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online()) {
            BOOST_LOG_TRIVIAL(warning) << "PrinterWebView axis control ignored: no online selected printer";
            return;
        }

        const double step = m_axis_move_step;
        switch (action) {
        case AxisControlAction::XMinus:
            obj->command_axis_control("X", 1.0, -step, 3000);
            break;
        case AxisControlAction::XPlus:
            obj->command_axis_control("X", 1.0, step, 3000);
            break;
        case AxisControlAction::YMinus:
            obj->command_axis_control("Y", 1.0, -step, 3000);
            break;
        case AxisControlAction::YPlus:
            obj->command_axis_control("Y", 1.0, step, 3000);
            break;
        case AxisControlAction::None:
            break;
        }
    };
    xy_area->set_action_handler(send_axis_action);

    auto *center_btn = new Button(xy_area, "", "home", 0, 34);
    center_btn->SetSize(wxRect(wxPoint(center_pos, center_pos), wxSize(center_size, center_size)));
    center_btn->SetMinSize(wxSize(center_size, center_size));
    center_btn->SetMaxSize(wxSize(center_size, center_size));
    center_btn->SetCornerRadius(FromDIP(18));
    center_btn->SetBorderWidth(0);
    center_btn->SetBackgroundColorNormal(wxColour(255, 255, 255));
    center_btn->SetBackgroundColour(wxColour(22, 24, 29));
    center_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    center_btn->Bind(wxEVT_BUTTON, [](wxCommandEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online()) {
            BOOST_LOG_TRIVIAL(warning) << "PrinterWebView home control ignored: no online selected printer";
            return;
        }
        obj->command_go_home();
    });

    content_row->AddSpacer(FromDIP(20));
    content_row->Add(xy_area, 0);

    auto make_icon_btn = [this, &make_btn](const std::string &icon_key, int w, int h, bool transparent_bg = false, int icon_w = -1, int icon_h = -1) {
        auto *btn = make_btn("", w, h, true);
        if (transparent_bg) {
            btn->SetBackgroundColour(wxColour(22, 24, 29));
            btn->SetForegroundColour(wxColour(22, 24, 29));
        }
        if (!icon_key.empty()) {
            if (icon_w > 0 && icon_h > 0) {
                const std::string png_path = Slic3r::var(icon_key + ".png");
                if (wxFileName::FileExists(from_u8(png_path))) {
                    wxImage img(from_u8(png_path), wxBITMAP_TYPE_PNG);
                    if (img.IsOk()) {
                        const int target_w = this->FromDIP(icon_w);
                        const int target_h = this->FromDIP(icon_h);
                        btn->SetBitmap(wxBitmap(img.Scale(target_w, target_h, wxIMAGE_QUALITY_HIGH)));
                        return btn;
                    }
                }
            }

            const int icon_px = this->ToDIP(wxSize(0, h)).GetHeight();
            btn->SetBitmap(create_scaled_bitmap(icon_key, this, icon_px > 0 ? icon_px : 1));
        }
        return btn;
    };

    auto *z_col = new wxBoxSizer(wxVERTICAL);
    auto *top_btn = make_icon_btn(resolve_icon("rectangle_10", ""), 90, 75, true);
    top_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    z_col->Add(top_btn, 0, wxLEFT, FromDIP(2));
    z_col->AddSpacer(FromDIP(5));
    auto *top_z_label = new wxStaticText(top_btn, wxID_ANY, "+Z");
    top_z_label->SetForegroundColour(wxColour(45, 48, 55));
    top_z_label->SetBackgroundColour(wxColour(214, 214, 214));
    const wxSize top_btn_size = top_btn->GetSize();
    const wxSize top_z_label_size = top_z_label->GetBestSize();
    top_z_label->SetPosition(wxPoint(
        (top_btn_size.GetWidth() - top_z_label_size.GetWidth()) / 2,
        (top_btn_size.GetHeight() - top_z_label_size.GetHeight()) / 2));

    auto *center_home_box = new Button(right_container, "", "home", 0, 40);
    center_home_box->SetMinSize(wxSize(FromDIP(80), FromDIP(75)));
    center_home_box->SetMaxSize(wxSize(FromDIP(80), FromDIP(75)));
    center_home_box->SetCornerRadius(FromDIP(15));
    center_home_box->SetBorderWidth(0);
    center_home_box->SetBackgroundColorNormal(wxColour(255, 255, 255));
    center_home_box->SetBackgroundColour(wxColour(22, 24, 29));
    center_home_box->SetCursor(wxCursor(wxCURSOR_HAND));

    z_col->Add(center_home_box, 0, wxLEFT | wxBOTTOM, FromDIP(7));
    auto *bottom_split_host = new wxPanel(right_container, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(89), FromDIP(75)));
    bottom_split_host->SetMinSize(wxSize(FromDIP(89), FromDIP(75)));
    bottom_split_host->SetMaxSize(wxSize(FromDIP(89), FromDIP(75)));
    bottom_split_host->SetBackgroundColour(wxColour(22, 24, 29));
    bottom_split_host->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *bottom_split_bitmap = new wxStaticBitmap(bottom_split_host, wxID_ANY, create_scaled_bitmap("rectangle_12", this, 75));
    const wxSize bottom_split_bitmap_size = bottom_split_bitmap->GetBestSize();
    bottom_split_bitmap->SetPosition(wxPoint(
        (bottom_split_host->GetMinSize().GetWidth() - bottom_split_bitmap_size.GetWidth()) / 2,
        (bottom_split_host->GetMinSize().GetHeight() - bottom_split_bitmap_size.GetHeight()) / 2));
    auto *bottom_divider = new wxPanel(bottom_split_host, wxID_ANY, wxPoint(FromDIP(44), 0), wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetMinSize(wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetMaxSize(wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetBackgroundColour(wxColour(70, 74, 82));
    auto *bottom_z_label = new wxStaticText(bottom_split_host, wxID_ANY, "-Z");
    bottom_z_label->SetForegroundColour(wxColour(45, 48, 55));
    bottom_z_label->SetBackgroundColour(wxColour(214, 214, 214));
    const wxSize bottom_z_label_size = bottom_z_label->GetBestSize();
    bottom_z_label->SetPosition(wxPoint(
        (bottom_split_host->GetMinSize().GetWidth() - bottom_z_label_size.GetWidth()) / 2,
        (bottom_split_host->GetMinSize().GetHeight() - bottom_z_label_size.GetHeight()) / 2));
    auto send_z_axis = [this](double direction) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online()) {
            BOOST_LOG_TRIVIAL(warning) << "PrinterWebView Z control ignored: no online selected printer";
            return;
        }

        obj->command_axis_control("Z", 1.0, direction * m_axis_move_step, 900);
    };
    top_btn->Bind(wxEVT_BUTTON, [send_z_axis](wxCommandEvent &) { send_z_axis(-1.0); });
    center_home_box->Bind(wxEVT_BUTTON, [](wxCommandEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj != nullptr && obj->is_online())
            obj->command_go_home();
    });
    bottom_split_host->Bind(wxEVT_LEFT_UP, [send_z_axis](wxMouseEvent &) { send_z_axis(1.0); });
    bottom_split_bitmap->Bind(wxEVT_LEFT_UP, [send_z_axis](wxMouseEvent &) { send_z_axis(1.0); });
    bottom_z_label->Bind(wxEVT_LEFT_UP, [send_z_axis](wxMouseEvent &) { send_z_axis(1.0); });
    z_col->Add(bottom_split_host, 0, wxLEFT, FromDIP(2));
    content_row->AddSpacer(FromDIP(20));
    content_row->Add(z_col, 0, wxALIGN_TOP | wxTOP, FromDIP(22));
    content_row->AddSpacer(FromDIP(20));
    content_row->Add(step_container, 0, wxALIGN_TOP | wxTOP, FromDIP(18));

    auto make_speed_btn = [this](wxWindow *parent, const wxString &txt, bool active = false) {
        auto *btn = new Button(parent, txt);
        btn->SetMinSize(wxSize(this->FromDIP(73), this->FromDIP(45)));
        btn->SetCornerRadius(this->FromDIP(8));
        btn->SetBorderWidth(1);
        btn->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
        btn->SetBackgroundColorNormal(wxColour(43, 46, 52));
        btn->SetTextColorNormal(active ? wxColour(44, 182, 125) : wxColour(185, 190, 200));
        return btn;
    };
    auto *speed_container = new StaticBox(right_container, wxID_ANY);
    speed_container->SetCornerRadius(FromDIP(8));
    speed_container->SetBorderWidth(1);
    speed_container->SetBorderColorNormal(wxColour(55, 58, 64));
    speed_container->SetBackgroundColorNormal(wxColour(22, 24, 29));
    speed_container->SetBackgroundColour(wxColour(22, 24, 29));
    auto *speed_container_sizer = new wxBoxSizer(wxVERTICAL);
    auto *printing_lbl = new wxStaticText(speed_container, wxID_ANY, "Printing");
    auto *speed_lbl    = new wxStaticText(speed_container, wxID_ANY, "Speed");
    for (auto *l : { printing_lbl, speed_lbl }) {
        l->SetForegroundColour(wxColour(150, 155, 165));
        wxFont f = l->GetFont(); if (f.GetPointSize() > 1) f.SetPointSize(f.GetPointSize() - 1); l->SetFont(f);
    }
    speed_container_sizer->Add(printing_lbl, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    speed_container_sizer->Add(speed_lbl,    0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(1));
    speed_container_sizer->AddSpacer(FromDIP(8));

    auto *slow_btn   = make_speed_btn(speed_container, "Slow");
    auto *normal_btn = make_speed_btn(speed_container, "Normal");
    auto *fast_btn   = make_speed_btn(speed_container, "Fast");
    auto *ultra_btn  = make_speed_btn(speed_container, "Ultra");
    std::vector<Button *> speed_buttons { slow_btn, normal_btn, fast_btn, ultra_btn };
    auto select_speed_btn = [this, speed_buttons](Button *active_btn, DevPrintingSpeedLevel level) {
        for (auto *btn : speed_buttons) {
            const bool active = btn == active_btn;
            btn->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
            btn->SetTextColorNormal(active ? wxColour(44, 182, 125) : wxColour(185, 190, 200));
            btn->Refresh();
        }
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj != nullptr && obj->is_online())
            obj->command_set_printing_speed(level);
    };
    slow_btn->Bind(wxEVT_BUTTON, [select_speed_btn, slow_btn](wxCommandEvent &) {
        select_speed_btn(slow_btn, SPEED_LEVEL_SILENCE);
    });
    normal_btn->Bind(wxEVT_BUTTON, [select_speed_btn, normal_btn](wxCommandEvent &) {
        select_speed_btn(normal_btn, SPEED_LEVEL_NORMAL);
    });
    fast_btn->Bind(wxEVT_BUTTON, [select_speed_btn, fast_btn](wxCommandEvent &) {
        select_speed_btn(fast_btn, SPEED_LEVEL_RAPID);
    });
    ultra_btn->Bind(wxEVT_BUTTON, [select_speed_btn, ultra_btn](wxCommandEvent &) {
        select_speed_btn(ultra_btn, SPEED_LEVEL_RAMPAGE);
    });
    speed_container_sizer->Add(slow_btn,   0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
    speed_container_sizer->AddSpacer(FromDIP(6));
    speed_container_sizer->Add(normal_btn, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
    speed_container_sizer->AddSpacer(FromDIP(6));
    speed_container_sizer->Add(fast_btn,   0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
    speed_container_sizer->AddSpacer(FromDIP(6));
    speed_container_sizer->Add(ultra_btn,  0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    speed_container->SetSizer(speed_container_sizer);
    content_row->AddSpacer(FromDIP(40));
    content_row->Add(speed_container, 0, wxALIGN_TOP | wxTOP, FromDIP(18));

    auto *movement_title_label = new wxStaticText(right_container, wxID_ANY, "Movement");
    movement_title_label->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = movement_title_label->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        movement_title_label->SetFont(f);
    }
    auto *movement_title_row = new wxBoxSizer(wxHORIZONTAL);
    movement_title_row->Add(movement_title_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));

    auto *movement_sep = new wxPanel(right_container, wxID_ANY);
    movement_sep->SetMinSize(wxSize(-1, FromDIP(1)));
    movement_sep->SetMaxSize(wxSize(-1, FromDIP(1)));
    movement_sep->SetBackgroundColour(wxColour(55, 58, 64));

    auto make_movement_hdr_lbl = [this](wxWindow *parent, const wxString &txt,
                                                         int width_dip, long style = wxALIGN_CENTER_HORIZONTAL) -> wxStaticText * {
        auto *lbl = new wxStaticText(parent, wxID_ANY, txt, wxDefaultPosition,
            width_dip > 0 ? wxSize(this->FromDIP(width_dip), -1) : wxDefaultSize, style);
        lbl->SetForegroundColour(wxColour(150, 155, 165));
        wxFont f = lbl->GetFont();
        if (f.GetPointSize() > 1)
            f.SetPointSize(f.GetPointSize() - 1);
        lbl->SetFont(f);
        return lbl;
    };

    const wxColour movement_hdr_bg(22, 24, 29);
    auto *col_labels = new wxBoxSizer(wxHORIZONTAL);
    {
        // "Tool" / "Selection" — centered over T column (matches tool button width 92 DIP).
        auto *tool_cell = new wxPanel(right_container);
        tool_cell->SetBackgroundColour(movement_hdr_bg);
        tool_cell->SetMinSize(wxSize(FromDIP(92), -1));
        auto *tool_vs = new wxBoxSizer(wxVERTICAL);
        tool_vs->Add(make_movement_hdr_lbl(tool_cell, "Tool", 0, wxALIGN_CENTER_HORIZONTAL), 0, wxEXPAND);
        tool_vs->Add(make_movement_hdr_lbl(tool_cell, "Selection", 0, wxALIGN_CENTER_HORIZONTAL), 0, wxEXPAND);
        tool_cell->SetSizer(tool_vs);
        col_labels->Add(tool_cell, 0);
    }
    col_labels->AddSpacer(FromDIP(20));
    {
        // "Move" / "X,Y axis" — centered over joystick (xy_square 300 DIP).
        auto *xy_cell = new wxPanel(right_container);
        xy_cell->SetBackgroundColour(movement_hdr_bg);
        xy_cell->SetMinSize(wxSize(FromDIP(300), -1));
        auto *xy_vs = new wxBoxSizer(wxVERTICAL);
        xy_vs->Add(make_movement_hdr_lbl(xy_cell, "Move", 0, wxALIGN_CENTER_HORIZONTAL), 0, wxEXPAND);
        xy_vs->Add(make_movement_hdr_lbl(xy_cell, "X,Y axis", 0, wxALIGN_CENTER_HORIZONTAL), 0, wxEXPAND);
        xy_cell->SetSizer(xy_vs);
        col_labels->Add(xy_cell, 0);
    }
    col_labels->AddSpacer(FromDIP(20));
    {
        // "Move" / "Z axis" — centered over Z column (~92 DIP).
        auto *z_cell_hdr = new wxPanel(right_container);
        z_cell_hdr->SetBackgroundColour(movement_hdr_bg);
        z_cell_hdr->SetMinSize(wxSize(FromDIP(92), -1));
        auto *z_vs = new wxBoxSizer(wxVERTICAL);
        z_vs->Add(make_movement_hdr_lbl(z_cell_hdr, "Move", 0, wxALIGN_CENTER_HORIZONTAL), 0, wxEXPAND);
        z_vs->Add(make_movement_hdr_lbl(z_cell_hdr, "Z axis", 0, wxALIGN_CENTER_HORIZONTAL), 0, wxEXPAND);
        z_cell_hdr->SetSizer(z_vs);
        col_labels->Add(z_cell_hdr, 0);
    }

    // Center the label row + control row as one unit so left/right outer margins match.
    auto *movement_body = new wxBoxSizer(wxVERTICAL);
    movement_body->Add(col_labels, 0, wxALIGN_LEFT | wxTOP, FromDIP(6));
    // Bring headers closer to controls (was 12 DIP gap).
    movement_body->Add(content_row, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(12));
    auto *movement_body_wrap = new wxBoxSizer(wxHORIZONTAL);
    movement_body_wrap->AddStretchSpacer(1);
    movement_body_wrap->Add(movement_body, 0);
    movement_body_wrap->AddStretchSpacer(1);

    right_sizer->Add(movement_title_row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
    right_sizer->Add(movement_sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    right_sizer->AddSpacer(FromDIP(8));
    right_sizer->Add(movement_body_wrap, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));

    auto *right_placeholder_box = new StaticBox(right_container, wxID_ANY);
    const int right_placeholder_height = FromDIP(240);
    const int right_placeholder_x = FromDIP(529);
    const int right_placeholder_width = FromDIP(205);
    right_placeholder_box->SetSize(wxRect(wxPoint(right_placeholder_x, FromDIP(120)), wxSize(right_placeholder_width, right_placeholder_height)));
    right_placeholder_box->SetMinSize(wxSize(right_placeholder_width, right_placeholder_height));
    right_placeholder_box->SetMaxSize(wxSize(right_placeholder_width, right_placeholder_height));
    right_placeholder_box->SetCornerRadius(FromDIP(12));
    right_placeholder_box->SetBorderWidth(1);
    right_placeholder_box->SetBorderColorNormal(wxColour(55, 58, 64));
    right_placeholder_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    right_placeholder_box->SetBackgroundColour(wxColour(28, 30, 34));

    auto add_placeholder_line = [this, right_placeholder_box, right_placeholder_width](int top_offset) {
        auto *line = new wxPanel(right_placeholder_box, wxID_ANY);
        line->SetSize(wxRect(
            wxPoint(FromDIP(0), FromDIP(top_offset)),
            wxSize(right_placeholder_width, FromDIP(1))));
        line->SetMinSize(wxSize(right_placeholder_width, FromDIP(1)));
        line->SetMaxSize(wxSize(right_placeholder_width, FromDIP(1)));
        line->SetBackgroundColour(wxColour(55, 58, 64));
    };

    add_placeholder_line(60);
    add_placeholder_line(120);
    add_placeholder_line(180);

    auto *bed_label = new wxStaticText(right_placeholder_box, wxID_ANY, "Bed");
    bed_label->SetPosition(wxPoint(FromDIP(12), FromDIP(20)));
    bed_label->SetForegroundColour(wxColour(220, 220, 220));

    auto *bed_value = new wxStaticText(right_placeholder_box, wxID_ANY, "__ / __");
    bed_value->SetPosition(wxPoint(FromDIP(107), FromDIP(20)));
    bed_value->SetForegroundColour(wxColour(220, 220, 220));
    bed_value->SetCursor(wxCursor(wxCURSOR_HAND));
    m_bed_temp_value = bed_value;

    auto *bed_unit = new wxStaticText(right_placeholder_box, wxID_ANY, wxString::FromUTF8("\xC2\xB0""C"));
    bed_unit->SetForegroundColour(wxColour(220, 220, 220));

    auto *extruder_label = new wxStaticText(right_placeholder_box, wxID_ANY, m_selected_extruder);
    extruder_label->SetPosition(wxPoint(FromDIP(12), FromDIP(80)));
    extruder_label->SetForegroundColour(wxColour(220, 220, 220));
    extruder_label->SetCursor(wxCursor(wxCURSOR_HAND));
    m_extruder_display_label = extruder_label;

    auto *extruder_value = new wxStaticText(right_placeholder_box, wxID_ANY, "__ / __");
    extruder_value->SetPosition(wxPoint(FromDIP(107), FromDIP(80)));
    extruder_value->SetForegroundColour(wxColour(220, 220, 220));
    extruder_value->SetCursor(wxCursor(wxCURSOR_HAND));
    m_extruder_temp_value = extruder_value;

    auto *extruder_unit = new wxStaticText(right_placeholder_box, wxID_ANY, wxString::FromUTF8("\xC2\xB0""C"));
    extruder_unit->SetForegroundColour(wxColour(220, 220, 220));
    extruder_unit->SetCursor(wxCursor(wxCURSOR_HAND));

    m_extruder_popup_button = extruder_label;
    auto extruder_popup_handler = [this](wxMouseEvent &) { toggle_extruder_popup(); };
    auto extruder_temp_handler = [this](wxMouseEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online())
            return;
        const long current_value = obj->GetExtderSystem() ? static_cast<long>(obj->GetExtderSystem()->GetNozzleTempTarget(0)) : 0;
        wxTextEntryDialog dlg(this, "Nozzle hedef sicakligini girin.", "Nozzle", wxString::Format("%ld", current_value));
        if (dlg.ShowModal() != wxID_OK)
            return;
        long entered_value = 0;
        if (!dlg.GetValue().ToLong(&entered_value))
            return;
        entered_value = std::max<long>(0, std::min<long>(350, entered_value));
        obj->command_set_nozzle_new(0, (int)entered_value);
    };
    extruder_label->Bind(wxEVT_LEFT_DOWN, extruder_popup_handler);
    extruder_value->Bind(wxEVT_LEFT_DOWN, extruder_temp_handler);
    extruder_unit->Bind(wxEVT_LEFT_DOWN, extruder_temp_handler);

    auto *fan_label = new wxStaticText(right_placeholder_box, wxID_ANY, m_selected_fan);
    fan_label->SetPosition(wxPoint(FromDIP(12), FromDIP(140)));
    fan_label->SetForegroundColour(wxColour(220, 220, 220));
    fan_label->SetCursor(wxCursor(wxCURSOR_HAND));
    m_fan_display_label = fan_label;

    auto *fan_value = new wxStaticText(right_placeholder_box, wxID_ANY, m_selected_fan_value);
    fan_value->SetForegroundColour(wxColour(220, 220, 220));
    fan_value->SetCursor(wxCursor(wxCURSOR_HAND));
    m_fan_value_label = fan_value;

    auto *fan_unit = new wxStaticText(right_placeholder_box, wxID_ANY, "%");
    fan_unit->SetForegroundColour(wxColour(220, 220, 220));
    fan_unit->SetCursor(wxCursor(wxCURSOR_HAND));

    const int temp_value_x = FromDIP(107);
    const int temp_unit_x = FromDIP(173);
    const int fan_value_x = FromDIP(165);
    const int fan_unit_x = FromDIP(190);
    const int speed_value_x = FromDIP(180);

    bed_value->SetPosition(wxPoint(temp_value_x, FromDIP(20)));
    bed_unit->SetPosition(wxPoint(temp_unit_x, FromDIP(20)));
    auto bed_temp_handler = [this](wxMouseEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_online())
            return;
        const long current_value = obj->GetBed() ? (long)obj->GetBed()->GetBedTempTarget() : 0;
        wxTextEntryDialog dlg(this, "Bed hedef sicakligini girin.", "Bed", wxString::Format("%ld", current_value));
        if (dlg.ShowModal() != wxID_OK)
            return;
        long entered_value = 0;
        if (!dlg.GetValue().ToLong(&entered_value))
            return;
        entered_value = std::max<long>(0, std::min<long>(140, entered_value));
        obj->command_set_bed((int)entered_value);
    };
    bed_label->SetCursor(wxCursor(wxCURSOR_HAND));
    bed_unit->SetCursor(wxCursor(wxCURSOR_HAND));
    bed_label->Bind(wxEVT_LEFT_DOWN, bed_temp_handler);
    bed_value->Bind(wxEVT_LEFT_DOWN, bed_temp_handler);
    bed_unit->Bind(wxEVT_LEFT_DOWN, bed_temp_handler);

    extruder_value->SetPosition(wxPoint(temp_value_x, FromDIP(80)));
    extruder_unit->SetPosition(wxPoint(temp_unit_x, FromDIP(80)));

    const int fan_row_y = FromDIP(140);
    fan_value->SetPosition(wxPoint(fan_value_x, fan_row_y));
    fan_unit->SetPosition(wxPoint(fan_unit_x, fan_row_y));

    m_fan_popup_button = fan_label;
    auto fan_popup_handler = [this](wxMouseEvent &) { toggle_fan_popup(); };
    auto fan_value_handler = [this](wxMouseEvent &) { prompt_fan_value(); };
    fan_label->Bind(wxEVT_LEFT_DOWN, fan_popup_handler);
    fan_value->Bind(wxEVT_LEFT_DOWN, fan_value_handler);
    fan_unit->Bind(wxEVT_LEFT_DOWN, fan_popup_handler);

    auto *speed_label = new wxStaticText(right_placeholder_box, wxID_ANY, "Speed");
    speed_label->SetPosition(wxPoint(FromDIP(12), FromDIP(200)));
    speed_label->SetForegroundColour(wxColour(220, 220, 220));

    auto *speed_value = new wxStaticText(right_placeholder_box, wxID_ANY, m_selected_speed);
    speed_value->SetForegroundColour(wxColour(220, 220, 220));
    speed_value->SetCursor(wxCursor(wxCURSOR_HAND));
    const int speed_row_y = FromDIP(200);
    speed_value->SetPosition(wxPoint(speed_value_x, speed_row_y));

    auto speed_popup_handler = [this](wxMouseEvent &) { toggle_speed_popup(); };
    speed_label->SetCursor(wxCursor(wxCURSOR_HAND));
    speed_label->Bind(wxEVT_LEFT_DOWN, speed_popup_handler);
    speed_value->Bind(wxEVT_LEFT_DOWN, speed_popup_handler);

    auto *right_placeholder_right_border = new wxPanel(right_placeholder_box, wxID_ANY);
    right_placeholder_right_border->SetSize(wxRect(
        wxPoint(right_placeholder_width - FromDIP(2), FromDIP(1)),
        wxSize(FromDIP(1), right_placeholder_height - FromDIP(2))));
    right_placeholder_right_border->SetMinSize(wxSize(FromDIP(1), right_placeholder_height - FromDIP(2)));
    right_placeholder_right_border->SetMaxSize(wxSize(FromDIP(1), right_placeholder_height - FromDIP(2)));
    right_placeholder_right_border->SetBackgroundColour(wxColour(55, 58, 64));
    right_placeholder_box->Hide();

    right_container->SetSizer(right_sizer);
    right_container->Layout();

    auto *content_columns = new wxBoxSizer(wxHORIZONTAL);

    auto *left_main_column = new wxBoxSizer(wxVERTICAL);
    left_main_column->Add(preview_box, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));
    left_main_column->AddSpacer(FromDIP(8));
    left_main_column->Add(progress_box, 0, wxEXPAND);

    auto *printer_status_box = new StaticBox(left_container, wxID_ANY);
    printer_status_box->SetMinSize(wxSize(FromDIP(775), FromDIP(168)));
    printer_status_box->SetCornerRadius(FromDIP(12));
    printer_status_box->SetBorderWidth(1);
    printer_status_box->SetBorderColorNormal(wxColour(55, 58, 64));
    printer_status_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    printer_status_box->SetBackgroundColour(wxColour(28, 30, 34));

    auto *ps_main_sizer = new wxBoxSizer(wxVERTICAL);

    auto *ps_title_label = new wxStaticText(printer_status_box, wxID_ANY, "Printer Status");
    ps_title_label->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = ps_title_label->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        ps_title_label->SetFont(f);
    }
    auto *ps_title_row = new wxBoxSizer(wxHORIZONTAL);
    ps_title_row->Add(ps_title_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    ps_main_sizer->Add(ps_title_row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));

    auto *ps_title_sep = new wxPanel(printer_status_box, wxID_ANY);
    ps_title_sep->SetMinSize(wxSize(-1, FromDIP(1)));
    ps_title_sep->SetMaxSize(wxSize(-1, FromDIP(1)));
    ps_title_sep->SetBackgroundColour(wxColour(55, 58, 64));
    ps_main_sizer->Add(ps_title_sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    auto *ps_grid = new wxBoxSizer(wxHORIZONTAL);

    const wxColour ps_card_body_bg(35, 38, 43);
    const wxColour ps_card_header_bg(22, 24, 29);

    auto make_ps_card = [this, printer_status_box, ps_card_body_bg](bool active) -> StaticBox * {
        auto *card = new StaticBox(printer_status_box, wxID_ANY);
        card->SetMinSize(wxSize(-1, this->FromDIP(110)));
        card->SetMaxSize(wxSize(-1, this->FromDIP(110)));
        card->SetCornerRadius(this->FromDIP(10));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
        card->SetBackgroundColorNormal(ps_card_body_bg);
        card->SetBackgroundColour(ps_card_body_bg);
        return card;
    };
    auto make_ps_header = [this, ps_card_header_bg](wxWindow *parent, const wxString &txt, bool active) -> wxStaticText * {
        auto *lbl = new wxStaticText(parent, wxID_ANY, txt);
        lbl->SetForegroundColour(active ? wxColour(220, 220, 220) : wxColour(120, 125, 135));
        lbl->SetBackgroundColour(ps_card_header_bg);
        wxFont f = lbl->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        lbl->SetFont(f);
        return lbl;
    };
    auto make_ps_value = [this](wxWindow *parent, const wxString &txt, bool active) -> wxStaticText * {
        auto *lbl = new wxStaticText(parent, wxID_ANY, txt);
        lbl->SetForegroundColour(active ? wxColour(200, 200, 200) : wxColour(90, 95, 105));
        wxFont f = lbl->GetFont();
        if (f.GetPointSize() > 1) f.SetPointSize(f.GetPointSize() - 1);
        lbl->SetFont(f);
        return lbl;
    };
    auto make_icon_row = [this](wxWindow *card, const std::string &icon_name, wxStaticText *lbl) -> wxBoxSizer * {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        wxBitmap bmp = create_scaled_bitmap(icon_name, this, 18);
        wxImage img = bmp.ConvertToImage();
        if (img.IsOk()) {
            for (int x = 0; x < img.GetWidth(); x++)
                for (int y = 0; y < img.GetHeight(); y++)
                    img.SetRGB(x, y, 255, 255, 255);
            bmp = wxBitmap(img);
        }
        row->Add(new wxStaticBitmap(card, wxID_ANY, bmp),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, this->FromDIP(5));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        return row;
    };
    auto add_ps_title_strip = [this, ps_card_header_bg, &make_ps_header](wxBoxSizer *card_sizer, wxWindow *card, const wxString &title, bool active) {
        const double hdr_corner_r = this->FromDIP(10);
        auto *header_panel = new PsCardHeaderPanel(card, ps_card_header_bg, hdr_corner_r);
        auto *header_sz = new wxBoxSizer(wxVERTICAL);
        header_sz->Add(make_ps_header(header_panel, title, active), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(8));
        // Former gap before separator — now inside the dark header so fill runs to the line.
        header_sz->AddSpacer(FromDIP(6));
        header_panel->SetSizer(header_sz);
        card_sizer->Add(header_panel, 0, wxEXPAND);
    };

    // Tool 1–4 cards, then Build Plate
    struct PsToolCol { wxString name; bool active; wxStaticText **temp; wxStaticText **fan; };
    PsToolCol tool_ps_cols[] = {
        { "Tool 1", true,  &m_ps_t1_temp_label, &m_ps_t1_fan_label },
        { "Tool 2", false, &m_ps_t2_temp_label, &m_ps_t2_fan_label },
        { "Tool 3", false, &m_ps_t3_temp_label, &m_ps_t3_fan_label },
        { "Tool 4", false, &m_ps_t4_temp_label, &m_ps_t4_fan_label },
    };
    for (int i = 0; i < 4; ++i) {
        auto &tc = tool_ps_cols[i];
        if (i > 0)
            ps_grid->AddSpacer(FromDIP(20));
        auto *card = make_ps_card(tc.active);
        auto *card_sizer = new wxBoxSizer(wxVERTICAL);
        add_ps_title_strip(card_sizer, card, tc.name, tc.active);
        {
            auto *sep = new wxPanel(card, wxID_ANY);
            sep->SetMinSize(wxSize(-1, FromDIP(1)));
            sep->SetMaxSize(wxSize(-1, FromDIP(1)));
            sep->SetBackgroundColour(wxColour(55, 58, 64));
            card_sizer->Add(sep, 0, wxEXPAND);
        }

        auto *temp_lbl = make_ps_value(card, "-- / --", tc.active);
        *tc.temp = temp_lbl;
        temp_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        const int nozzle_idx = i;
        temp_lbl->Bind(wxEVT_LEFT_DOWN, [this, nozzle_idx](wxMouseEvent &) { prompt_ps_target_temperature(false, nozzle_idx); });
        card_sizer->Add(make_icon_row(card, "tool_temperature_white", temp_lbl), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(8));

        auto *fan_lbl = make_ps_value(card, "--%", tc.active);
        *tc.fan = fan_lbl;
        card_sizer->Add(make_icon_row(card, "tool_fan_white", fan_lbl), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(13));

        card->SetSizer(card_sizer);
        ps_grid->Add(card, 1, wxEXPAND);
    }

    // Build Plate card
    {
        ps_grid->AddSpacer(FromDIP(20));
        auto *card = make_ps_card(false);
        auto *card_sizer = new wxBoxSizer(wxVERTICAL);
        add_ps_title_strip(card_sizer, card, "Build Plate", false);
        {
            auto *sep = new wxPanel(card, wxID_ANY);
            sep->SetMinSize(wxSize(-1, FromDIP(1)));
            sep->SetMaxSize(wxSize(-1, FromDIP(1)));
            sep->SetBackgroundColour(wxColour(55, 58, 64));
            card_sizer->Add(sep, 0, wxEXPAND);
        }

        auto *bed_temp_lbl = make_ps_value(card, "-- / --", false);
        m_ps_bed_temp_label = bed_temp_lbl;
        bed_temp_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        bed_temp_lbl->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { prompt_ps_target_temperature(true, 0); });
        card_sizer->Add(make_icon_row(card, "bed_heating", bed_temp_lbl), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(13));

        card->SetSizer(card_sizer);
        ps_grid->Add(card, 1, wxEXPAND);
    }

    {
        auto *ps_grid_row = new wxBoxSizer(wxHORIZONTAL);
        ps_grid_row->AddSpacer(FromDIP(40));
        ps_grid_row->Add(ps_grid, 1, wxEXPAND);
        ps_grid_row->AddSpacer(FromDIP(40));
        ps_main_sizer->Add(ps_grid_row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));
    }
    printer_status_box->SetSizer(ps_main_sizer);

    auto *right_main_column = new wxBoxSizer(wxVERTICAL);
    right_main_column->Add(right_container, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));
    right_main_column->AddSpacer(FromDIP(3));
    right_main_column->Add(printer_status_box, 0, wxEXPAND);
    right_main_column->AddSpacer(FromDIP(3));
    right_main_column->Add(upper_placeholder_box, 0, wxEXPAND);

    content_columns->Add(left_main_column, 640, wxEXPAND);
    content_columns->AddSpacer(FromDIP(20));
    content_columns->Add(right_main_column, 760, wxEXPAND | wxRIGHT, FromDIP(5));

    left_sizer->Add(content_columns, 0, wxEXPAND);
    left_sizer->AddSpacer(FromDIP(52));

    left_container->SetSizer(left_sizer);

    status_page_sizer->Add(left_container, 1, wxEXPAND | wxRIGHT, FromDIP(20));
    m_status_page->SetSizer(status_page_sizer);

    m_storage_placeholder = new wxPanel(content_host, wxID_ANY);
    m_storage_placeholder->SetBackgroundColour(wxColour(28, 30, 34));
    m_storage_placeholder->Hide();
    m_update_page = create_update_page(content_host);
    m_assistant_page = create_placeholder_page(content_host, "Asistan", "Asistan paneli icin gecici yer tutucu.");

    content_host_sizer->Add(m_status_page, 1, wxEXPAND);
    content_host_sizer->Add(m_storage_placeholder, 1, wxEXPAND);
    content_host_sizer->Add(m_update_page, 1, wxEXPAND);
    content_host_sizer->Add(m_assistant_page, 1, wxEXPAND);
    content_host->SetSizer(content_host_sizer);

    main_sizer->Add(preview_menu_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));
    main_sizer->AddSpacer(FromDIP(20));
    main_sizer->Add(content_host, 1, wxEXPAND);
    SetSizer(main_sizer);
    select_tab(PrinterWebViewTab::Status);

    m_browser = nullptr;
    m_zoomFactor = 100;
    Bind(wxEVT_WEBREQUEST_STATE, &PrinterWebView::on_thumbnail_webrequest_state, this);
    m_layer_refresh_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_layer_info_from_selected_machine(); }, m_layer_refresh_timer->GetId());
    m_layer_refresh_timer->Start(250);
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);
    Bind(wxEVT_SHOW, [this](wxShowEvent &ev) {
        if (ev.IsShown())
            this->ensure_camera_webview_created();
        ev.Skip();
    });
 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    dismiss_printers_popup();
    dismiss_extruder_popup();
    dismiss_fan_popup();
    dismiss_speed_popup();
    if (m_thumbnail_web_request.IsOk())
        m_thumbnail_web_request.Cancel();
    if (m_layer_refresh_timer != nullptr) {
        m_layer_refresh_timer->Stop();
        delete m_layer_refresh_timer;
        m_layer_refresh_timer = nullptr;
    }
    SetEvtHandlerEnabled(false);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    (void) url;
    m_apikey = apikey;
    return;
}

bool PrinterWebView::Show(bool show)
{
    if (show)
        ensure_camera_webview_created();
    if (show)
        refresh_layer_info_from_selected_machine();
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    return;
}

void PrinterWebView::update_mode()
{
    refresh_layer_info_from_selected_machine();
    return;
}

void PrinterWebView::set_printer_layer(int layer)
{
    if (m_layer_printer_value == nullptr)
        return;
    m_layer_printer_value->SetLabelText(layer_value_text(layer));
    Layout();
}

void PrinterWebView::set_file_layer(int layer)
{
    if (m_layer_file_value == nullptr)
        return;
    m_layer_file_value->SetLabelText(layer_value_text(layer));
    Layout();
}

void PrinterWebView::set_layer_info(int printer_layer, int file_layer)
{
    set_printer_layer(printer_layer);
    set_file_layer(file_layer);
}

void PrinterWebView::set_estimated_remaining_seconds(int remaining_seconds)
{
    if (m_estimated_finish_value == nullptr)
        return;
    m_estimated_finish_value->SetLabelText(remaining_minutes_text(remaining_seconds));
    Layout();
}

void PrinterWebView::set_active_file_name(const wxString &file_name)
{
    if (m_active_file_name_value == nullptr)
        return;
    m_active_file_name_value->SetLabelText(file_name.empty() ? "N/A" : file_name);
    Layout();
}

void PrinterWebView::toggle_printers_popup()
{
    if (m_printers_popup == nullptr || m_preview_printers_button == nullptr)
        return;

    rebuild_printers_popup();

    if (m_printers_popup->IsShown()) {
        m_printers_popup->Dismiss();
        return;
    }

    const wxPoint screen_pos = m_preview_printers_button->ClientToScreen(wxPoint(0, m_preview_printers_button->GetSize().GetHeight()));
    m_printers_popup->Position(screen_pos, wxSize(0, 0));
    m_printers_popup->Popup(m_preview_printers_button);
}

void PrinterWebView::dismiss_printers_popup()
{
    if (m_printers_popup != nullptr && m_printers_popup->IsShown())
        m_printers_popup->Dismiss();
}

void PrinterWebView::toggle_extruder_popup()
{
    if (m_extruder_popup == nullptr || m_extruder_popup_button == nullptr)
        return;

    rebuild_extruder_popup();

    if (m_extruder_popup->IsShown()) {
        m_extruder_popup->Dismiss();
        return;
    }

    const wxPoint screen_pos = m_extruder_popup_button->ClientToScreen(wxPoint(0, m_extruder_popup_button->GetSize().GetHeight() + FromDIP(8)));
    m_extruder_popup->Position(screen_pos, wxSize(0, 0));
    m_extruder_popup->Popup(m_extruder_popup_button);
}

void PrinterWebView::dismiss_extruder_popup()
{
    if (m_extruder_popup != nullptr && m_extruder_popup->IsShown())
        m_extruder_popup->Dismiss();
}

void PrinterWebView::toggle_fan_popup()
{
    if (m_fan_popup == nullptr || m_fan_popup_button == nullptr)
        return;

    rebuild_fan_popup();

    if (m_fan_popup->IsShown()) {
        m_fan_popup->Dismiss();
        return;
    }

    const wxPoint screen_pos = m_fan_popup_button->ClientToScreen(wxPoint(0, m_fan_popup_button->GetSize().GetHeight() + FromDIP(8)));
    m_fan_popup->Position(screen_pos, wxSize(0, 0));
    m_fan_popup->Popup(m_fan_popup_button);
}

void PrinterWebView::dismiss_fan_popup()
{
    if (m_fan_popup != nullptr && m_fan_popup->IsShown())
        m_fan_popup->Dismiss();
}

void PrinterWebView::prompt_fan_value()
{
    const long current_value = m_selected_fan_value == "__" ? 0 : wxAtoi(m_selected_fan_value);
    const long entered_value = wxGetNumberFromUser(
        "0 ile 100 arasinda bir fan degeri girin.",
        "%",
        "Fan",
        current_value,
        0,
        100,
        this);

    if (entered_value < 0)
        return;

    m_selected_fan_value = wxString::Format("%ld", entered_value);
    if (m_selected_fan.StartsWith("T"))
        m_fan_values[m_selected_fan] = m_selected_fan_value;
    refresh_fan_value_display();

    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj != nullptr && obj->is_online() && obj->GetFan() != nullptr)
        obj->GetFan()->command_control_fan(1, (int)entered_value);
}

void PrinterWebView::refresh_fan_value_display()
{
    if (m_selected_fan.StartsWith("T")) {
        const auto it = m_fan_values.find(m_selected_fan);
        m_selected_fan_value = it != m_fan_values.end() ? it->second : "__";
    }

    if (m_fan_value_label != nullptr) {
        m_fan_value_label->SetLabelText(m_selected_fan_value);
        m_fan_value_label->SetPosition(wxPoint(FromDIP(165), FromDIP(140)));
    }
    Layout();
}

void PrinterWebView::reset_placeholder_selections()
{
    m_selected_extruder = "Extruder";
    if (m_extruder_display_label != nullptr)
        m_extruder_display_label->SetLabelText(m_selected_extruder);

    m_selected_fan = "Fan";
    if (m_fan_display_label != nullptr)
        m_fan_display_label->SetLabelText(m_selected_fan);

    for (auto &entry : m_fan_values)
        entry.second = "__";
    m_selected_fan_value = "__";
    refresh_fan_value_display();

    m_selected_speed = "--";
    if (m_speed_display_label != nullptr)
        m_speed_display_label->SetLabelText(m_selected_speed);

    dismiss_extruder_popup();
    dismiss_fan_popup();
    dismiss_speed_popup();
    m_selected_filament_tool = 0;
    apply_filament_tool_selection(0);

    Layout();
}

void PrinterWebView::toggle_speed_popup()
{
    if (m_speed_popup == nullptr || m_speed_popup_button == nullptr)
        return;

    rebuild_speed_popup();

    if (m_speed_popup->IsShown()) {
        m_speed_popup->Dismiss();
        return;
    }

    const wxPoint screen_pos = m_speed_popup_button->ClientToScreen(wxPoint(0, m_speed_popup_button->GetSize().GetHeight() + FromDIP(8)));
    m_speed_popup->Position(screen_pos, wxSize(0, 0));
    m_speed_popup->Popup(m_speed_popup_button);
}

void PrinterWebView::dismiss_speed_popup()
{
    if (m_speed_popup != nullptr && m_speed_popup->IsShown())
        m_speed_popup->Dismiss();
}

void PrinterWebView::prompt_ip_connect()
{
    wxTextEntryDialog ip_dialog(this, "Yazicinin IP adresini veya Moonraker adresini girin.", "IP Adresi ile Baglan");
    if (ip_dialog.ShowModal() != wxID_OK)
        return;

    wxString ip_value = ip_dialog.GetValue();
    ip_value.Trim(true);
    ip_value.Trim(false);
    if (ip_value.empty()) {
        wxMessageBox("IP adresi bos olamaz.", "IP Adresi ile Baglan", wxOK | wxICON_WARNING, this);
        return;
    }

    std::string host = into_u8(ip_value);
    const bool has_scheme = host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0;
    const std::string normalized_host = MachineObject::dev_id_from_address(host);
    std::string dev_ip = normalized_host;
    if (!has_scheme && normalized_host.find(':') == std::string::npos)
        dev_ip += ":7125";
    const std::string dev_id = dev_ip;

    wxTextEntryDialog name_dialog(this, "Bu yazici icin gorunecek bir isim belirleyin.", "Yazici Adi", from_u8(dev_ip));
    if (name_dialog.ShowModal() != wxID_OK)
        return;

    wxString name_value = name_dialog.GetValue();
    name_value.Trim(true);
    name_value.Trim(false);
    if (name_value.empty()) {
        wxMessageBox("Yazici adi bos olamaz.", "Yazici Adi", wxOK | wxICON_WARNING, this);
        return;
    }

    auto *dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager == nullptr) {
        wxMessageBox("Device manager hazir degil.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return;
    }

    auto *agent = wxGetApp().getAgent();
    if (agent == nullptr) {
        wxMessageBox("Network agent hazir degil.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return;
    }

    const auto current_printer_agent = agent->get_printer_agent();
    const bool needs_moonraker_agent = current_printer_agent == nullptr ||
        current_printer_agent->get_agent_info().id != "moonraker";
    if (needs_moonraker_agent) {
        auto moonraker_agent = NetworkAgentFactory::create_printer_agent_by_id(
            "moonraker", agent->get_cloud_agent(), Slic3r::data_dir());
        if (moonraker_agent == nullptr) {
            wxMessageBox("Moonraker network agent baslatilamadi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
            return;
        }
        agent->set_printer_agent(moonraker_agent);
    }

    BBLocalMachine machine;
    machine.dev_id = dev_id;
    machine.dev_ip = dev_ip;
    machine.dev_name = into_u8(name_value);
    machine.printer_type = "Moonraker";

    MachineObject *obj = dev_manager->insert_local_device(machine, "lan", "free", "", "");
    if (obj == nullptr) {
        wxMessageBox("Yazici yerel cihaz listesine eklenemedi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return;
    }

    obj->local_use_ssl = host.rfind("https://", 0) == 0;
    if (!dev_manager->set_selected_machine(dev_id)) {
        wxMessageBox("Yazici secilemedi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return;
    }
    m_has_active_printer_connection = true;
    obj->command_request_push_all(true);

    dismiss_printers_popup();
    rebuild_printers_popup();
    refresh_layer_info_from_selected_machine();
    Layout();
}

void PrinterWebView::rebuild_printers_popup()
{
    if (m_printers_popup == nullptr || m_printers_popup_panel == nullptr)
        return;

    if (auto *old_sizer = m_printers_popup_panel->GetSizer()) {
        m_printers_popup_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_printers_popup_panel->DestroyChildren();

    auto *printers_popup_sizer = new wxBoxSizer(wxVERTICAL);
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *selected_machine = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const auto my_machines = dev_manager ? dev_manager->get_my_machine_list() : std::map<std::string, MachineObject*>();
    const auto local_machines = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();
    const bool has_any_machine = selected_machine != nullptr || !my_machines.empty() || !local_machines.empty();

    m_printers_popup_panel->SetMinSize(wxSize(FromDIP(238), -1));

    if (selected_machine != nullptr) {
        auto *printer_current_panel = new wxPanel(m_printers_popup_panel, wxID_ANY);
        printer_current_panel->SetBackgroundColour(wxColour(245, 245, 245));
        auto *printer_current_row = new wxBoxSizer(wxHORIZONTAL);
        printer_current_row->AddSpacer(FromDIP(12));
        printer_current_row->Add(new wxStaticBitmap(printer_current_panel, wxID_ANY, create_scaled_bitmap("printer_preview_BL-P001", this, 18)), 0, wxALIGN_CENTER_VERTICAL);
        printer_current_row->AddSpacer(FromDIP(10));

        auto *current_printer_label = new wxStaticText(printer_current_panel, wxID_ANY, from_u8(selected_machine->get_dev_name()));
        current_printer_label->SetForegroundColour(wxColour(20, 20, 20));
        printer_current_row->Add(current_printer_label, 0, wxALIGN_CENTER_VERTICAL);
        printer_current_row->AddStretchSpacer(1);

        auto *logout_label = new wxStaticText(printer_current_panel, wxID_ANY, wxString::FromUTF8("C\xC4\xB1k\xC4\xB1\xC5\x9F"));
        logout_label->SetForegroundColour(wxColour(110, 110, 110));
        logout_label->SetCursor(wxCursor(wxCURSOR_HAND));
        printer_current_row->Add(logout_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto *logout_icon = new wxStaticBitmap(printer_current_panel, wxID_ANY, create_scaled_bitmap("menu_exit", this, 14));
        logout_icon->SetCursor(wxCursor(wxCURSOR_HAND));
        printer_current_row->Add(logout_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

        auto logout_handler = [this](wxMouseEvent &) {
            dismiss_printers_popup();
            wxGetApp().request_user_logout();
        };
        logout_label->Bind(wxEVT_LEFT_DOWN, logout_handler);
        logout_icon->Bind(wxEVT_LEFT_DOWN, logout_handler);

        printer_current_panel->SetSizer(printer_current_row);
        printers_popup_sizer->Add(printer_current_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));

        auto *header_divider = new wxPanel(m_printers_popup_panel, wxID_ANY);
        header_divider->SetMinSize(wxSize(-1, FromDIP(1)));
        header_divider->SetMaxSize(wxSize(-1, FromDIP(1)));
        header_divider->SetBackgroundColour(wxColour(220, 220, 220));
        printers_popup_sizer->Add(header_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    }

    printers_popup_sizer->AddSpacer(FromDIP(5));

    auto *search_box = new wxTextCtrl(m_printers_popup_panel, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(210), -1));
    search_box->SetHint("Ara");
    search_box->ChangeValue(m_printers_search_query);
    search_box->Bind(wxEVT_TEXT, [this](wxCommandEvent &evt) {
        m_printers_search_query = evt.GetString();
        CallAfter([this]() {
            rebuild_printers_popup();
        });
    });
    printers_popup_sizer->Add(search_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    const wxString search_query = m_printers_search_query.Lower();
    auto matches_search = [&search_query](MachineObject *machine) {
        if (machine == nullptr)
            return false;
        if (search_query.empty())
            return true;

        wxString searchable;
        searchable << from_u8(machine->get_dev_name()) << " "
                   << from_u8(machine->get_dev_id()) << " "
                   << from_u8(machine->get_dev_ip());
        return searchable.Lower().Find(search_query) != wxNOT_FOUND;
    };

    auto add_popup_line = [this, printers_popup_sizer](wxWindow *parent, const wxString &text, const wxColour &color, bool bold = false, int top = 16) {
        auto *line = new wxStaticText(parent, wxID_ANY, text);
        line->SetForegroundColour(color);
        if (bold) {
            wxFont font = line->GetFont();
            font.SetWeight(wxFONTWEIGHT_BOLD);
            line->SetFont(font);
        }
        printers_popup_sizer->Add(line, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(top));
        return line;
    };

    auto add_ip_connect_line = [&]() {
        auto *line = add_popup_line(m_printers_popup_panel, "+ IP Adresi ile Baglan", wxColour(20, 20, 20), true, 14);
        line->SetCursor(wxCursor(wxCURSOR_HAND));
        line->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { prompt_ip_connect(); });
    };

    auto bind_machine_line = [this, dev_manager](wxStaticText *line, MachineObject *machine) {
        if (line == nullptr || machine == nullptr)
            return;
        line->SetCursor(wxCursor(wxCURSOR_HAND));
        line->Bind(wxEVT_LEFT_DOWN, [this, dev_manager, machine](wxMouseEvent &) {
            const std::string dev_id = machine->get_dev_id();
            dismiss_printers_popup();
            if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->m_monitor != nullptr)
                wxGetApp().mainframe->m_monitor->select_machine(dev_id);
            else if (dev_manager != nullptr)
                dev_manager->set_selected_machine(dev_id);
            m_has_active_printer_connection = true;
            machine->command_request_push_all(true);
            refresh_layer_info_from_selected_machine();
        });
    };

    auto remove_local_machine = [this, dev_manager](MachineObject *machine) {
        if (dev_manager == nullptr || machine == nullptr)
            return;

        const std::string dev_id = machine->get_dev_id();
        const wxString    name   = from_u8(machine->get_dev_name());
        const int answer = wxMessageBox(
            wxString::Format("%s kayitli yazicisini silmek istiyor musunuz?", name),
            "Yaziciyi Sil",
            wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
            this);
        if (answer != wxYES)
            return;

        MachineObject *selected_machine = dev_manager->get_selected_machine();
        if (selected_machine != nullptr && selected_machine->get_dev_id() == dev_id) {
            selected_machine->disconnect();
            selected_machine->set_online_state(false);
            selected_machine->reset();
            m_has_active_printer_connection = false;
            dev_manager->set_selected_machine("");
        }

        if (wxGetApp().app_config != nullptr)
            wxGetApp().app_config->erase_local_machine(dev_id);
        dev_manager->erase_local_machine(dev_id);
        delete machine;

        rebuild_printers_popup();
        refresh_layer_info_from_selected_machine();
        Layout();
    };

    auto add_machine_line = [this, printers_popup_sizer, bind_machine_line, remove_local_machine, &local_machines](
                                MachineObject *machine, const wxColour &color, int top = 12) {
        if (machine == nullptr)
            return;

        auto *row = new wxPanel(m_printers_popup_panel, wxID_ANY);
        row->SetBackgroundColour(wxColour(245, 245, 245));
        auto *row_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *name_label = new wxStaticText(row, wxID_ANY, from_u8(machine->get_dev_name()), wxDefaultPosition, wxSize(FromDIP(155), -1), wxST_ELLIPSIZE_END);
        name_label->SetForegroundColour(color);
        row_sizer->Add(name_label, 1, wxALIGN_CENTER_VERTICAL);

        const bool is_saved_local_machine = local_machines.find(machine->get_dev_id()) != local_machines.end();
        if (is_saved_local_machine) {
            auto *delete_label = new wxStaticText(row, wxID_ANY, "Sil");
            delete_label->SetForegroundColour(wxColour(180, 60, 60));
            delete_label->SetCursor(wxCursor(wxCURSOR_HAND));
            delete_label->Bind(wxEVT_LEFT_DOWN, [remove_local_machine, machine](wxMouseEvent &) { remove_local_machine(machine); });
            row_sizer->AddSpacer(FromDIP(8));
            row_sizer->Add(delete_label, 0, wxALIGN_CENTER_VERTICAL);
        }

        row->SetSizer(row_sizer);
        bind_machine_line(name_label, machine);
        printers_popup_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(top));
    };

    std::map<std::string, MachineObject*> visible_my_machines;
    for (const auto &entry : my_machines) {
        if (entry.second == nullptr)
            continue;
        if (selected_machine != nullptr && entry.first == selected_machine->get_dev_id())
            continue;
        if (!matches_search(entry.second))
            continue;
        visible_my_machines.emplace(entry);
    }

    std::map<std::string, MachineObject*> other_local_machines;
    for (const auto &entry : local_machines) {
        if (entry.second == nullptr)
            continue;
        if (visible_my_machines.find(entry.first) != visible_my_machines.end())
            continue;
        if (selected_machine != nullptr && entry.first == selected_machine->get_dev_id())
            continue;
        if (!matches_search(entry.second))
            continue;
        other_local_machines.emplace(entry);
    }

    if (!has_any_machine) {
        add_popup_line(m_printers_popup_panel, "Cihaz ekle +", wxColour(20, 20, 20), true, 18);
        add_ip_connect_line();
    } else {
        if (!visible_my_machines.empty()) {
            add_popup_line(m_printers_popup_panel, "Cihazim", wxColour(120, 120, 120), false, 18);
            for (const auto &entry : visible_my_machines) {
                auto *machine = entry.second;
                if (machine == nullptr)
                    continue;
                add_machine_line(machine, wxColour(20, 20, 20), 12);
            }
        }

        if (!other_local_machines.empty()) {
            add_popup_line(m_printers_popup_panel, "Diger Cihazlar", wxColour(120, 120, 120), false, 18);
            for (const auto &entry : other_local_machines) {
                auto *machine = entry.second;
                if (machine == nullptr)
                    continue;
                add_machine_line(machine, wxColour(80, 80, 80), 12);
            }
        }

        if (!m_printers_search_query.empty() && visible_my_machines.empty() && other_local_machines.empty())
            add_popup_line(m_printers_popup_panel, "Sonuc bulunamadi", wxColour(120, 120, 120), false, 14);

        auto *ip_line = add_popup_line(m_printers_popup_panel, "+ IP Adresi ile Baglan", wxColour(20, 20, 20), true, 16);
        ip_line->SetCursor(wxCursor(wxCURSOR_HAND));
        ip_line->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { prompt_ip_connect(); });
    }

    add_popup_line(m_printers_popup_panel, "Cihazlarimi bulamiyor musunuz?", wxColour(38, 94, 190), false, 16);
    printers_popup_sizer->AddSpacer(FromDIP(10));

    m_printers_popup_panel->SetSizer(printers_popup_sizer);
    printers_popup_sizer->Fit(m_printers_popup_panel);
    m_printers_popup_panel->Layout();

    const wxSize popup_size = m_printers_popup_panel->GetBestSize();
    m_printers_popup_panel->SetSize(popup_size);
    m_printers_popup->SetClientSize(popup_size);
    m_printers_popup->SetSize(popup_size);
    m_printers_popup->Layout();
    if (!m_printers_search_query.empty()) {
        search_box->SetFocus();
        search_box->SetInsertionPointEnd();
    }
}

void PrinterWebView::rebuild_extruder_popup()
{
    if (m_extruder_popup == nullptr || m_extruder_popup_panel == nullptr)
        return;

    if (auto *old_sizer = m_extruder_popup_panel->GetSizer()) {
        m_extruder_popup_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_extruder_popup_panel->DestroyChildren();

    const int popup_width = FromDIP(95);
    const int popup_height = FromDIP(180);

    m_extruder_popup_panel->SetMinSize(wxSize(popup_width, popup_height));
    m_extruder_popup_panel->SetMaxSize(wxSize(popup_width, popup_height));
    m_extruder_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));

    auto *popup_sizer = new wxBoxSizer(wxVERTICAL);
    auto *popup_box = new StaticBox(m_extruder_popup_panel, wxID_ANY);
    popup_box->SetMinSize(wxSize(popup_width, popup_height));
    popup_box->SetMaxSize(wxSize(popup_width, popup_height));
    popup_box->SetCornerRadius(FromDIP(12));
    popup_box->SetBorderWidth(1);
    popup_box->SetBorderColorNormal(wxColour(55, 58, 64));
    popup_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    popup_box->SetBackgroundColour(wxColour(22, 24, 29));

    auto add_popup_line = [this, popup_box, popup_width](int top_offset) {
        auto *line = new wxPanel(popup_box, wxID_ANY);
        line->SetSize(wxRect(
            wxPoint(FromDIP(0), FromDIP(top_offset)),
            wxSize(popup_width, FromDIP(1))));
        line->SetMinSize(wxSize(popup_width, FromDIP(1)));
        line->SetMaxSize(wxSize(popup_width, FromDIP(1)));
        line->SetBackgroundColour(wxColour(55, 58, 64));
    };

    add_popup_line(45);
    add_popup_line(90);
    add_popup_line(135);

    const std::array<wxString, 1> extruder_labels = { "T1" };
    for (int i = 0; i < (int)extruder_labels.size(); ++i) {
        auto *label = new wxStaticText(popup_box, wxID_ANY, extruder_labels[i]);
        label->SetForegroundColour(wxColour(220, 220, 220));
        label->SetCursor(wxCursor(wxCURSOR_HAND));
        const wxSize label_size = label->GetBestSize();
        const int row_height = FromDIP(45);
        const int label_x = (popup_width - label_size.GetWidth()) / 2;
        const int label_y = i * row_height + (row_height - label_size.GetHeight()) / 2;
        label->SetPosition(wxPoint(label_x, label_y));
        label->Bind(wxEVT_LEFT_DOWN, [this, choice = extruder_labels[i]](wxMouseEvent &) {
            m_selected_extruder = choice;
            if (m_extruder_display_label != nullptr)
                m_extruder_display_label->SetLabelText(m_selected_extruder);
            dismiss_extruder_popup();
            Layout();
        });
    }

    popup_sizer->Add(popup_box, 0, wxEXPAND);
    m_extruder_popup_panel->SetSizer(popup_sizer);
    popup_sizer->Fit(m_extruder_popup_panel);
    m_extruder_popup_panel->Layout();

    const wxSize popup_size = m_extruder_popup_panel->GetBestSize();
    m_extruder_popup_panel->SetSize(popup_size);
    m_extruder_popup->SetClientSize(popup_size);
    m_extruder_popup->SetSize(popup_size);
    m_extruder_popup->Layout();
}

void PrinterWebView::rebuild_fan_popup()
{
    if (m_fan_popup == nullptr || m_fan_popup_panel == nullptr)
        return;

    if (auto *old_sizer = m_fan_popup_panel->GetSizer()) {
        m_fan_popup_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_fan_popup_panel->DestroyChildren();

    const int popup_width = FromDIP(95);
    const int popup_height = FromDIP(180);

    m_fan_popup_panel->SetMinSize(wxSize(popup_width, popup_height));
    m_fan_popup_panel->SetMaxSize(wxSize(popup_width, popup_height));
    m_fan_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));

    auto *popup_sizer = new wxBoxSizer(wxVERTICAL);
    auto *popup_box = new StaticBox(m_fan_popup_panel, wxID_ANY);
    popup_box->SetMinSize(wxSize(popup_width, popup_height));
    popup_box->SetMaxSize(wxSize(popup_width, popup_height));
    popup_box->SetCornerRadius(FromDIP(12));
    popup_box->SetBorderWidth(1);
    popup_box->SetBorderColorNormal(wxColour(55, 58, 64));
    popup_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    popup_box->SetBackgroundColour(wxColour(22, 24, 29));

    auto add_popup_line = [this, popup_box, popup_width](int top_offset) {
        auto *line = new wxPanel(popup_box, wxID_ANY);
        line->SetSize(wxRect(
            wxPoint(FromDIP(0), FromDIP(top_offset)),
            wxSize(popup_width, FromDIP(1))));
        line->SetMinSize(wxSize(popup_width, FromDIP(1)));
        line->SetMaxSize(wxSize(popup_width, FromDIP(1)));
        line->SetBackgroundColour(wxColour(55, 58, 64));
    };

    add_popup_line(45);
    add_popup_line(90);
    add_popup_line(135);

    const std::array<wxString, 1> fan_labels = { "T1" };
    for (int i = 0; i < (int)fan_labels.size(); ++i) {
        auto *label = new wxStaticText(popup_box, wxID_ANY, fan_labels[i]);
        label->SetForegroundColour(wxColour(220, 220, 220));
        label->SetCursor(wxCursor(wxCURSOR_HAND));
        const wxSize label_size = label->GetBestSize();
        const int row_height = FromDIP(45);
        const int label_x = (popup_width - label_size.GetWidth()) / 2;
        const int label_y = i * row_height + (row_height - label_size.GetHeight()) / 2;
        label->SetPosition(wxPoint(label_x, label_y));
        label->Bind(wxEVT_LEFT_DOWN, [this, choice = fan_labels[i]](wxMouseEvent &) {
            m_selected_fan = choice;
            if (m_fan_display_label != nullptr)
                m_fan_display_label->SetLabelText(m_selected_fan);
            refresh_fan_value_display();
            dismiss_fan_popup();
        });
    }

    popup_sizer->Add(popup_box, 0, wxEXPAND);
    m_fan_popup_panel->SetSizer(popup_sizer);
    popup_sizer->Fit(m_fan_popup_panel);
    m_fan_popup_panel->Layout();

    const wxSize popup_size = m_fan_popup_panel->GetBestSize();
    m_fan_popup_panel->SetSize(popup_size);
    m_fan_popup->SetClientSize(popup_size);
    m_fan_popup->SetSize(popup_size);
    m_fan_popup->Layout();
}

void PrinterWebView::rebuild_speed_popup()
{
    if (m_speed_popup == nullptr || m_speed_popup_panel == nullptr)
        return;

    if (auto *old_sizer = m_speed_popup_panel->GetSizer()) {
        m_speed_popup_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_speed_popup_panel->DestroyChildren();

    const int popup_width = FromDIP(120);
    const int popup_height = FromDIP(180);

    m_speed_popup_panel->SetMinSize(wxSize(popup_width, popup_height));
    m_speed_popup_panel->SetMaxSize(wxSize(popup_width, popup_height));
    m_speed_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));

    auto *popup_sizer = new wxBoxSizer(wxVERTICAL);
    auto *popup_box = new StaticBox(m_speed_popup_panel, wxID_ANY);
    popup_box->SetMinSize(wxSize(popup_width, popup_height));
    popup_box->SetMaxSize(wxSize(popup_width, popup_height));
    popup_box->SetCornerRadius(FromDIP(12));
    popup_box->SetBorderWidth(1);
    popup_box->SetBorderColorNormal(wxColour(55, 58, 64));
    popup_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    popup_box->SetBackgroundColour(wxColour(22, 24, 29));

    auto add_popup_line = [this, popup_box, popup_width](int top_offset) {
        auto *line = new wxPanel(popup_box, wxID_ANY);
        line->SetSize(wxRect(
            wxPoint(FromDIP(0), FromDIP(top_offset)),
            wxSize(popup_width, FromDIP(1))));
        line->SetMinSize(wxSize(popup_width, FromDIP(1)));
        line->SetMaxSize(wxSize(popup_width, FromDIP(1)));
        line->SetBackgroundColour(wxColour(55, 58, 64));
    };

    add_popup_line(45);
    add_popup_line(90);
    add_popup_line(135);

    const std::array<wxString, 4> speed_labels = { "slow", "normal", "fast", "very fast" };
    for (int i = 0; i < 4; ++i) {
        auto *label = new wxStaticText(popup_box, wxID_ANY, speed_labels[i]);
        label->SetForegroundColour(wxColour(220, 220, 220));
        label->SetCursor(wxCursor(wxCURSOR_HAND));
        const wxSize label_size = label->GetBestSize();
        const int row_height = FromDIP(45);
        const int label_x = (popup_width - label_size.GetWidth()) / 2;
        const int label_y = i * row_height + (row_height - label_size.GetHeight()) / 2;
        label->SetPosition(wxPoint(label_x, label_y));
        label->Bind(wxEVT_LEFT_DOWN, [this, choice = speed_labels[i], speed_index = i](wxMouseEvent &) {
            m_selected_speed = choice;
            if (m_speed_display_label != nullptr)
                m_speed_display_label->SetLabelText(m_selected_speed);
            auto *dev_manager = wxGetApp().getDeviceManager();
            MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
            if (obj != nullptr && obj->is_online()) {
                const std::array<DevPrintingSpeedLevel, 4> speed_values = {
                    SPEED_LEVEL_SILENCE,
                    SPEED_LEVEL_NORMAL,
                    SPEED_LEVEL_RAPID,
                    SPEED_LEVEL_RAMPAGE
                };
                obj->command_set_printing_speed(speed_values[speed_index]);
            }
            dismiss_speed_popup();
            Layout();
        });
    }

    popup_sizer->Add(popup_box, 0, wxEXPAND);
    m_speed_popup_panel->SetSizer(popup_sizer);
    popup_sizer->Fit(m_speed_popup_panel);
    m_speed_popup_panel->Layout();

    const wxSize popup_size = m_speed_popup_panel->GetBestSize();
    m_speed_popup_panel->SetSize(popup_size);
    m_speed_popup->SetClientSize(popup_size);
    m_speed_popup->SetSize(popup_size);
    m_speed_popup->Layout();
}

void PrinterWebView::ensure_camera_webview_created()
{
    if (m_camera_webview_initialized || m_camera_webview_host == nullptr)
        return;
    m_camera_webview_initialized = true;

    wxWebView *const wv = ::WebView::CreateWebView(m_camera_webview_host, wxString{});
    if (wv == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "PrinterWebView: WebView::CreateWebView returned null; camera area disabled";
        auto *msg = new wxStaticText(
            m_camera_webview_host,
            wxID_ANY,
            _L("Camera preview could not start. Install or repair Microsoft WebView2 Runtime."));
        auto *vs = new wxBoxSizer(wxVERTICAL);
        vs->AddStretchSpacer(1);
        vs->Add(msg, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(24));
        vs->AddStretchSpacer(1);
        m_camera_webview_host->SetSizer(vs);
        m_camera_webview_host->Layout();
        return;
    }

    m_camera_webview = wv;
    m_camera_webview->SetBackgroundColour(*wxBLACK);
    m_camera_webview->SetPage(camera_stream_page({}), "");

    auto *hs = new wxBoxSizer(wxHORIZONTAL);
    hs->Add(m_camera_webview, 1, wxEXPAND);
    m_camera_webview_host->SetSizer(hs);

#ifdef __WXMSW__
    m_camera_webview->Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
        e.Skip();
        if (m_camera_webview == nullptr)
            return;
        if (m_camera_webview->GetNativeBackend() == nullptr)
            return;
        wxSize sz = e.GetSize();
        if (sz.x <= 0 || sz.y <= 0)
            return;
        HWND hwnd = (HWND) m_camera_webview->GetHWND();
        if (hwnd == nullptr)
            return;
        const int d = this->FromDIP(12) * 2;
        HRGN hrgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, d, d);
        if (hrgn == nullptr)
            return;
        if (!::SetWindowRgn(hwnd, hrgn, TRUE))
            ::DeleteObject(hrgn);
    });
#endif

    m_camera_webview_host->Layout();
    // Do not call refresh_layer_info_from_selected_machine() here: it can re-enter device/network
    // paths while WebView2 is still attaching and has been linked to startup crashes.
    CallAfter([this]() {
        if (m_camera_webview != nullptr)
            refresh_layer_info_from_selected_machine();
    });
}

void PrinterWebView::apply_filament_tool_selection(int tool_index)
{
    if (tool_index < 0 || tool_index > 3)
        tool_index = 0;
    m_selected_filament_tool = tool_index;

    static const wxColour k_filament_tool_colors[4] = {
        wxColour(214, 181, 46),
        wxColour(57, 145, 212),
        wxColour(217, 101, 43),
        wxColour(164, 207, 42),
    };
    const wxColour c = k_filament_tool_colors[tool_index];

    if (m_filament_tool_color_dot != nullptr) {
        m_filament_tool_color_dot->SetBackgroundColorNormal(c);
        m_filament_tool_color_dot->SetBackgroundColour(c);
        m_filament_tool_color_dot->Refresh();
    }
    if (m_filament_tool_name_lbl != nullptr)
        m_filament_tool_name_lbl->SetLabelText(wxString::Format("Tool %d", tool_index + 1));
}

void PrinterWebView::prompt_ps_target_temperature(bool is_bed, int extruder_index)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online())
        return;

    extruder_index = std::max(0, std::min(3, extruder_index));

    long min_t = 0;
    long max_t = 300;
    long cur = 0;
    wxString caption;
    wxString msg;

    if (is_bed) {
        caption = _L("Tabla hedef sicakligi");
        msg = _L("Yeni tabla hedef sicakligini girin (°C).");
        if (obj->GetBed() != nullptr)
            cur = static_cast<long>(std::nearbyint(static_cast<double>(obj->GetBed()->GetBedTempTarget())));
        max_t = obj->get_bed_temperature_limit();
        if (obj->bed_temp_range.size() >= 2) {
            min_t = obj->bed_temp_range[0];
            max_t = obj->bed_temp_range[1];
        }
    } else {
        caption = wxString::Format(_L("Nozul %d hedef sicakligi"), extruder_index + 1);
        msg = _L("Yeni nozul hedef sicakligini girin (°C).");
        if (obj->GetExtderSystem() != nullptr)
            cur = static_cast<long>(std::nearbyint(static_cast<double>(obj->GetExtderSystem()->GetNozzleTempTarget(extruder_index))));
        if (obj->nozzle_temp_range.size() >= 2) {
            min_t = obj->nozzle_temp_range[0];
            max_t = obj->nozzle_temp_range[1];
        }
    }

    wxTextEntryDialog dlg(this, msg, caption, wxString::Format("%ld", cur));
    if (dlg.ShowModal() != wxID_OK)
        return;
    long v = 0;
    if (!dlg.GetValue().ToLong(&v))
        return;
    v = std::max(min_t, std::min(max_t, v));
    if (is_bed)
        obj->command_set_bed(static_cast<int>(v));
    else
        obj->command_set_nozzle_new(extruder_index, static_cast<int>(v));
}

void PrinterWebView::show_filament_load_wizard()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->is_in_printing())
        return;

    wxDialog wizard(this, wxID_ANY, _L("Filament yukleme"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wizard.SetBackgroundColour(wxColour(40, 42, 48));

    auto *outer = new wxBoxSizer(wxVERTICAL);
    auto *msg = new wxStaticText(&wizard, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    msg->SetForegroundColour(wxColour(220, 220, 220));
    msg->Wrap(FromDIP(380));

    int step = 0;
    const auto refresh_text = [&]() {
        if (step == 0)
            msg->SetLabel(_L("1/3: Nozul isiniyor. Hedef sicakliga ulastiginda Ileri'ye basin."));
        else if (step == 1)
            msg->SetLabel(_L("2/3: Nozul hazirsa filament yukleme komutunu gondermek icin Ileri'ye basin."));
        else
            msg->SetLabel(_L("3/3: Filament uctan duzgun ciktiysa Tamam ile kapatin."));
    };
    refresh_text();

    outer->Add(msg, 0, wxEXPAND | wxALL, FromDIP(16));

    auto *btn_row = new wxBoxSizer(wxHORIZONTAL);
    auto *btn_next = new wxButton(&wizard, wxID_ANY, _L("Ileri"));
    auto *btn_cancel = new wxButton(&wizard, wxID_ANY, _L("Iptal"));
    btn_row->AddStretchSpacer(1);
    btn_row->Add(btn_next, 0, wxRIGHT, FromDIP(8));
    btn_row->Add(btn_cancel, 0);
    outer->Add(btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    wizard.SetSizer(outer);
    wizard.Fit();

    btn_next->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        if (step == 0) {
            step = 1;
            refresh_text();
        } else if (step == 1) {
            const std::string slot = std::to_string(m_selected_filament_tool);
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: load wizard send command slot=" << slot;
            obj->command_ams_change_filament(true, "0", slot);
            step = 2;
            refresh_text();
            btn_next->SetLabel(_L("Tamam"));
        } else {
            wizard.EndModal(wxID_OK);
        }
    });
    btn_cancel->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { wizard.EndModal(wxID_CANCEL); });

    wizard.ShowModal();
}

void PrinterWebView::ensure_storage_page_created()
{
    if (m_storage_page != nullptr || m_storage_placeholder == nullptr)
        return;

    wxSizer *const sizer = m_storage_placeholder->GetContainingSizer();
    wxWindow *const parent = m_storage_placeholder->GetParent();
    if (sizer == nullptr || parent == nullptr)
        return;

    m_storage_page = new CloudTaskManagerPage(parent);
    m_storage_page->Hide();

    if (!sizer->Replace(m_storage_placeholder, m_storage_page, false)) {
        BOOST_LOG_TRIVIAL(error) << "PrinterWebView: ensure_storage_page_created Replace failed";
        delete m_storage_page;
        m_storage_page = nullptr;
        return;
    }

    m_storage_placeholder->Destroy();
    m_storage_placeholder = nullptr;
    parent->Layout();
}

void PrinterWebView::select_tab(PrinterWebViewTab tab)
{
    m_selected_tab = tab;

    if (tab == PrinterWebViewTab::Storage)
        ensure_storage_page_created();

    if (m_status_page != nullptr)
        m_status_page->Show(tab == PrinterWebViewTab::Status);
    if (m_storage_page != nullptr)
        m_storage_page->Show(tab == PrinterWebViewTab::Storage);
    else if (m_storage_placeholder != nullptr)
        m_storage_placeholder->Show(tab == PrinterWebViewTab::Storage);
    if (m_update_page != nullptr)
        m_update_page->Show(tab == PrinterWebViewTab::Update);
    if (m_assistant_page != nullptr)
        m_assistant_page->Show(tab == PrinterWebViewTab::Assistant);

    if (tab == PrinterWebViewTab::Storage && m_storage_page != nullptr) {
        m_storage_page->refresh_user_device();
        m_storage_page->update_page();
    }

    update_sidebar_selection();
    Layout();
    Refresh();
}

void PrinterWebView::update_sidebar_selection()
{
    for (auto &item : m_sidebar_items) {
        const bool selected = item.tab == m_selected_tab;
        if (item.panel != nullptr)
            item.panel->SetBackgroundColour(selected ? wxColour(47, 54, 44) : wxColour(28, 30, 34));
        if (item.active_strip != nullptr)
            item.active_strip->SetBackgroundColour(selected ? wxColour(47, 181, 90) : wxColour(28, 30, 34));
        if (item.label != nullptr)
            item.label->SetForegroundColour(selected ? wxColour(245, 245, 245) : wxColour(235, 235, 235));
        if (item.chevron != nullptr)
            item.chevron->SetForegroundColour(selected ? wxColour(210, 210, 210) : wxColour(130, 130, 130));
    }
}

wxPanel *PrinterWebView::create_placeholder_page(wxWindow *parent, const wxString &title, const wxString &description)
{
    auto *page = new wxPanel(parent, wxID_ANY);
    page->SetBackgroundColour(wxColour(28, 30, 34));

    auto *page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->AddStretchSpacer(1);

    auto *card = new StaticBox(page, wxID_ANY);
    card->SetCornerRadius(FromDIP(10));
    card->SetBorderWidth(1);
    card->SetBorderColorNormal(wxColour(55, 58, 64));
    card->SetBackgroundColorNormal(wxColour(22, 24, 29));
    card->SetBackgroundColour(wxColour(28, 30, 34));
    card->SetMinSize(wxSize(FromDIP(520), FromDIP(220)));

    auto *card_sizer = new wxBoxSizer(wxVERTICAL);
    card_sizer->AddStretchSpacer(1);

    auto *title_label = new wxStaticText(card, wxID_ANY, title);
    title_label->SetForegroundColour(wxColour(235, 235, 235));
    wxFont title_font = title_label->GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 4);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_label->SetFont(title_font);
    card_sizer->Add(title_label, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(12));

    auto *description_label = new wxStaticText(card, wxID_ANY, description);
    description_label->SetForegroundColour(wxColour(150, 156, 166));
    card_sizer->Add(description_label, 0, wxALIGN_CENTER_HORIZONTAL);

    card_sizer->AddStretchSpacer(1);
    card->SetSizer(card_sizer);

    page_sizer->Add(card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(24));
    page_sizer->AddStretchSpacer(1);
    page->SetSizer(page_sizer);
    return page;
}

wxPanel *PrinterWebView::create_update_page(wxWindow *parent)
{
    auto *page = new wxPanel(parent, wxID_ANY);
    page->SetBackgroundColour(wxColour(28, 30, 34));

    auto *page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->AddSpacer(FromDIP(40));

    auto *card = new StaticBox(page, wxID_ANY);
    card->SetCornerRadius(FromDIP(8));
    card->SetBorderWidth(1);
    card->SetBorderColorNormal(wxColour(72, 78, 86));
    card->SetBackgroundColorNormal(wxColour(247, 247, 247));
    card->SetBackgroundColour(wxColour(28, 30, 34));
    card->SetMinSize(wxSize(FromDIP(920), FromDIP(245)));

    auto *card_sizer = new wxBoxSizer(wxVERTICAL);

    auto *header = new wxPanel(card, wxID_ANY);
    header->SetBackgroundColour(wxColour(239, 239, 239));
    header->SetMinSize(wxSize(-1, FromDIP(34)));
    auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_update_header_title = new wxStaticText(header, wxID_ANY, "P1P(Bosta)");
    m_update_header_title->SetForegroundColour(wxColour(40, 40, 40));
    header_sizer->Add(m_update_header_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(18));
    header->SetSizer(header_sizer);
    card_sizer->Add(header, 0, wxEXPAND);

    auto *body = new wxPanel(card, wxID_ANY);
    body->SetBackgroundColour(wxColour(252, 252, 252));
    auto *body_sizer = new wxBoxSizer(wxHORIZONTAL);
    body_sizer->AddSpacer(FromDIP(28));

    m_update_printer_bitmap = new wxStaticBitmap(body, wxID_ANY, create_scaled_bitmap("printer_thumbnail", this, 150));
    m_update_printer_bitmap->SetMinSize(wxSize(FromDIP(170), FromDIP(170)));
    body_sizer->Add(m_update_printer_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(34));

    auto *info_col = new wxBoxSizer(wxVERTICAL);
    info_col->AddStretchSpacer(1);

    auto make_info_row = [body, this](const wxString &label_text, wxStaticText **value_out) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        auto *label = new wxStaticText(body, wxID_ANY, label_text);
        wxFont label_font = label->GetFont();
        label_font.SetWeight(wxFONTWEIGHT_BOLD);
        label->SetFont(label_font);
        label->SetForegroundColour(wxColour(18, 18, 18));
        row->Add(label, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

        auto *value = new wxStaticText(body, wxID_ANY, "-");
        value->SetForegroundColour(wxColour(18, 18, 18));
        row->Add(value, 0, wxALIGN_CENTER_VERTICAL);
        *value_out = value;
        return row;
    };

    info_col->Add(make_info_row("Model:", &m_update_model_value), 0, wxBOTTOM, FromDIP(12));
    info_col->Add(make_info_row("Seri:", &m_update_serial_value), 0, wxBOTTOM, FromDIP(12));
    info_col->Add(make_info_row("Surum:", &m_update_version_value), 0, wxBOTTOM, FromDIP(12));
    info_col->AddStretchSpacer(1);
    body_sizer->Add(info_col, 0, wxEXPAND | wxRIGHT, FromDIP(30));

    body_sizer->AddStretchSpacer(1);

    auto *right_col = new wxBoxSizer(wxVERTICAL);
    right_col->AddSpacer(FromDIP(18));

    auto *update_button = new Button(body, "Urun yazilimini guncelle");
    update_button->SetMinSize(wxSize(FromDIP(138), FromDIP(30)));
    update_button->SetCornerRadius(FromDIP(15));
    update_button->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal)));
    update_button->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour("#C5CCD3"), StateColor::Normal)));
    update_button->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour("#7C848C"), StateColor::Normal)));
    update_button->Disable();
    right_col->Add(update_button, 0, wxALIGN_RIGHT | wxBOTTOM, FromDIP(14));

    m_update_status_value = new wxStaticText(body, wxID_ANY, "Guncelleme bilgisi bekleniyor");
    m_update_status_value->SetForegroundColour(wxColour(0, 166, 76));
    right_col->Add(m_update_status_value, 0, wxALIGN_RIGHT | wxBOTTOM, FromDIP(12));

    auto *progress_row = new wxBoxSizer(wxHORIZONTAL);
    m_update_progress_gauge = new wxGauge(body, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(88), FromDIP(12)), wxGA_SMOOTH);
    m_update_progress_gauge->SetValue(0);
    progress_row->Add(m_update_progress_gauge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    m_update_percent_value = new wxStaticText(body, wxID_ANY, "0%");
    m_update_percent_value->SetForegroundColour(wxColour(0, 166, 76));
    progress_row->Add(m_update_percent_value, 0, wxALIGN_CENTER_VERTICAL);
    right_col->Add(progress_row, 0, wxALIGN_RIGHT | wxBOTTOM, FromDIP(12));

    m_update_release_note_link = new wxStaticText(body, wxID_ANY, "Surum notu");
    m_update_release_note_link->SetForegroundColour(wxColour(36, 125, 255));
    right_col->Add(m_update_release_note_link, 0, wxALIGN_RIGHT);

    right_col->AddStretchSpacer(1);
    body_sizer->Add(right_col, 0, wxEXPAND | wxRIGHT, FromDIP(24));
    body->SetSizer(body_sizer);

    card_sizer->Add(body, 1, wxEXPAND);
    card->SetSizer(card_sizer);

    page_sizer->Add(card, 0, wxLEFT | wxRIGHT, FromDIP(24));
    page_sizer->AddStretchSpacer(1);
    page->SetSizer(page_sizer);
    return page;
}

void PrinterWebView::set_fallback_preview_thumbnail()
{
    if (m_preview_thumbnail == nullptr)
        return;

    m_preview_thumbnail_url.clear();
    if (auto *plater = wxGetApp().plater()) {
        PartPlate *plate = plater->get_partplate_list().get_curr_plate();
        if (plate != nullptr) {
            wxImage plate_image = image_from_thumbnail_data(plate->thumbnail_data);
            if (plate_image.IsOk()) {
                m_preview_thumbnail->SetBitmap(wxBitmap(scale_preview_thumbnail(plate_image, FromDIP(120), FromDIP(120))));
                Layout();
                return;
            }
        }
    }

    const wxString logo_path = from_u8(Slic3r::resources_dir() + "/images/logo.jpg");
    wxImage logo_image;
    if (logo_image.LoadFile(logo_path, wxBITMAP_TYPE_JPEG)) {
        wxImage resized = scale_preview_thumbnail(logo_image, FromDIP(110), FromDIP(60));
        m_preview_thumbnail->SetBitmap(wxBitmap(resized));
    } else {
        m_preview_thumbnail->SetBitmap(create_scaled_bitmap("CoPrintSlicer", m_preview_thumbnail, 96));
    }
    Layout();
}

void PrinterWebView::on_thumbnail_webrequest_state(wxWebRequestEvent &evt)
{
    if (!m_thumbnail_web_request.IsOk())
        return;

    switch (evt.GetState()) {
    case wxWebRequest::State_Completed: {
        m_thumbnail_image = *evt.GetResponse().GetStream();
        if (m_preview_thumbnail != nullptr && m_thumbnail_image.IsOk()) {
            wxImage resized = scale_preview_thumbnail(m_thumbnail_image, FromDIP(120), FromDIP(120));
            m_preview_thumbnail->SetBitmap(wxBitmap(resized));
            Layout();
        } else {
            set_fallback_preview_thumbnail();
        }
        break;
    }
    case wxWebRequest::State_Failed:
    case wxWebRequest::State_Cancelled:
    case wxWebRequest::State_Unauthorized:
        set_fallback_preview_thumbnail();
        break;
    case wxWebRequest::State_Active:
    case wxWebRequest::State_Idle:
        break;
    default:
        break;
    }
}

void PrinterWebView::update_preview_thumbnail(const MachineObject *obj)
{
    if (obj == nullptr || obj->slice_info == nullptr) {
        if (m_thumbnail_web_request.IsOk())
            m_thumbnail_web_request.Cancel();
        set_fallback_preview_thumbnail();
        return;
    }

    wxString next_source = from_u8(obj->slice_info->thumbnail_url);
    if (next_source.IsEmpty() && !obj->slice_info->thumbnail_dir.empty() && !obj->slice_info->thumbnail_name.empty()) {
        wxFileName thumbnail_file(from_u8(obj->slice_info->thumbnail_dir), from_u8(obj->slice_info->thumbnail_name));
        next_source = thumbnail_file.GetFullPath();
    }

    if (next_source.IsEmpty()) {
        if (m_thumbnail_web_request.IsOk())
            m_thumbnail_web_request.Cancel();
        set_fallback_preview_thumbnail();
        return;
    }

    if (next_source == m_preview_thumbnail_url)
        return;

    if (m_thumbnail_web_request.IsOk())
        m_thumbnail_web_request.Cancel();

    if (!is_http_url(next_source)) {
        const wxString local_path = normalize_local_file_url(next_source);
        wxImage local_image;
        if (wxFileName::FileExists(local_path) && local_image.LoadFile(local_path, wxBITMAP_TYPE_ANY) && local_image.IsOk()) {
            m_preview_thumbnail_url = next_source;
            m_preview_thumbnail->SetBitmap(wxBitmap(scale_preview_thumbnail(local_image, FromDIP(120), FromDIP(120))));
            Layout();
            return;
        }
        set_fallback_preview_thumbnail();
        return;
    }

    m_preview_thumbnail_url = next_source;
    m_thumbnail_web_request = wxWebSession::GetDefault().CreateRequest(this, m_preview_thumbnail_url);
    if (!m_thumbnail_web_request.IsOk()) {
        set_fallback_preview_thumbnail();
        return;
    }

    m_thumbnail_web_request.Start();
}

void PrinterWebView::refresh_print_controls_from_selected_machine()
{
    if (m_pause_resume_icon == nullptr)
        return;

    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj != nullptr && obj->can_resume())
        m_pause_resume_icon->SetBitmap(create_scaled_bitmap("Vector", this, 16));
    else
        m_pause_resume_icon->SetBitmap(create_scaled_bitmap("pause", this, 20));
}

void PrinterWebView::refresh_layer_info_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;

    refresh_print_controls_from_selected_machine();

    if (m_filament_load_btn != nullptr && m_filament_unload_btn != nullptr) {
        const bool allow_filament_ops = obj != nullptr && obj->is_online() && !obj->is_in_printing();
        m_filament_load_btn->Enable(allow_filament_ops);
        m_filament_unload_btn->Enable(allow_filament_ops);
        if (m_filament_tool_selector != nullptr)
            m_filament_tool_selector->Enable(allow_filament_ops);
        if (m_manage_filament_title != nullptr)
            m_manage_filament_title->Enable(allow_filament_ops);
    }

    set_active_file_name(active_file_name_text(obj));
    update_preview_thumbnail(obj);

    if (m_connected_printer_panel != nullptr && m_connected_printer_status_label != nullptr && m_connected_printer_name_label != nullptr &&
        m_connected_printer_logout_label != nullptr) {
        const bool has_connected_printer = m_has_active_printer_connection && obj != nullptr && obj->is_online();
        if (has_connected_printer) {
            m_connected_printer_status_label->SetLabelText(wxString::FromUTF8("Ba\xC4\x9Fland\xC4\xB1:"));
            m_connected_printer_name_label->SetLabelText(from_u8(obj->get_dev_name()));
        } else {
            m_connected_printer_status_label->SetLabelText(wxString::FromUTF8("Ba\xC4\x9Fl\xC4\xB1 yaz\xC4\xB1""c\xC4\xB1 yok"));
        }
        m_connected_printer_name_label->Show(has_connected_printer);
        m_connected_printer_logout_label->Show(has_connected_printer);
        if (m_connected_printer_panel->GetParent() != nullptr) {
            m_connected_printer_panel->GetParent()->Layout();
            m_connected_printer_panel->GetParent()->Refresh();
        }
    }

    if (m_print_progress_bar != nullptr) {
        const int pct = (obj != nullptr && obj->mc_print_percent >= 0 && obj->mc_print_percent <= 100)
                            ? obj->mc_print_percent : 0;
        m_print_progress_bar->SetValue(pct);
        if (m_progress_percent_label != nullptr)
            m_progress_percent_label->SetLabelText(wxString::Format("%d%%", pct));
    }

    if (m_bed_temp_value != nullptr) {
        wxString bed_text = "__ / __";
        if (obj != nullptr && obj->GetBed() != nullptr)
            bed_text = wxString::Format("%.1f / %.1f", obj->GetBed()->GetBedTemp(), obj->GetBed()->GetBedTempTarget());
        m_bed_temp_value->SetLabelText(bed_text);
    }

    if (m_extruder_temp_value != nullptr) {
        wxString nozzle_text = "__ / __";
        if (obj != nullptr && obj->GetExtderSystem() != nullptr)
            nozzle_text = wxString::Format(
                "%.1f / %.1f",
                static_cast<double>(obj->GetExtderSystem()->GetNozzleTempCurrent(0)),
                static_cast<double>(obj->GetExtderSystem()->GetNozzleTempTarget(0)));
        m_extruder_temp_value->SetLabelText(nozzle_text);
    }

    if (m_fan_value_label != nullptr && obj != nullptr && obj->GetFan() != nullptr) {
        m_selected_fan = "T1";
        if (m_fan_display_label != nullptr)
            m_fan_display_label->SetLabelText(m_selected_fan);
        m_selected_fan_value = wxString::Format("%d", (int)std::round(obj->GetFan()->GetCoolingFanSpeed() / 25.5f));
        m_fan_value_label->SetLabelText(m_selected_fan_value);
    }

    if (m_ps_bed_temp_label != nullptr) {
        wxString bed_text = "-- / --";
        if (obj != nullptr && obj->GetBed() != nullptr)
            bed_text = wxString::Format("%.1f / %.1f", obj->GetBed()->GetBedTemp(), obj->GetBed()->GetBedTempTarget());
        m_ps_bed_temp_label->SetLabelText(bed_text);
    }
    if (m_ps_t1_temp_label != nullptr) {
        wxString t1_text = "-- / --";
        if (obj != nullptr && obj->GetExtderSystem() != nullptr)
            t1_text = wxString::Format("%.1f / %.1f",
                static_cast<double>(obj->GetExtderSystem()->GetNozzleTempCurrent(0)),
                static_cast<double>(obj->GetExtderSystem()->GetNozzleTempTarget(0)));
        m_ps_t1_temp_label->SetLabelText(t1_text);
    }
    if (m_ps_t1_fan_label != nullptr && obj != nullptr && obj->GetFan() != nullptr)
        m_ps_t1_fan_label->SetLabelText(wxString::Format("%d%%", (int)std::round(obj->GetFan()->GetCoolingFanSpeed() / 25.5f)));

    if (m_printer_name_value != nullptr)
        m_printer_name_value->SetLabelText(obj != nullptr ? from_u8(obj->get_dev_name()) : "N/A");
    if (m_printer_model_value != nullptr)
        m_printer_model_value->SetLabelText(obj != nullptr ? obj->get_printer_type_display_str() : "N/A");
    if (m_printer_serial_value != nullptr) {
        wxString serial = obj != nullptr ? from_u8(obj->get_dev_id()) : "N/A";
        m_printer_serial_value->SetLabelText(serial.MakeUpper());
    }
    if (m_printer_firmware_value != nullptr) {
        wxString version = "N/A";
        if (obj != nullptr) {
            auto ota_it = obj->module_vers.find("ota");
            version = ota_it != obj->module_vers.end() ? ota_it->second.sw_ver : from_u8(obj->get_ota_version());
            if (version.empty())
                version = "N/A";
        }
        m_printer_firmware_value->SetLabelText(version);
    }
    if (m_printer_photo_bitmap != nullptr && obj != nullptr) {
        const wxBitmap bmp = create_scaled_bitmap(obj->get_printer_thumbnail_img_str(), this, 82);
        if (bmp.IsOk())
            m_printer_photo_bitmap->SetBitmap(bmp);
    }

    const std::string camera_machine_id = obj != nullptr ? obj->get_dev_id() : "";
    if (m_camera_webview != nullptr) {
        const std::vector<wxString> camera_urls = configured_camera_stream_urls(obj);
        const wxString next_camera_url = camera_urls.empty() ? wxString() : camera_urls.front();
        const bool machine_changed = camera_machine_id != m_camera_machine_id;
        const bool stream_changed = next_camera_url != m_camera_stream_url;
        m_camera_machine_id = camera_machine_id;
        m_camera_stream_url = next_camera_url;

        if (machine_changed || stream_changed)
            m_camera_webview->SetPage(camera_stream_page(camera_urls), m_camera_stream_url.BeforeLast('/'));
    }

    const int printer_layer = (obj != nullptr && obj->curr_layer > 0) ? obj->curr_layer : -1;
    const int file_layer = (obj != nullptr && obj->total_layers > 0) ? obj->total_layers : -1;
    set_layer_info(printer_layer, file_layer);

    int remaining_seconds = -1;
    if (obj != nullptr) {
        const int total_duration_seconds = (obj->slice_info != nullptr && obj->slice_info->prediction > 0) ? obj->slice_info->prediction : -1;
        if (m_total_time_value != nullptr)
            m_total_time_value->SetLabelText(remaining_minutes_text(total_duration_seconds));
        if (total_duration_seconds > 0 && obj->mc_print_percent >= 0 && obj->mc_print_percent <= 100) {
            const int elapsed_seconds = static_cast<int>((static_cast<long long>(total_duration_seconds) * obj->mc_print_percent) / 100);
            remaining_seconds = total_duration_seconds - elapsed_seconds;
        } else if (obj->mc_left_time > 0) {
            remaining_seconds = obj->mc_left_time;
        }
    }
    set_estimated_remaining_seconds(remaining_seconds);
    refresh_update_page_from_selected_machine();
}

void PrinterWebView::refresh_update_page_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (m_update_page == nullptr)
        return;

    if (obj == nullptr) {
        if (m_update_header_title != nullptr) m_update_header_title->SetLabelText("Yazici secili degil");
        if (m_update_model_value != nullptr) m_update_model_value->SetLabelText("-");
        if (m_update_serial_value != nullptr) m_update_serial_value->SetLabelText("-");
        if (m_update_version_value != nullptr) m_update_version_value->SetLabelText("-");
        if (m_update_status_value != nullptr) m_update_status_value->SetLabelText("Yazici baglantisi bekleniyor");
        if (m_update_percent_value != nullptr) m_update_percent_value->SetLabelText("0%");
        if (m_update_progress_gauge != nullptr) m_update_progress_gauge->SetValue(0);
        return;
    }

    if (m_update_header_title != nullptr) {
        const wxString header_title = wxString::Format("%s(%s)", from_u8(obj->get_dev_name()), obj->is_connected() ? "Hazir" : "Cevrimdisi");
        m_update_header_title->SetLabelText(header_title);
    }

    if (m_update_model_value != nullptr)
        m_update_model_value->SetLabelText(obj->get_printer_type_display_str());

    if (m_update_serial_value != nullptr) {
        wxString serial = from_u8(obj->get_dev_id());
        m_update_serial_value->SetLabelText(serial.MakeUpper());
    }

    wxString current_version = "-";
    wxString next_version;
    auto ota_it = obj->module_vers.find("ota");
    if (ota_it != obj->module_vers.end())
        current_version = ota_it->second.sw_ver;

    if (!obj->ota_new_version_number.empty())
        next_version = from_u8(obj->ota_new_version_number);
    else if (ota_it != obj->module_vers.end())
        next_version = ota_it->second.sw_new_ver;

    if (m_update_version_value != nullptr) {
        if (!next_version.empty() && next_version != current_version && current_version != "-")
            m_update_version_value->SetLabelText(wxString::Format("%s -> %s", current_version, next_version));
        else if (current_version != "-")
            m_update_version_value->SetLabelText(wxString::Format("%s (Son surum)", current_version));
        else
            m_update_version_value->SetLabelText("-");
    }

    int progress = 0;
    wxString status = obj->is_connected() ? "Guncelleme basarili" : "Yazici cevrimdisi";
    wxColour status_colour(0, 166, 76);

    if (!obj->is_connected()) {
        progress = 0;
        status_colour = wxColour(180, 180, 180);
    } else if (obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress) {
        progress = std::max(0, std::min(100, obj->get_upgrade_percent()));
        status = "Guncelleniyor";
    } else if (obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingFinished) {
        progress = std::max(0, std::min(100, obj->get_upgrade_percent()));
        if (obj->upgrade_status == "UPGRADE_FAIL" || obj->upgrade_err_code != UpgradeNoError) {
            status = "Guncelleme basarisiz";
            status_colour = wxColour(231, 111, 81);
        } else {
            status = "Guncelleme basarili";
            if (progress <= 0)
                progress = 100;
        }
    } else if (!next_version.empty() && next_version != current_version && current_version != "-") {
        progress = 100;
        status = "Guncelleme hazir";
    } else if (current_version != "-") {
        progress = 100;
        status = "Guncelleme basarili";
    }

    if (m_update_status_value != nullptr) {
        m_update_status_value->SetLabelText(status);
        m_update_status_value->SetForegroundColour(status_colour);
    }
    if (m_update_percent_value != nullptr) {
        m_update_percent_value->SetLabelText(wxString::Format("%d%%", progress));
        m_update_percent_value->SetForegroundColour(status_colour);
    }
    if (m_update_progress_gauge != nullptr)
        m_update_progress_gauge->SetValue(progress);

    if (m_update_printer_bitmap != nullptr) {
        try {
            const wxBitmap bmp = create_scaled_bitmap(obj->get_printer_thumbnail_img_str(), this, 150);
            if (bmp.IsOk())
                m_update_printer_bitmap->SetBitmap(bmp);
        } catch (...) {
        }
    }

    m_update_page->Layout();
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());
    refresh_layer_info_from_selected_machine();

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_browser == nullptr || m_apikey_sent || m_apikey.IsEmpty())
        return;
    m_apikey_sent   = true;
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();

    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    SendAPIKey();
}

} // GUI
} // Slic3r
