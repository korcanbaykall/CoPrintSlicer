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
#include "slic3r/GUI/DeviceDashboard/services/DashboardStateAdapter.hpp"
#include "slic3r/GUI/DeviceDashboard/panels/PrintStatusPanel.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/Http.hpp"
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
#include <wx/textctrl.h>
#include <wx/dialog.h>
#include <wx/button.h>
#include <wx/menu.h>
#include <wx/simplebook.h>
#include <algorithm>
#include <cctype>
#include <wx/artprov.h>
#include <wx/scrolwin.h>

#include <string>
#include <wx/graphics.h>
#include <wx/dcgraph.h>
#include <wx/event.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <limits>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

static bool looks_like_network_identifier(const std::string &value)
{
    if (value.empty())
        return false;

    // Manual-IP additions used to seed the display name from the address itself.
    // Treat those values as technical identifiers rather than user-facing names.
    const auto has_scheme = value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
    const auto colon_pos  = value.find(':');
    const auto dot_count  = std::count(value.begin(), value.end(), '.');
    const bool ipv4ish    = dot_count == 3 &&
                         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                             return std::isdigit(ch) || ch == '.' || ch == ':';
                         });
    return has_scheme || colon_pos != std::string::npos || ipv4ish;
}

static std::string to_lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

struct LocalIpv4Subnet {
    uint32_t network{ 0 };
    uint32_t broadcast{ 0 };
};

static uint32_t ipv4_to_uint(const std::string &ip)
{
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address_v4(ip, ec);
    return ec ? 0 : address.to_uint();
}

static std::string uint_to_ipv4(uint32_t value)
{
    return boost::asio::ip::address_v4(value).to_string();
}

static std::vector<LocalIpv4Subnet> local_ipv4_subnets()
{
    std::vector<LocalIpv4Subnet> subnets;
    std::unordered_set<uint64_t> seen;
#ifdef _WIN32
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size) == ERROR_BUFFER_OVERFLOW) {
        std::vector<unsigned char> buffer(size);
        auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, addresses, &size) == NO_ERROR) {
            for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp)
                    continue;
                for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                    if (unicast->Address.lpSockaddr == nullptr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    const auto *sockaddr = reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
                    const uint32_t ip = ntohl(sockaddr->sin_addr.s_addr);
                    if ((ip >> 24) == 127 || ip == 0)
                        continue;
                    const uint32_t prefix = unicast->OnLinkPrefixLength;
                    if (prefix == 0 || prefix > 30)
                        continue;
                    const uint32_t mask = prefix == 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix));
                    const uint32_t network = ip & mask;
                    const uint32_t broadcast = network | ~mask;
                    const uint64_t key = (static_cast<uint64_t>(network) << 32) | broadcast;
                    if (seen.insert(key).second)
                        subnets.push_back({ network, broadcast });
                }
            }
        }
    }
#else
    try {
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver(io);
        const auto results = resolver.resolve(boost::asio::ip::host_name(), "");
        for (const auto &entry : results) {
            const auto address = entry.endpoint().address();
            if (!address.is_v4() || address.is_loopback())
                continue;
            const uint32_t ip = address.to_v4().to_uint();
            const uint32_t network = ip & 0xffffff00u;
            const uint32_t broadcast = network | 0x000000ffu;
            const uint64_t key = (static_cast<uint64_t>(network) << 32) | broadcast;
            if (seen.insert(key).second)
                subnets.push_back({ network, broadcast });
        }
    } catch (...) {
    }
#endif
    return subnets;
}

static bool probe_moonraker_host(const std::string &ip, BBLocalMachine &machine)
{
    auto fetch_json = [&](const std::string &path) -> nlohmann::json {
        std::string body;
        bool success = false;
        auto http = Http::get("http://" + ip + ":7125" + path);
        http.timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200) {
                    body = std::move(response);
                    success = true;
                }
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();
        if (!success)
            return nlohmann::json();

        auto json = nlohmann::json::parse(body, nullptr, false, true);
        if (json.is_discarded())
            return nlohmann::json();
        return json.contains("result") ? json["result"] : json;
    };

    const auto server_info = fetch_json("/server/info");
    if (!server_info.is_object())
        return false;

    std::string dev_name = server_info.value("machine_name", server_info.value("hostname", ""));
    const auto printer_info = fetch_json("/printer/info");
    if (printer_info.is_object()) {
        const std::string printer_name = printer_info.value("machine_name", printer_info.value("hostname", ""));
        if (!printer_name.empty())
            dev_name = printer_name;
    }

    machine.dev_ip = ip + ":7125";
    machine.dev_id = machine.dev_ip;
    machine.dev_name = dev_name.empty() ? ip : dev_name;
    machine.printer_type = "Moonraker";
    return true;
}


namespace {

/** CoPrint printers sidebar / list icon: resources/images/cprint_printer_nav.png */
constexpr const char *k_cprint_printer_nav_bitmap = "cprint_printer_nav";

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

wxSize preview_thumbnail_target_size(const wxStaticBitmap *bitmap, int fallback_w, int fallback_h)
{
    if (bitmap == nullptr)
        return wxSize(fallback_w, fallback_h);
    wxSize size = bitmap->GetMinSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        size = bitmap->GetSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        size = wxSize(fallback_w, fallback_h);
    return size;
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

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            static constexpr double kPi = 3.14159265358979323846;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gc->SetBrush(wxBrush(m_fill));
            gc->SetPen(*wxTRANSPARENT_PEN);

            // Top-left and top-right rounded only; bottom edge of header stays straight.
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(x + r, y);
            path.AddLineToPoint(x + w - r, y);
            path.AddArc(x + w - r, y + r, r, -kPi / 2.0, 0.0, false);
            path.AddLineToPoint(x + w, y + h);
            path.AddLineToPoint(x, y + h);
            path.AddLineToPoint(x, y + r);
            path.AddArc(x + r, y + r, r, kPi, 1.5 * kPi, false);
            path.CloseSubpath();
            gc->FillPath(path);
            return;
        }

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

class LeftRoundedColourPanel : public wxPanel
{
public:
    LeftRoundedColourPanel(wxWindow *parent, const wxColour &fill, int radius, const wxColour &background)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_fill(fill)
        , m_background(background)
        , m_radius(radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(m_background);
        SetDoubleBuffered(true);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
        Bind(wxEVT_PAINT, &LeftRoundedColourPanel::on_paint, this);
    }

    void SetFillColour(const wxColour &fill)
    {
        m_fill = fill;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        const wxRect rect = GetClientRect();
        if (rect.width <= 0 || rect.height <= 0)
            return;

        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const double x = rect.x;
        const double y = rect.y;
        const double w = rect.width;
        const double h = rect.height;
        const double r = std::min<double>(m_radius, std::min(w * 0.5, h * 0.5));

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gc->SetBrush(wxBrush(m_fill));
            gc->SetPen(*wxTRANSPARENT_PEN);
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(x + w, y);
            path.AddLineToPoint(x + r, y);
            path.AddQuadCurveToPoint(x, y, x, y + r);
            path.AddLineToPoint(x, y + h - r);
            path.AddQuadCurveToPoint(x, y + h, x + r, y + h);
            path.AddLineToPoint(x + w, y + h);
            path.CloseSubpath();
            gc->DrawPath(path);
            return;
        }

        const int ri = std::max(1, static_cast<int>(r + 0.5));
        dc.SetBrush(wxBrush(m_fill));
        dc.SetPen(wxPen(m_fill, 1));
        dc.DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, ri);
        dc.DrawRectangle(rect.x + ri, rect.y, rect.width - ri, rect.height);
    }

    wxColour m_fill;
    wxColour m_background;
    int      m_radius;
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

std::array<wxColour, 4> default_filament_preview_colors()
{
    return {
        wxColour(214, 181, 46),
        wxColour(57, 145, 212),
        wxColour(217, 101, 43),
        wxColour(164, 207, 42)
    };
}

std::array<wxString, 4> default_filament_preview_materials()
{
    return { wxString("PLA"), wxString("PLA"), wxString("PLA"), wxString("PLA") };
}

std::array<wxString, 4> default_filament_preview_weights()
{
    return { wxString("14.3g"), wxString("14.3g"), wxString("14.3g"), wxString("14.3g") };
}

std::array<int, 4> default_filament_preview_assigned_tools()
{
    return { 1, 2, 3, 4 };
}

wxColour colour_from_hex(const std::string &hex, const wxColour &fallback)
{
    std::string v = hex;
    if (!v.empty() && v.front() == '#')
        v.erase(v.begin());
    if (v.size() < 6)
        return fallback;
    try {
        const int r = std::stoi(v.substr(0, 2), nullptr, 16);
        const int g = std::stoi(v.substr(2, 2), nullptr, 16);
        const int b = std::stoi(v.substr(4, 2), nullptr, 16);
        return wxColour(r, g, b);
    } catch (...) {
        return fallback;
    }
}

wxString hex_from_colour(const wxColour &color)
{
    return wxString::Format("#%02X%02X%02X", color.Red(), color.Green(), color.Blue());
}

bool looks_like_hex_colour(wxString value)
{
    value.Trim(true);
    value.Trim(false);
    if (value.StartsWith("#"))
        value = value.Mid(1);
    if (value.length() != 6)
        return false;
    const std::string utf8 = into_u8(value);
    if (utf8.size() != 6)
        return false;
    for (const unsigned char ch : utf8) {
        if (!std::isxdigit(ch))
            return false;
    }
    return true;
}

std::string url_encode_component(const wxString &text)
{
    const std::string input = into_u8(text);
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char ch : input) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0f]);
        }
    }
    return out;
}

std::string moonraker_base_url(const MachineObject *obj)
{
    if (obj == nullptr)
        return {};
    std::string host = obj->get_dev_ip();
    if (host.empty())
        return {};
    if (host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0)
        return host;
    if (host.find(':') == std::string::npos)
        host += ":7125";
    return "http://" + host;
}

double rgb_distance(const wxColour &a, const wxColour &b)
{
    const double dr = static_cast<double>(a.Red()) - static_cast<double>(b.Red());
    const double dg = static_cast<double>(a.Green()) - static_cast<double>(b.Green());
    const double db = static_cast<double>(a.Blue()) - static_cast<double>(b.Blue());
    return std::sqrt(dr * dr + dg * dg + db * db);
}

std::array<int, 4> nearest_unique_tool_assignment(const std::array<wxColour, 4> &model_colors,
                                                  const std::array<wxColour, 4> &tool_colors)
{
    std::array<int, 4> tools { 1, 2, 3, 4 };
    std::array<int, 4> best = tools;
    double best_cost = std::numeric_limits<double>::infinity();
    std::sort(tools.begin(), tools.end());
    do {
        double cost = 0.0;
        for (int i = 0; i < 4; ++i)
            cost += rgb_distance(model_colors[i], tool_colors[tools[i] - 1]);
        if (cost < best_cost) {
            best_cost = cost;
            best = tools;
        }
    } while (std::next_permutation(tools.begin(), tools.end()));
    return best;
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

wxString active_file_metadata_path(const MachineObject *obj)
{
    if (obj == nullptr)
        return wxString();

    wxString path;
    if (!obj->m_gcode_file.empty())
        path = from_u8(obj->m_gcode_file);
    else if (!obj->subtask_name.empty())
        path = from_u8(obj->subtask_name);

    path.Trim(true);
    path.Trim(false);
    if (path.empty() || path == "N/A")
        return wxString();

    path.Replace("\\", "/");
    if (path.StartsWith("file://"))
        path = path.Mid(7);

    const wxString lower = path.Lower();
    int gcodes_pos = lower.Find("/gcodes/");
    if (gcodes_pos != wxNOT_FOUND)
        path = path.Mid(gcodes_pos + 8);
    else if (lower.StartsWith("gcodes/"))
        path = path.Mid(7);
    else if (wxFileName(path).IsAbsolute())
        path = wxFileName(path).GetFullName();

    while (path.StartsWith("/"))
        path = path.Mid(1);
    return path;
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
    m_filament_loaded_tool_colors = default_filament_preview_colors();
    m_filament_loaded_tool_materials = default_filament_preview_materials();

    auto *main_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *preview_menu_panel = new wxPanel(this, wxID_ANY);
    preview_menu_panel->SetBackgroundColour(wxColour("#2A2C2E"));
    preview_menu_panel->SetMinSize(wxSize(FromDIP(259), FromDIP(360)));
    preview_menu_panel->SetMaxSize(wxSize(FromDIP(259), -1));
    auto *preview_menu_sizer = new wxBoxSizer(wxVERTICAL);

    {
        auto *header_panel = new wxPanel(preview_menu_panel, wxID_ANY);
        m_sidebar_header_panel = header_panel;
        header_panel->SetBackgroundColour(wxColour("#2A2C2E"));
        header_panel->SetMinSize(wxSize(-1, FromDIP(50)));
        header_panel->SetMaxSize(wxSize(-1, FromDIP(50)));
        auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_sidebar_header_back = new wxStaticText(header_panel, wxID_ANY, wxString::FromUTF8("\xC3\x97"));
        m_sidebar_header_back->SetForegroundColour(wxColour(235, 235, 235));
        m_sidebar_header_back->SetCursor(wxCursor(wxCURSOR_HAND));
        {
            wxFont f = m_sidebar_header_back->GetFont();
            f.SetPointSize(16);
            m_sidebar_header_back->SetFont(f);
        }
        m_sidebar_header_back->Hide();
        header_sizer->Add(m_sidebar_header_back, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        header_sizer->AddStretchSpacer(1);
        auto *printers_label = new wxStaticText(header_panel, wxID_ANY, "Printers");
        m_sidebar_header_title = printers_label;
        printers_label->SetForegroundColour(wxColour(235, 235, 235));
        {
            wxFont f = printers_label->GetFont();
            f.SetPointSize(14);
            f.SetWeight(wxFONTWEIGHT_BOLD);
            printers_label->SetFont(f);
        }
        header_sizer->Add(printers_label, 0, wxALIGN_CENTER_VERTICAL);
        header_sizer->AddStretchSpacer(1);
        auto *printers_add = new wxStaticText(header_panel, wxID_ANY, "+ Add");
        m_sidebar_header_add = printers_add;
        printers_add->SetForegroundColour(wxColour(74, 149, 37));
        {
            wxFont f = printers_add->GetFont();
            f.SetPointSize(12);
            printers_add->SetFont(f);
        }
        printers_add->SetCursor(wxCursor(wxCURSOR_HAND));
        header_sizer->Add(printers_add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
        header_panel->SetSizer(header_sizer);
        m_preview_printers_button = header_panel;
        printers_add->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) {
            evt.StopPropagation();
            show_sidebar_add_printer_view();
        });
        m_sidebar_header_back->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) {
            evt.StopPropagation();
            if (m_sidebar_add_printer_panel != nullptr && m_sidebar_add_printer_panel->IsShown())
                show_sidebar_printers_view();
            else
                show_sidebar_root_view();
        });
        header_panel->Hide();
        preview_menu_sizer->Add(header_panel, 0, wxEXPAND);
    }

    m_sidebar_root_panel = new wxPanel(preview_menu_panel, wxID_ANY);
    m_sidebar_root_panel->SetBackgroundColour(wxColour("#2A2C2E"));
    m_sidebar_root_sizer = new wxBoxSizer(wxVERTICAL);
    m_sidebar_root_panel->SetSizer(m_sidebar_root_sizer);
    preview_menu_sizer->Add(m_sidebar_root_panel, 1, wxEXPAND);

    m_sidebar_printer_list_panel = new wxScrolledWindow(preview_menu_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_sidebar_printer_list_panel->SetBackgroundColour(wxColour("#2A2C2E"));
    if (auto *scrolled = dynamic_cast<wxScrolledWindow *>(m_sidebar_printer_list_panel)) {
        scrolled->SetScrollRate(0, FromDIP(8));
        scrolled->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    }
    m_sidebar_printer_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_sidebar_printer_list_panel->SetSizer(m_sidebar_printer_list_sizer);
    m_sidebar_printer_list_panel->Hide();
    preview_menu_sizer->Add(m_sidebar_printer_list_panel, 1, wxEXPAND);

    m_sidebar_add_printer_panel = new wxPanel(preview_menu_panel, wxID_ANY);
    m_sidebar_add_printer_panel->SetBackgroundColour(wxColour("#2A2C2E"));
    m_sidebar_add_printer_panel->Hide();
    preview_menu_sizer->Add(m_sidebar_add_printer_panel, 1, wxEXPAND);
    auto add_sidebar_nav_row = [this](wxWindow *parent,
                                      wxBoxSizer *parent_sizer,
                                      const wxString &label,
                                      const std::string &icon_name,
                                      const std::function<void()> &on_activate) {
        auto *row = new wxPanel(parent, wxID_ANY);
        row->SetBackgroundColour(wxColour("#2A2C2E"));
        row->SetMinSize(wxSize(-1, FromDIP(37)));
        row->SetMaxSize(wxSize(-1, FromDIP(37)));
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *sz = new wxBoxSizer(wxHORIZONTAL);
        sz->AddSpacer(FromDIP(18));
        auto *icon = new wxStaticBitmap(row, wxID_ANY, create_scaled_bitmap(icon_name, row, 14));
        auto *text = new wxStaticText(row, wxID_ANY, label);
        text->SetForegroundColour(wxColour("#E1E3E5"));
        auto *chev = new wxStaticText(row, wxID_ANY, ">");
        chev->SetForegroundColour(wxColour("#B7BCC2"));
        sz->Add(icon, 0, wxALIGN_CENTER_VERTICAL);
        sz->AddSpacer(FromDIP(10));
        sz->Add(text, 1, wxALIGN_CENTER_VERTICAL);
        sz->Add(chev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
        row->SetSizer(sz);
        auto on_click = [on_activate](wxMouseEvent &) { on_activate(); };
        row->Bind(wxEVT_LEFT_DOWN, on_click);
        icon->Bind(wxEVT_LEFT_DOWN, on_click);
        text->Bind(wxEVT_LEFT_DOWN, on_click);
        chev->Bind(wxEVT_LEFT_DOWN, on_click);
        parent_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));
    };

    m_sidebar_root_sizer->AddSpacer(FromDIP(10));
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("Printers"), k_cprint_printer_nav_bitmap,
                        [this]() { show_sidebar_printers_view(); });
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("System Upgrade"), "monitor_upgrade_online",
                        [this]() { select_tab(PrinterWebViewTab::Update); });
    auto *sidebar_divider = new wxPanel(m_sidebar_root_panel, wxID_ANY);
    sidebar_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    sidebar_divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    sidebar_divider->SetBackgroundColour(wxColour("#34373A"));
    m_sidebar_root_sizer->Add(sidebar_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("Media"), "monitor_sdcard_thumbnail",
                        [this]() { select_tab(PrinterWebViewTab::Storage); });
    preview_menu_sizer->AddStretchSpacer(1);
    preview_menu_panel->SetSizer(preview_menu_sizer);

    m_printers_popup = new wxPopupTransientWindow(this, wxBORDER_NONE);
    m_printers_popup_panel = new wxPanel(m_printers_popup, wxID_ANY);
    m_printers_popup_panel->SetBackgroundColour(wxColour(255, 255, 255));
    rebuild_printers_popup();
    rebuild_sidebar_printer_list();
    show_sidebar_root_view();

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

    m_filament_tool_popup = new wxPopupTransientWindow(this, wxBORDER_NONE);
    m_filament_tool_popup_panel = new wxPanel(m_filament_tool_popup, wxID_ANY);
    m_filament_tool_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));
    rebuild_filament_tool_popup();

    auto *content_host = new wxPanel(this, wxID_ANY);
    content_host->SetBackgroundColour(wxColour(28, 30, 34));
    auto *content_host_sizer = new wxBoxSizer(wxVERTICAL);

    m_status_page = new wxPanel(content_host, wxID_ANY);
    m_status_page->SetBackgroundColour(wxColour(28, 30, 34));
    auto *status_page_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto *status_content = new wxPanel(m_status_page, wxID_ANY);
    status_content->SetBackgroundColour(wxColour(28, 30, 34));
    auto *status_content_sizer = new wxBoxSizer(wxVERTICAL);

    auto *left_container = new wxPanel(status_content, wxID_ANY);
    left_container->SetBackgroundColour(wxColour(28, 30, 34));
    auto *left_sizer = new wxBoxSizer(wxVERTICAL);
    auto *top_row = new wxBoxSizer(wxHORIZONTAL);
    auto *preview_box = new StaticBox(left_container, wxID_ANY);
    preview_box->SetCornerRadius(FromDIP(12));
    preview_box->SetBorderWidth(1);
    preview_box->SetBorderColorNormal(wxColour(55, 58, 64));
    preview_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    preview_box->SetBackgroundColour(wxColour(28, 30, 34));
    preview_box->SetMinSize(wxSize(FromDIP(460), -1));
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
    m_camera_webview_host->SetMinSize(wxSize(FromDIP(420), FromDIP(300)));
    m_camera_webview_host->SetBackgroundColour(wxColour(0, 0, 0));
    preview_box_sizer->Add(m_camera_webview_host, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));
    preview_box->SetSizer(preview_box_sizer);
    auto update_camera_host_responsive_size = [this, preview_box]() {
        if (preview_box == nullptr || m_camera_webview_host == nullptr)
            return;
        const int box_w = preview_box->GetClientSize().GetWidth();
        if (box_w <= 0)
            return;
        const int host_w = std::max(FromDIP(320), box_w - FromDIP(30));
        const int host_h = std::clamp(static_cast<int>(host_w * 0.78), FromDIP(260), FromDIP(454));
        m_camera_webview_host->SetMinSize(wxSize(host_w, host_h));
        preview_box->Layout();
    };
    preview_box->Bind(wxEVT_SIZE, [update_camera_host_responsive_size](wxSizeEvent &event) {
        event.Skip();
        update_camera_host_responsive_size();
    });
    CallAfter(update_camera_host_responsive_size);
    top_row->Add(preview_box, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));

    auto make_upper_placeholder_box = [this, left_container]() {
        auto *box = new StaticBox(left_container, wxID_ANY);
        box->SetMinSize(wxSize(FromDIP(620), FromDIP(340)));
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
    const int left_shell_width = FromDIP(238);
    const int right_shell_width = FromDIP(136);
    const int tool_row_gap = FromDIP(16);
    const int arrow_column_width = FromDIP(24);
    const int color_strip_vertical_inset = FromDIP(1);
    std::vector<StaticBox *> filament_model_shells;
    std::vector<StaticBox *> filament_assigned_shells;
    std::vector<LeftRoundedColourPanel *> filament_model_color_strips;
    std::vector<LeftRoundedColourPanel *> filament_assigned_color_strips;

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

    const std::array<wxColour, 4> filament_colors = default_filament_preview_colors();

    for (int i = 0; i < 4; ++i) {
        auto *tool_row = new wxBoxSizer(wxHORIZONTAL);
        tool_row->AddStretchSpacer(1);

        auto *left_shell = new StaticBox(upper_placeholder_box, wxID_ANY);
        left_shell->SetMinSize(wxSize(left_shell_width, FromDIP(44)));
        left_shell->SetMaxSize(wxSize(left_shell_width, FromDIP(44)));
        filament_model_shells.push_back(left_shell);
        left_shell->SetCornerRadius(FromDIP(12));
        left_shell->SetBorderWidth(0);
        left_shell->SetBackgroundColorNormal(wxColour(43, 46, 52));
        left_shell->SetBackgroundColour(wxColour(43, 46, 52));
        auto *left_shell_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *left_color = new LeftRoundedColourPanel(left_shell, filament_colors[i], FromDIP(10), wxColour(43, 46, 52));
        left_color->SetMinSize(wxSize(FromDIP(83), FromDIP(44)));
        left_color->SetMaxSize(wxSize(FromDIP(83), FromDIP(44)));
        filament_model_color_strips.push_back(left_color);
        m_filament_model_color_panels[i] = left_color;
        left_shell_sizer->Add(left_color, 0, wxEXPAND | wxTOP | wxBOTTOM, color_strip_vertical_inset);
        left_shell_sizer->AddSpacer(FromDIP(18));

        auto *material_label = new wxStaticText(left_shell, wxID_ANY, "PLA");
        material_label->SetForegroundColour(wxColour(235, 235, 235));
        wxFont material_font = material_label->GetFont();
        material_font.SetWeight(wxFONTWEIGHT_BOLD);
        material_label->SetFont(material_font);
        m_filament_material_labels[i] = material_label;
        left_shell_sizer->Add(material_label, 0, wxALIGN_CENTER_VERTICAL);
        left_shell_sizer->AddStretchSpacer(1);

        auto *weight_label = new wxStaticText(left_shell, wxID_ANY, "14.3g");
        weight_label->SetForegroundColour(wxColour(220, 220, 220));
        m_filament_weight_labels[i] = weight_label;
        left_shell_sizer->Add(weight_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
        left_shell->SetSizer(left_shell_sizer);
        tool_row->Add(left_shell, 0, wxALIGN_CENTER_VERTICAL);

        tool_row->AddSpacer(FromDIP(16));
        auto *arrow_icon = new wxStaticBitmap(
            upper_placeholder_box,
            wxID_ANY,
            create_scaled_bitmap("filament_forward", upper_placeholder_box, 24));
        tool_row->Add(arrow_icon, 0, wxALIGN_CENTER_VERTICAL);
        tool_row->AddSpacer(FromDIP(16));

        auto *right_shell = new StaticBox(upper_placeholder_box, wxID_ANY);
        right_shell->SetMinSize(wxSize(FromDIP(136), FromDIP(44)));
        right_shell->SetMaxSize(wxSize(FromDIP(136), FromDIP(44)));
        filament_assigned_shells.push_back(right_shell);
        right_shell->SetCornerRadius(FromDIP(12));
        right_shell->SetBorderWidth(0);
        right_shell->SetBackgroundColorNormal(wxColour(43, 46, 52));
        right_shell->SetBackgroundColour(wxColour(43, 46, 52));
        right_shell->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *right_shell_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *right_color = new LeftRoundedColourPanel(right_shell, filament_colors[i], FromDIP(10), wxColour(43, 46, 52));
        right_color->SetMinSize(wxSize(FromDIP(39), FromDIP(44)));
        right_color->SetMaxSize(wxSize(FromDIP(39), FromDIP(44)));
        filament_assigned_color_strips.push_back(right_color);
        right_color->SetCursor(wxCursor(wxCURSOR_HAND));
        m_filament_assigned_color_panels[i] = right_color;
        right_shell_sizer->Add(right_color, 0, wxEXPAND);
        right_shell_sizer->AddSpacer(FromDIP(16));

        auto *tool_label = new wxStaticText(right_shell, wxID_ANY, wxString::Format("T%d", i + 1));
        tool_label->SetForegroundColour(wxColour(235, 235, 235));
        wxFont tool_font = tool_label->GetFont();
        tool_font.SetWeight(wxFONTWEIGHT_BOLD);
        tool_label->SetFont(tool_font);
        tool_label->SetCursor(wxCursor(wxCURSOR_HAND));
        m_filament_assigned_tool_labels[i] = tool_label;
        right_shell_sizer->Add(tool_label, 0, wxALIGN_CENTER_VERTICAL);
        right_shell_sizer->AddStretchSpacer(1);

        auto *refresh_label = new wxStaticBitmap(right_shell, wxID_ANY, create_scaled_bitmap("assigned_tools_update", right_shell, 14));
        refresh_label->SetCursor(wxCursor(wxCURSOR_HAND));
        right_shell_sizer->Add(refresh_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
        right_shell->SetSizer(right_shell_sizer);
        const auto show_tool_menu = [this, i, right_shell](wxMouseEvent &) {
            wxMenu menu;
            for (int tool = 1; tool <= 4; ++tool)
                menu.Append(1000 + tool, wxString::Format("T%d", tool));
            menu.Bind(wxEVT_MENU, [this, i](wxCommandEvent &evt) {
                const int selected_tool = evt.GetId() - 1000;
                set_filament_assigned_tool(i, selected_tool, true);
            });
            right_shell->PopupMenu(&menu);
        };
        right_shell->Bind(wxEVT_LEFT_DOWN, show_tool_menu);
        right_color->Bind(wxEVT_LEFT_DOWN, show_tool_menu);
        tool_label->Bind(wxEVT_LEFT_DOWN, show_tool_menu);
        refresh_label->Bind(wxEVT_LEFT_DOWN, show_tool_menu);
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
    auto *tool_sel_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *tool_color_dot = new StaticBox(tool_selector, wxID_ANY);
    tool_color_dot->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    tool_color_dot->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    tool_color_dot->SetCornerRadius(FromDIP(8));
    tool_color_dot->SetBorderWidth(0);
    tool_color_dot->SetBackgroundColorNormal(filament_colors[0]);
    tool_color_dot->SetBackgroundColour(filament_colors[0]);
    tool_color_dot->SetCursor(wxCursor(wxCURSOR_HAND));
    tool_sel_sizer->Add(tool_color_dot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    tool_sel_sizer->AddSpacer(FromDIP(8));
    auto *tool_name_lbl = new wxStaticText(tool_selector, wxID_ANY, "Tool 1");
    tool_name_lbl->SetForegroundColour(wxColour(220, 220, 220));
    tool_name_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
    {
        wxFont f = tool_name_lbl->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        tool_name_lbl->SetFont(f);
    }
    tool_sel_sizer->Add(tool_name_lbl, 1, wxALIGN_CENTER_VERTICAL);
    auto *dropdown_arrow = new wxStaticText(tool_selector, wxID_ANY, wxString::FromUTF8("\xE2\x8C\x84"));
    dropdown_arrow->SetForegroundColour(wxColour(150, 155, 165));
    dropdown_arrow->SetCursor(wxCursor(wxCURSOR_HAND));
    tool_sel_sizer->Add(dropdown_arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    tool_selector->SetSizer(tool_sel_sizer);
    mf_sizer->Add(tool_selector, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    mf_sizer->AddSpacer(FromDIP(10));

    m_filament_tool_color_dot = tool_color_dot;
    m_filament_tool_name_lbl  = tool_name_lbl;
    m_filament_tool_selector   = tool_selector;
    m_filament_tool_popup_button = tool_selector;
    m_selected_filament_tool  = 0;

    const auto filament_tool_row_click = [this](wxMouseEvent &) { toggle_filament_tool_popup(); };
    tool_selector->SetCursor(wxCursor(wxCURSOR_HAND));
    tool_selector->Bind(wxEVT_LEFT_DOWN, filament_tool_row_click);
    tool_color_dot->Bind(wxEVT_LEFT_DOWN, filament_tool_row_click);
    tool_name_lbl->Bind(wxEVT_LEFT_DOWN, filament_tool_row_click);
    dropdown_arrow->Bind(wxEVT_LEFT_DOWN, filament_tool_row_click);

    // Load button
    auto *load_btn = new Button(upper_placeholder_box, _L("Load"));
    load_btn->SetMinSize(wxSize(-1, FromDIP(40)));
    load_btn->SetCornerRadius(FromDIP(8));
    load_btn->SetBorderWidth(0);
    load_btn->SetBackgroundColorNormal(wxColour(65, 68, 75));
    load_btn->SetTextColorNormal(wxColour(220, 220, 220));
    load_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { prompt_and_save_filament_selection_then_load(); });
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
        clear_filament_selection_from_moonraker(m_selected_filament_tool + 1);
    });
    mf_sizer->Add(unload_btn, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_filament_load_btn   = load_btn;
    m_filament_unload_btn = unload_btn;

    mf_sizer->AddStretchSpacer(1);
    filament_qt_sizer->Add(mf_sizer, 224, wxEXPAND);
    filament_qt_sizer->SetItemMinSize(mf_sizer, FromDIP(188), -1);
    upper_placeholder_box->SetSizer(filament_qt_sizer);

    auto update_filament_management_responsive_widths = [
        this,
        upper_placeholder_box,
        upper_header_row,
        model_colors_slot,
        assigned_tools_slot,
        filament_qt_sizer,
        upper_placeholder_sizer,
        mf_sizer,
        filament_model_shells,
        filament_assigned_shells,
        filament_model_color_strips,
        filament_assigned_color_strips
    ]() {
        if (upper_placeholder_box == nullptr)
            return;

        const int card_w = upper_placeholder_box->GetClientSize().GetWidth();
        if (card_w <= 0)
            return;

        const int manage_w = std::clamp(static_cast<int>(card_w * 0.28), FromDIP(188), FromDIP(224));
        const int table_w = std::max(FromDIP(360), card_w - manage_w - FromDIP(54));
        const int fixed_between_cols = FromDIP(16 + 24 + 16);
        const int right_w = std::clamp(static_cast<int>(table_w * 0.30), FromDIP(116), FromDIP(156));
        const int left_w = std::clamp(table_w - fixed_between_cols - right_w, FromDIP(190), FromDIP(305));
        const int left_color_w = std::clamp(static_cast<int>(left_w * 0.35), FromDIP(58), FromDIP(92));
        const int right_color_w = std::clamp(static_cast<int>(right_w * 0.30), FromDIP(28), FromDIP(48));

        upper_header_row->SetItemMinSize(model_colors_slot, left_w, -1);
        upper_header_row->SetItemMinSize(assigned_tools_slot, right_w, -1);
        filament_qt_sizer->SetItemMinSize(mf_sizer, manage_w, -1);
        filament_qt_sizer->SetItemMinSize(upper_placeholder_sizer, left_w + fixed_between_cols + right_w + FromDIP(50), -1);

        for (auto *shell : filament_model_shells) {
            if (shell == nullptr)
                continue;
            shell->SetMinSize(wxSize(left_w, FromDIP(44)));
            shell->SetMaxSize(wxSize(left_w, FromDIP(44)));
        }
        for (auto *strip : filament_model_color_strips) {
            if (strip == nullptr)
                continue;
            strip->SetMinSize(wxSize(left_color_w, FromDIP(44)));
            strip->SetMaxSize(wxSize(left_color_w, FromDIP(44)));
        }
        for (auto *shell : filament_assigned_shells) {
            if (shell == nullptr)
                continue;
            shell->SetMinSize(wxSize(right_w, FromDIP(44)));
            shell->SetMaxSize(wxSize(right_w, FromDIP(44)));
        }
        for (auto *strip : filament_assigned_color_strips) {
            if (strip == nullptr)
                continue;
            strip->SetMinSize(wxSize(right_color_w, FromDIP(44)));
            strip->SetMaxSize(wxSize(right_color_w, FromDIP(44)));
        }

        upper_placeholder_box->Layout();
        upper_placeholder_box->Refresh();
    };
    upper_placeholder_box->Bind(wxEVT_SIZE, [update_filament_management_responsive_widths](wxSizeEvent &event) {
        event.Skip();
        update_filament_management_responsive_widths();
    });
    CallAfter(update_filament_management_responsive_widths);

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
        btn->SetBorderWidth(1);
        btn->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(61, 64, 68));
        btn->SetBackgroundColorNormal(active ? wxColour(61, 64, 68) : wxColour(61, 64, 68));
        btn->SetTextColorNormal(active ? wxColour(235, 235, 235) : wxColour(215, 215, 215));
        return btn;
    };

    auto *tool_col = new wxBoxSizer(wxVERTICAL);
    auto *t1_btn = make_tool_btn(right_container, "T1", true);
    auto *t2_btn = make_tool_btn(right_container, "T2");
    auto *t3_btn = make_tool_btn(right_container, "T3");
    auto *t4_btn = make_tool_btn(right_container, "T4");
    std::vector<Button *> tool_buttons { t1_btn, t2_btn, t3_btn, t4_btn };
    m_axis_tool_buttons = { t1_btn, t2_btn, t3_btn, t4_btn };
    auto refresh_tool_buttons = [this, tool_buttons]() {
        for (int i = 0; i < static_cast<int>(tool_buttons.size()); ++i) {
            Button *btn = tool_buttons[i];
            if (btn == nullptr)
                continue;
            const bool active = i == m_selected_extruder_index;
            btn->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(61, 64, 68));
            btn->SetBackgroundColorNormal(wxColour(61, 64, 68));
            btn->SetTextColorNormal(active ? wxColour(235, 235, 235) : wxColour(215, 215, 215));
            btn->Refresh();
        }
    };
    for (int i = 0; i < static_cast<int>(tool_buttons.size()); ++i) {
        Button *btn = tool_buttons[i];
        btn->SetCursor(wxCursor(wxCURSOR_HAND));
        btn->Bind(wxEVT_BUTTON, [this, i, refresh_tool_buttons](wxCommandEvent &) {
            apply_printer_status_tool_selection(i);
            refresh_tool_buttons();
        });
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
    auto center_child_in_parent = [](wxWindow *parent, wxWindow *child) {
        if (parent == nullptr || child == nullptr)
            return;
        const wxSize parent_size = parent->GetClientSize();
        const wxSize child_size = child->GetBestSize();
        child->Move(
            std::max(0, (parent_size.GetWidth() - child_size.GetWidth()) / 2),
            std::max(0, (parent_size.GetHeight() - child_size.GetHeight()) / 2));
    };
    top_btn->Bind(wxEVT_SIZE, [center_child_in_parent, top_btn, top_z_label](wxSizeEvent &event) {
        event.Skip();
        center_child_in_parent(top_btn, top_z_label);
    });
    CallAfter([center_child_in_parent, top_btn, top_z_label]() {
        center_child_in_parent(top_btn, top_z_label);
    });

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
    auto *bottom_divider = new wxPanel(bottom_split_host, wxID_ANY);
    bottom_divider->SetMinSize(wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetMaxSize(wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetBackgroundColour(wxColour(70, 74, 82));
    auto *bottom_z_label = new wxStaticText(bottom_split_host, wxID_ANY, "-Z");
    bottom_z_label->SetForegroundColour(wxColour(45, 48, 55));
    bottom_z_label->SetBackgroundColour(wxColour(214, 214, 214));
    auto layout_bottom_z_button = [bottom_split_host, bottom_split_bitmap, bottom_divider, bottom_z_label, this]() {
        if (bottom_split_host == nullptr)
            return;
        const wxSize host_size = bottom_split_host->GetClientSize();
        if (bottom_split_bitmap != nullptr) {
            const wxSize bmp_size = bottom_split_bitmap->GetBestSize();
            bottom_split_bitmap->Move(
                std::max(0, (host_size.GetWidth() - bmp_size.GetWidth()) / 2),
                std::max(0, (host_size.GetHeight() - bmp_size.GetHeight()) / 2));
        }
        if (bottom_divider != nullptr) {
            bottom_divider->SetSize(
                std::max(0, (host_size.GetWidth() - this->FromDIP(1)) / 2),
                0,
                this->FromDIP(1),
                std::max(1, host_size.GetHeight()));
        }
        if (bottom_z_label != nullptr) {
            const wxSize label_size = bottom_z_label->GetBestSize();
            bottom_z_label->Move(
                std::max(0, (host_size.GetWidth() - label_size.GetWidth()) / 2),
                std::max(0, (host_size.GetHeight() - label_size.GetHeight()) / 2));
        }
    };
    bottom_split_host->Bind(wxEVT_SIZE, [layout_bottom_z_button](wxSizeEvent &event) {
        event.Skip();
        layout_bottom_z_button();
    });
    CallAfter(layout_bottom_z_button);
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
        obj->command_set_nozzle_new(m_selected_extruder_index, (int)entered_value);
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

    // PrintStatusPanel replaces the old progress_box. Thumbnail is loaded through
    // m_preview_thumbnail which now points at the panel's internal bitmap widget.
    m_dashboard_print_status_panel = new DeviceDashboard::PrintStatusPanel(left_container);
    m_preview_thumbnail = m_dashboard_print_status_panel->thumbnail_widget();
    set_fallback_preview_thumbnail();
    m_dashboard_print_status_panel->set_pause_handler([this]() {
        DeviceDashboard::DeviceCommand cmd;
        cmd.kind = DeviceDashboard::DeviceCommandKind::PausePrint;
        handle_dashboard_command(cmd);
    });
    m_dashboard_print_status_panel->set_stop_handler([this]() {
        DeviceDashboard::DeviceCommand cmd;
        cmd.kind = DeviceDashboard::DeviceCommandKind::StopPrint;
        handle_dashboard_command(cmd);
    });

    auto *left_main_column = new wxBoxSizer(wxVERTICAL);
    left_main_column->Add(preview_box, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));
    left_main_column->AddSpacer(FromDIP(8));
    left_main_column->Add(m_dashboard_print_status_panel, 0, wxEXPAND);

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
    auto make_ps_header = [this](wxWindow *parent, const wxString &txt, bool active) -> wxStaticText * {
        auto *lbl = new wxStaticText(parent, wxID_ANY, txt, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
        lbl->SetForegroundColour(active ? wxColour(220, 220, 220) : wxColour(120, 125, 135));
        // Do not paint an opaque label background — it would hide PsCardHeaderPanel's rounded top corners.
        lbl->SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
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
    auto make_icon_row = [this, ps_card_body_bg](wxWindow *card, const std::string &icon_name, wxStaticText *lbl) -> wxPanel * {
        auto *row_panel = new wxPanel(card, wxID_ANY);
        row_panel->SetBackgroundColour(ps_card_body_bg);
        row_panel->SetMinSize(wxSize(-1, this->FromDIP(20)));
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        wxBitmap bmp = create_scaled_bitmap(icon_name, this, 18);
        wxImage img = bmp.ConvertToImage();
        if (img.IsOk()) {
            for (int x = 0; x < img.GetWidth(); x++)
                for (int y = 0; y < img.GetHeight(); y++)
                    img.SetRGB(x, y, 255, 255, 255);
            bmp = wxBitmap(img);
        }
        lbl->Reparent(row_panel);
        row->AddStretchSpacer(1);
        row->Add(new wxStaticBitmap(row_panel, wxID_ANY, bmp),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, this->FromDIP(5));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row_panel->SetSizer(row);
        return row_panel;
    };
    auto add_ps_title_strip = [this, ps_card_header_bg, &make_ps_header](wxBoxSizer *card_sizer, wxWindow *card, const wxString &title, bool active) -> wxStaticText * {
        const int border_inset = this->FromDIP(1);
        const double hdr_corner_r = this->FromDIP(8);
        auto *header_panel = new PsCardHeaderPanel(card, ps_card_header_bg, hdr_corner_r);
        auto *header_sz = new wxBoxSizer(wxVERTICAL);
        auto *header_label = make_ps_header(header_panel, title, active);
        header_sz->Add(header_label, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(8));
        // Former gap before separator — now inside the dark header so fill runs to the line.
        header_sz->AddSpacer(FromDIP(6));
        header_panel->SetSizer(header_sz);
        // Keep a tiny inset so the selected card's green border remains visible
        // around the whole tool button instead of being covered by the header panel.
        card_sizer->Add(header_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border_inset);
        return header_label;
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
        m_ps_tool_cards[i] = card;
        auto *card_sizer = new wxBoxSizer(wxVERTICAL);
        auto *header_label = add_ps_title_strip(card_sizer, card, tc.name, tc.active);
        m_ps_tool_headers[i] = header_label;
        {
            auto *sep = new wxPanel(card, wxID_ANY);
            sep->SetMinSize(wxSize(-1, FromDIP(1)));
            sep->SetMaxSize(wxSize(-1, FromDIP(1)));
            sep->SetBackgroundColour(wxColour(55, 58, 64));
            card_sizer->Add(sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
        }

        auto *temp_lbl = make_ps_value(card, "-- / --", tc.active);
        *tc.temp = temp_lbl;
        temp_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        const int nozzle_idx = i;
        temp_lbl->Bind(wxEVT_LEFT_DOWN, [this, nozzle_idx](wxMouseEvent &) {
            apply_printer_status_tool_selection(nozzle_idx);
            prompt_ps_target_temperature(false, nozzle_idx);
        });
        card_sizer->AddSpacer(FromDIP(8));
        card_sizer->Add(make_icon_row(card, "tool_temperature_white", temp_lbl), 0,
            wxEXPAND | wxLEFT | wxRIGHT, FromDIP(1));

        auto *fan_lbl = make_ps_value(card, "--%", tc.active);
        *tc.fan = fan_lbl;
        card_sizer->AddSpacer(FromDIP(13));
        card_sizer->Add(make_icon_row(card, "tool_fan_white", fan_lbl), 0,
            wxEXPAND | wxLEFT | wxRIGHT, FromDIP(1));
        card_sizer->AddSpacer(FromDIP(13));
        card->SetCursor(wxCursor(wxCURSOR_HAND));
        header_label->SetCursor(wxCursor(wxCURSOR_HAND));
        card->Bind(wxEVT_LEFT_DOWN, [this, nozzle_idx](wxMouseEvent &) {
            apply_printer_status_tool_selection(nozzle_idx);
        });
        header_label->Bind(wxEVT_LEFT_DOWN, [this, nozzle_idx](wxMouseEvent &) {
            apply_printer_status_tool_selection(nozzle_idx);
        });
        fan_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        fan_lbl->Bind(wxEVT_LEFT_DOWN, [this, nozzle_idx](wxMouseEvent &) {
            apply_printer_status_tool_selection(nozzle_idx);
        });

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
            card_sizer->Add(sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
        }

        auto *bed_temp_lbl = make_ps_value(card, "-- / --", false);
        m_ps_bed_temp_label = bed_temp_lbl;
        bed_temp_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        bed_temp_lbl->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { prompt_ps_target_temperature(true, 0); });
        card_sizer->AddSpacer(FromDIP(13));
        card_sizer->Add(make_icon_row(card, "bed_heating", bed_temp_lbl), 0,
            wxEXPAND | wxLEFT | wxRIGHT, FromDIP(1));
        card_sizer->AddSpacer(FromDIP(13));

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

    content_columns->Add(left_main_column, 46, wxEXPAND);
    content_columns->AddSpacer(FromDIP(20));
    content_columns->Add(right_main_column, 54, wxEXPAND | wxRIGHT, FromDIP(5));

    left_sizer->Add(content_columns, 0, wxEXPAND);
    left_sizer->AddSpacer(FromDIP(52));

    left_container->SetSizer(left_sizer);

    status_content_sizer->Add(left_container, 1, wxEXPAND | wxRIGHT, FromDIP(20));
    status_content->SetSizer(status_content_sizer);
    status_content->Bind(wxEVT_SIZE, [status_content, left_container, last_width = -1](wxSizeEvent &event) mutable {
        event.Skip();
        if (status_content == nullptr || left_container == nullptr)
            return;
        const int width = status_content->GetClientSize().GetWidth();
        if (width <= 0 || width == last_width)
            return;
        last_width = width;
        left_container->SetMinSize(wxSize(width, -1));
        status_content->Layout();
    });
    status_page_sizer->Add(status_content, 1, wxEXPAND);
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
    m_layer_refresh_timer->Start(1000);
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
    dismiss_filament_tool_popup();
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
    m_selected_extruder_index = 0;
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
    dismiss_filament_tool_popup();
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

bool PrinterWebView::finish_add_moonraker_printer(const BBLocalMachine &machine, bool use_ssl)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager == nullptr) {
        wxMessageBox("Device manager hazir degil.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }

    auto *agent = wxGetApp().getAgent();
    if (agent == nullptr) {
        wxMessageBox("Network agent hazir degil.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }

    const auto current_printer_agent = agent->get_printer_agent();
    const bool needs_moonraker_agent = current_printer_agent == nullptr ||
        current_printer_agent->get_agent_info().id != "moonraker";
    if (needs_moonraker_agent) {
        auto moonraker_agent = NetworkAgentFactory::create_printer_agent_by_id(
            "moonraker", agent->get_cloud_agent(), Slic3r::data_dir());
        if (moonraker_agent == nullptr) {
            wxMessageBox("Moonraker network agent baslatilamadi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
            return false;
        }
        agent->set_printer_agent(moonraker_agent);
    }

    auto normalize_host = [](std::string value) {
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        trim(value);
        const auto scheme_pos = value.find("://");
        if (scheme_pos != std::string::npos)
            value = value.substr(scheme_pos + 3);
        const auto slash_pos = value.find('/');
        if (slash_pos != std::string::npos)
            value = value.substr(0, slash_pos);
        if (std::count(value.begin(), value.end(), ':') == 1) {
            const auto colon_pos = value.rfind(':');
            if (colon_pos != std::string::npos)
                value = value.substr(0, colon_pos);
        }
        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };
    if (wxGetApp().app_config != nullptr) {
        const std::string host = normalize_host(!machine.dev_ip.empty() ? machine.dev_ip : machine.dev_id);
        if (!host.empty())
            wxGetApp().app_config->erase("forgotten_lan_machines", host);
    }

    MachineObject *obj = dev_manager->insert_local_device(machine, "lan", "free", "", "");
    if (obj == nullptr) {
        wxMessageBox("Yazici yerel cihaz listesine eklenemedi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }

    obj->local_use_ssl = use_ssl;
    if (!dev_manager->set_selected_machine(machine.dev_id)) {
        wxMessageBox("Yazici secilemedi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }
    m_has_active_printer_connection = true;
    obj->command_request_push_all(true);

    dismiss_printers_popup();
    rebuild_printers_popup();
    refresh_layer_info_from_selected_machine();
    Layout();
    return true;
}

void PrinterWebView::show_printer_card_actions_menu(wxWindow *anchor, MachineObject *machine)
{
    if (anchor == nullptr || machine == nullptr)
        return;

    auto *dev_manager = wxGetApp().getDeviceManager();
    const auto local_machines = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();
    const bool can_remove = local_machines.find(machine->get_dev_id()) != local_machines.end();

    wxMenu menu;
    menu.Append(1, _L("Edit printer"));
    if (can_remove) {
        auto *forget_item = new wxMenuItem(&menu, 2, _L("Forget printer"));
        forget_item->SetBitmap(create_scaled_bitmap("device_sidebar_bin", this, 16));
        menu.Append(forget_item);
    }

    const wxPoint screen_pos = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().GetHeight()));
    const int sel = GetPopupMenuSelectionFromUser(menu, screen_pos);
    if (sel == 1) {
        wxTextEntryDialog dlg(this, _L("Printer display name"), _L("Edit printer"), from_u8(machine->get_dev_name()));
        if (dlg.ShowModal() != wxID_OK)
            return;
        wxString v = dlg.GetValue();
        v.Trim(true);
        v.Trim(false);
        if (v.empty())
            return;
        const std::string new_name = into_u8(v);
        if (machine->is_lan_mode_printer()) {
            machine->set_dev_name(new_name);
            DeviceManager::update_local_machine(*machine);
        } else if (dev_manager != nullptr) {
            dev_manager->modify_device_name(machine->get_dev_id(), new_name);
            machine->set_dev_name(new_name);
        } else {
            machine->set_dev_name(new_name);
        }
        rebuild_printers_popup();
        refresh_layer_info_from_selected_machine();
        Layout();
    } else if (sel == 2 && can_remove) {
        if (confirm_forget_printer())
            forget_local_printer(machine);
    }
}

bool PrinterWebView::confirm_forget_printer()
{
    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(wxColour("#232527"));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *frame = new StaticBox(&dlg, wxID_ANY);
    frame->SetCornerRadius(FromDIP(12));
    frame->SetBorderWidth(1);
    frame->SetBorderColorNormal(wxColour("#3A3D40"));
    frame->SetBackgroundColorNormal(wxColour("#232527"));
    frame->SetBackgroundColour(wxColour("#232527"));

    auto *content = new wxBoxSizer(wxVERTICAL);
    auto *title = new wxStaticText(frame, wxID_ANY, _L("Are you sure to delete this printer?"));
    title->SetForegroundColour(wxColour("#F1F3F4"));
    wxFont tf = title->GetFont();
    tf.SetPointSize(11);
    tf.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(tf);
    content->Add(title, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, FromDIP(20));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *cancel = new Button(frame, _L("Cancel"));
    cancel->SetMinSize(wxSize(FromDIP(88), FromDIP(34)));
    auto *forget = new Button(frame, _L("Forget"));
    forget->SetMinSize(wxSize(FromDIP(88), FromDIP(34)));
    forget->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    buttons->Add(cancel, 0);
    buttons->AddSpacer(FromDIP(10));
    buttons->Add(forget, 0);
    content->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(20));

    frame->SetSizer(content);
    root->Add(frame, 1, wxEXPAND);
    dlg.SetSizerAndFit(root);
    dlg.CentreOnParent();

    cancel->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    forget->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_OK); });
    return dlg.ShowModal() == wxID_OK;
}

void PrinterWebView::forget_local_printer(MachineObject *machine)
{
    if (machine == nullptr)
        return;

    auto *dev_manager = wxGetApp().getDeviceManager();
    const std::string dev_id = machine->get_dev_id();
    const std::string dev_ip = machine->get_dev_ip();

    auto normalize_host = [](std::string value) {
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };

        trim(value);
        const auto scheme_pos = value.find("://");
        if (scheme_pos != std::string::npos)
            value = value.substr(scheme_pos + 3);

        const auto slash_pos = value.find('/');
        if (slash_pos != std::string::npos)
            value = value.substr(0, slash_pos);

        const auto question_pos = value.find('?');
        if (question_pos != std::string::npos)
            value = value.substr(0, question_pos);

        const auto at_pos = value.find('@');
        if (at_pos != std::string::npos)
            value = value.substr(at_pos + 1);

        const auto colon_count = std::count(value.begin(), value.end(), ':');
        if (colon_count == 1) {
            const auto colon_pos = value.rfind(':');
            if (colon_pos != std::string::npos)
                value = value.substr(0, colon_pos);
        }

        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };

    auto same_printer = [&](const std::string &key, const std::string &entry_dev_id, const std::string &entry_dev_ip) {
        if ((!dev_id.empty() && (key == dev_id || entry_dev_id == dev_id || entry_dev_ip == dev_id)) ||
            (!dev_ip.empty() && (key == dev_ip || entry_dev_id == dev_ip || entry_dev_ip == dev_ip)))
            return true;

        const std::string target_host = !dev_ip.empty() ? normalize_host(dev_ip) : normalize_host(dev_id);
        if (target_host.empty())
            return false;

        return normalize_host(key) == target_host ||
               normalize_host(entry_dev_id) == target_host ||
               normalize_host(entry_dev_ip) == target_host;
    };

    MachineObject *selected_machine = dev_manager != nullptr ? dev_manager->get_selected_machine() : nullptr;
    if (selected_machine != nullptr &&
        same_printer(selected_machine->get_dev_id(), selected_machine->get_dev_id(), selected_machine->get_dev_ip())) {
        selected_machine->disconnect();
        selected_machine->set_online_state(false);
        selected_machine->reset();
        m_has_active_printer_connection = false;
        if (dev_manager != nullptr)
            dev_manager->set_selected_machine("");
    }

    std::vector<std::string> keys_to_erase;
    auto add_key = [&keys_to_erase](const std::string &key) {
        if (!key.empty() && std::find(keys_to_erase.begin(), keys_to_erase.end(), key) == keys_to_erase.end())
            keys_to_erase.push_back(key);
    };
    add_key(dev_id);
    add_key(dev_ip);

    if (wxGetApp().app_config != nullptr) {
        const std::string forgotten_host = normalize_host(!dev_ip.empty() ? dev_ip : dev_id);
        if (!forgotten_host.empty())
            wxGetApp().app_config->set("forgotten_lan_machines", forgotten_host, std::string("1"));
        const auto saved_machines = wxGetApp().app_config->get_local_machines();
        for (const auto &entry : saved_machines) {
            const BBLocalMachine &local = entry.second;
            if (same_printer(entry.first, local.dev_id, local.dev_ip))
                add_key(entry.first);
        }
    }

    std::vector<MachineObject *> objects_to_delete;
    if (dev_manager != nullptr) {
        const auto local_machines = dev_manager->get_local_machinelist();
        for (const auto &entry : local_machines) {
            MachineObject *local = entry.second;
            if (local != nullptr && same_printer(entry.first, local->get_dev_id(), local->get_dev_ip())) {
                add_key(entry.first);
                if (std::find(objects_to_delete.begin(), objects_to_delete.end(), local) == objects_to_delete.end())
                    objects_to_delete.push_back(local);
            }
        }
    }

    for (const std::string &key : keys_to_erase) {
        if (wxGetApp().app_config != nullptr)
            wxGetApp().app_config->erase_local_machine(key);
        if (dev_manager != nullptr)
            dev_manager->erase_local_machine(key);
    }

    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->save();

    for (MachineObject *obj : objects_to_delete) {
        if (obj != nullptr) {
            obj->disconnect();
            delete obj;
        }
    }

    rebuild_printers_popup();
    rebuild_sidebar_printer_list();
    refresh_layer_info_from_selected_machine();
    Layout();
}

void PrinterWebView::show_add_printer_dialog()
{
    dismiss_printers_popup();

    struct AddPrinterDialog : public wxDialog
    {
        void apply_tab_selection(int idx)
        {
            m_tab_index = idx;
            const wxColour k_accent(40, 167, 69);
            for (int i = 0; i < 3; ++i) {
                if (m_tab_lbl[i] == nullptr || m_tab_under[i] == nullptr)
                    continue;
                const bool on = (i == idx);
                m_tab_lbl[i]->SetForegroundColour(on ? k_accent : wxColour(72, 72, 78));
                wxFont f = m_tab_lbl[i]->GetFont();
                f.SetWeight(on ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
                m_tab_lbl[i]->SetFont(f);
                m_tab_under[i]->SetBackgroundColour(on ? k_accent : wxColour(255, 255, 255));
            }
            if (m_book != nullptr)
                m_book->ChangeSelection(static_cast<size_t>(idx));
            Refresh();
        }

        explicit AddPrinterDialog(wxWindow *parent, PrinterWebView *owner)
            : wxDialog(parent, wxID_ANY, _L("Add Printer"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
            , m_owner(owner)
        {
            SetBackgroundColour(wxColour(255, 255, 255));
            const wxColour k_accent(40, 167, 69);
            auto *root = new wxBoxSizer(wxVERTICAL);

            auto *header = new wxBoxSizer(wxHORIZONTAL);
            auto *close_btn = new wxButton(this, wxID_CANCEL, "X", wxDefaultPosition, wxSize(FromDIP(28), FromDIP(28)));
            close_btn->SetToolTip(_L("Close"));
            header->Add(close_btn, 0, wxLEFT | wxTOP, FromDIP(10));
            auto *title = new wxStaticText(this, wxID_ANY, _L("Add Printer"));
            wxFont tf = title->GetFont();
            tf.SetPointSize(tf.GetPointSize() + 1);
            tf.SetWeight(wxFONTWEIGHT_BOLD);
            title->SetFont(tf);
            title->SetForegroundColour(wxColour(28, 28, 28));
            header->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxALIGN_CENTER_HORIZONTAL);
            header->AddSpacer(FromDIP(38));
            root->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
            auto *header_line = new wxPanel(this, wxID_ANY);
            header_line->SetMinSize(wxSize(-1, FromDIP(1)));
            header_line->SetMaxSize(wxSize(-1, FromDIP(1)));
            header_line->SetBackgroundColour(wxColour(232, 232, 235));
            root->Add(header_line, 0, wxEXPAND);

            m_tab_area = new wxPanel(this, wxID_ANY);
            m_tab_area->SetBackgroundColour(wxColour(255, 255, 255));
            auto *tab_hs = new wxBoxSizer(wxHORIZONTAL);
            const wxString tab_titles[3] = { _L("Auto Connect"), _L("IP Connect"), _L("Manual Setup") };
            for (int i = 0; i < 3; ++i) {
                auto *cell = new wxPanel(m_tab_area, wxID_ANY);
                cell->SetBackgroundColour(wxColour(255, 255, 255));
                auto *vs = new wxBoxSizer(wxVERTICAL);
                m_tab_lbl[i] = new wxStaticText(cell, wxID_ANY, tab_titles[i]);
                m_tab_lbl[i]->SetCursor(wxCursor(wxCURSOR_HAND));
                vs->Add(m_tab_lbl[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
                m_tab_under[i] = new wxPanel(cell, wxID_ANY);
                m_tab_under[i]->SetMinSize(wxSize(-1, FromDIP(2)));
                m_tab_under[i]->SetMaxSize(wxSize(-1, FromDIP(2)));
                vs->Add(m_tab_under[i], 0, wxEXPAND | wxTOP, FromDIP(6));
                cell->SetSizer(vs);
                const int idx = i;
                auto pick_tab = [this, idx](wxMouseEvent &) { apply_tab_selection(idx); };
                cell->Bind(wxEVT_LEFT_DOWN, pick_tab);
                m_tab_lbl[i]->Bind(wxEVT_LEFT_DOWN, pick_tab);
                tab_hs->Add(cell, 1, wxEXPAND);
            }
            m_tab_area->SetSizer(tab_hs);
            root->Add(m_tab_area, 0, wxEXPAND | wxLEFT | wxRIGHT);

            m_book = new wxSimplebook(this, wxID_ANY);
            m_book->SetBackgroundColour(wxColour(255, 255, 255));

            m_auto_page = new wxPanel(m_book, wxID_ANY);
            m_auto_page->SetBackgroundColour(wxColour(255, 255, 255));
            auto *auto_sz = new wxBoxSizer(wxVERTICAL);
            auto *search_row = new wxBoxSizer(wxHORIZONTAL);
            m_auto_status = new wxStaticText(m_auto_page, wxID_ANY, _L("Searching for printers on your network..."));
            search_row->Add(m_auto_status, 1, wxALIGN_CENTER_VERTICAL);
            auto *refresh_btn = new wxButton(m_auto_page, wxID_ANY, _L("Refresh"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            search_row->Add(refresh_btn, 0, wxALIGN_CENTER_VERTICAL);
            auto_sz->Add(search_row, 0, wxEXPAND | wxALL, FromDIP(10));
            m_auto_list = new wxScrolledWindow(m_auto_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
            m_auto_list->SetScrollRate(FromDIP(8), FromDIP(8));
            m_auto_list->SetBackgroundColour(wxColour(255, 255, 255));
            m_auto_list_sizer = new wxBoxSizer(wxVERTICAL);
            m_auto_list->SetSizer(m_auto_list_sizer);
            auto_sz->Add(m_auto_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
            auto_sz->Add(build_hint_panel(m_auto_page, _L("Make sure your printer is powered on and connected to the same network."), false, true), 0,
                wxEXPAND | wxALL, FromDIP(10));
            auto_sz->Add(build_hint_panel(m_auto_page, _L("Can't find your printer? Try IP Connect or Manual Setup."), true, false), 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
            m_auto_page->SetSizer(auto_sz);

            m_ip_page = new wxPanel(m_book, wxID_ANY);
            m_ip_page->SetBackgroundColour(wxColour(255, 255, 255));
            auto *ip_sz = new wxBoxSizer(wxVERTICAL);
            ip_sz->Add(new wxStaticText(m_ip_page, wxID_ANY, _L("Enter your printer's IP address")), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

            auto *ip_shell = new StaticBox(m_ip_page, wxID_ANY);
            ip_shell->SetCornerRadius(static_cast<double>(FromDIP(10)));
            ip_shell->SetBorderWidth(FromDIP(1));
            ip_shell->SetBorderColorNormal(wxColour(210, 210, 215));
            ip_shell->SetBackgroundColorNormal(wxColour(255, 255, 255));
            ip_shell->SetBackgroundColour(wxColour(255, 255, 255));
            auto *ip_inner = new wxBoxSizer(wxHORIZONTAL);
            m_ip_field = new wxTextCtrl(ip_shell, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            m_ip_field->SetBackgroundColour(wxColour(255, 255, 255));
            m_ip_field->SetHint(_L("Type IP address..."));
            ip_inner->Add(m_ip_field, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));
            auto *ip_add = new wxButton(ip_shell, wxID_ANY, _L("Add"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            ip_add->SetBackgroundColour(wxColour(232, 246, 238));
            ip_add->SetForegroundColour(k_accent);
            ip_inner->Add(ip_add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            ip_shell->SetSizer(ip_inner);
            ip_sz->Add(ip_shell, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

            auto *ip_stat_row = new wxBoxSizer(wxHORIZONTAL);
            m_ip_status = new wxStaticText(m_ip_page, wxID_ANY, wxString());
            m_ip_status->SetForegroundColour(wxColour(72, 72, 76));
            ip_stat_row->Add(m_ip_status, 0, wxALIGN_CENTER_VERTICAL);
            ip_stat_row->AddStretchSpacer(1);
            auto *ip_refresh = new wxButton(m_ip_page, wxID_ANY, wxString::FromUTF8("\xE2\x86\xBB"), wxDefaultPosition, wxSize(FromDIP(32), FromDIP(28)), wxBU_EXACTFIT);
            ip_refresh->SetToolTip(_L("Retry"));
            ip_stat_row->Add(ip_refresh, 0, wxALIGN_CENTER_VERTICAL);
            ip_sz->Add(ip_stat_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
            ip_sz->Add(build_hint_panel(m_ip_page, _L("Make sure your printer is powered on and connected to the same network."), false, true), 0,
                wxEXPAND | wxALL, FromDIP(10));
            m_ip_page->SetSizer(ip_sz);

            m_manual_page = new wxPanel(m_book, wxID_ANY);
            m_manual_page->SetBackgroundColour(wxColour(255, 255, 255));
            auto *man_sz = new wxBoxSizer(wxVERTICAL);
            man_sz->Add(new wxStaticText(m_manual_page, wxID_ANY,
                              _L("Manual setup opens the classic IP and name dialogs step by step.")),
                0, wxALL, FromDIP(12));
            auto *open_manual = new wxButton(m_manual_page, wxID_ANY, _L("Start manual setup"));
            man_sz->Add(open_manual, 0, wxLEFT | wxRIGHT, FromDIP(12));
            man_sz->AddStretchSpacer(1);
            m_manual_page->SetSizer(man_sz);

            m_book->AddPage(m_auto_page, wxString(), false);
            m_book->AddPage(m_ip_page, wxString(), false);
            m_book->AddPage(m_manual_page, wxString(), false);
            root->Add(m_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

            auto add_footer_row = [&](PrinterWebViewTab tab, const wxString &label, wxArtID art_id) {
                auto *row = new wxPanel(this, wxID_ANY);
                row->SetBackgroundColour(wxColour(246, 247, 248));
                row->SetCursor(wxCursor(wxCURSOR_HAND));
                row->SetMinSize(wxSize(-1, FromDIP(44)));
                auto *r = new wxBoxSizer(wxHORIZONTAL);
                r->AddSpacer(FromDIP(12));
                wxBitmap ib = wxArtProvider::GetBitmap(art_id, wxART_CMN_DIALOG, wxSize(FromDIP(20), FromDIP(20)));
                if (ib.IsOk())
                    r->Add(new wxStaticBitmap(row, wxID_ANY, ib), 0, wxALIGN_CENTER_VERTICAL);
                else
                    r->AddSpacer(FromDIP(20));
                r->AddSpacer(FromDIP(8));
                auto *lbl = new wxStaticText(row, wxID_ANY, label);
                lbl->SetForegroundColour(wxColour(40, 40, 44));
                lbl->SetCursor(wxCursor(wxCURSOR_HAND));
                r->Add(lbl, 1, wxALIGN_CENTER_VERTICAL);
                auto *ch = new wxStaticText(row, wxID_ANY, ">");
                ch->SetForegroundColour(wxColour(130, 130, 130));
                ch->SetCursor(wxCursor(wxCURSOR_HAND));
                r->Add(ch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
                row->SetSizer(r);
                auto go = [this, owner, tab](wxMouseEvent &) {
                    EndModal(wxID_OK);
                    if (owner != nullptr)
                        owner->CallAfter([owner, tab]() { owner->select_tab(tab); });
                };
                row->Bind(wxEVT_LEFT_DOWN, go);
                lbl->Bind(wxEVT_LEFT_DOWN, go);
                ch->Bind(wxEVT_LEFT_DOWN, go);
                root->Add(row, 0, wxEXPAND | wxTOP, FromDIP(2));
            };
            add_footer_row(PrinterWebViewTab::Update, _L("System Upgrade"), wxART_FILE_SAVE);
            add_footer_row(PrinterWebViewTab::Storage, _L("Media"), wxART_FOLDER_OPEN);

            SetSizer(root);

            refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { rebuild_auto_list(); });
            ip_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { try_ip_add(); });
            m_ip_field->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { try_ip_add(); });
            ip_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { try_ip_add(); });
            open_manual->Bind(wxEVT_BUTTON, [this, owner](wxCommandEvent &) {
                EndModal(wxID_OK);
                if (owner != nullptr)
                    owner->CallAfter([owner]() { owner->prompt_ip_connect(); });
            });

            SetSize(wxSize(FromDIP(380), FromDIP(480)));
            rebuild_auto_list();
            apply_tab_selection(1);
        }

        wxPanel *build_hint_panel(wxWindow *parent, const wxString &text, bool green_bg, bool with_info_icon = false)
        {
            auto *p = new wxPanel(parent, wxID_ANY);
            p->SetBackgroundColour(green_bg ? wxColour(220, 248, 230) : wxColour(240, 240, 242));
            auto *s = new wxBoxSizer(wxHORIZONTAL);
            s->AddSpacer(FromDIP(8));
            if (with_info_icon) {
                wxBitmap tip = wxArtProvider::GetBitmap(wxART_INFORMATION, wxART_CMN_DIALOG, wxSize(FromDIP(18), FromDIP(18)));
                if (tip.IsOk())
                    s->Add(new wxStaticBitmap(p, wxID_ANY, tip), 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(8));
                else
                    s->AddSpacer(FromDIP(18));
                s->AddSpacer(FromDIP(6));
            }
            auto *t = new wxStaticText(p, wxID_ANY, text);
            t->SetForegroundColour(wxColour(55, 55, 58));
            t->Wrap(FromDIP(300));
            s->Add(t, 1, wxALL, FromDIP(10));
            p->SetSizer(s);
            return p;
        }

        void rebuild_auto_list()
        {
            if (m_auto_list_sizer == nullptr || m_owner == nullptr)
                return;
            m_auto_list_sizer->Clear(true);
            auto *dev_manager = wxGetApp().getDeviceManager();
            if (dev_manager != nullptr)
                dev_manager->start_refresher();

            const auto locals = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();
            const wxColour green(40, 167, 69);
            for (const auto &it : locals) {
                MachineObject *obj = it.second;
                if (obj == nullptr)
                    continue;
                auto *row = new wxPanel(m_auto_list, wxID_ANY);
                row->SetBackgroundColour(wxColour(255, 255, 255));
                row->SetCursor(wxCursor(wxCURSOR_HAND));
                auto *hs = new wxBoxSizer(wxHORIZONTAL);
                hs->AddSpacer(FromDIP(8));
                hs->Add(new wxStaticBitmap(row, wxID_ANY, create_scaled_bitmap(k_cprint_printer_nav_bitmap, m_owner, 20)), 0, wxALIGN_CENTER_VERTICAL);
                hs->AddSpacer(FromDIP(8));
                auto *vs = new wxBoxSizer(wxVERTICAL);
                auto *name = new wxStaticText(row, wxID_ANY, from_u8(obj->get_dev_name()));
                wxFont nf = name->GetFont();
                nf.SetWeight(wxFONTWEIGHT_BOLD);
                name->SetFont(nf);
                vs->Add(name, 0);
                wxString ip = from_u8(obj->get_dev_ip());
                if (!ip.empty())
                    vs->Add(new wxStaticText(row, wxID_ANY, ip), 0);
                hs->Add(vs, 1, wxALIGN_CENTER_VERTICAL);
                auto *add_b = new wxButton(row, wxID_ANY, _L("Add"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                add_b->SetForegroundColour(green);
                hs->Add(add_b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
                row->SetSizer(hs);
                add_b->Bind(wxEVT_BUTTON, [this, obj](wxCommandEvent &) {
                    BBLocalMachine machine;
                    machine.dev_id = obj->get_dev_id();
                    machine.dev_ip = obj->get_dev_ip();
                    machine.dev_name = obj->get_dev_name();
                    machine.printer_type = obj->printer_type.empty() ? std::string("Moonraker") : obj->printer_type;
                    if (m_owner != nullptr && m_owner->finish_add_moonraker_printer(machine, obj->local_use_ssl))
                        EndModal(wxID_OK);
                });
                m_auto_list_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
            }
            if (locals.empty()) {
                auto *empty = new wxStaticText(m_auto_list, wxID_ANY, _L("No printers discovered yet. Tap Refresh."));
                m_auto_list_sizer->Add(empty, 0, wxALL, FromDIP(8));
            }
            m_auto_list->FitInside();
            m_auto_list->Layout();
        }

        void try_ip_add()
        {
            if (m_ip_status != nullptr)
                m_ip_status->SetLabelText(wxString());
            wxString ip_value = m_ip_field->GetValue();
            ip_value.Trim(true);
            ip_value.Trim(false);
            if (ip_value.empty()) {
                wxMessageBox(_L("IP address cannot be empty."), _L("IP Connect"), wxOK | wxICON_WARNING, this);
                return;
            }

            wxTextEntryDialog name_dialog(this, _L("Display name for this printer"), _L("Printer name"), _L("Unknown Printer"));
            if (name_dialog.ShowModal() != wxID_OK)
                return;
            wxString name_value = name_dialog.GetValue();
            name_value.Trim(true);
            name_value.Trim(false);
            if (name_value.empty()) {
                wxMessageBox(_L("Printer name cannot be empty."), _L("Printer name"), wxOK | wxICON_WARNING, this);
                return;
            }

            std::string host = into_u8(ip_value);
            const bool has_scheme = host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0;
            const std::string normalized_host = MachineObject::dev_id_from_address(host);
            std::string dev_ip = normalized_host;
            if (!has_scheme && normalized_host.find(':') == std::string::npos)
                dev_ip += ":7125";
            const std::string dev_id = dev_ip;

            BBLocalMachine machine;
            machine.dev_id = dev_id;
            machine.dev_ip = dev_ip;
            machine.dev_name = into_u8(name_value);
            machine.printer_type = "Moonraker";

            if (m_ip_status != nullptr)
                m_ip_status->SetLabelText(_L("Connecting to printer..."));
            if (m_owner != nullptr && m_owner->finish_add_moonraker_printer(machine, host.rfind("https://", 0) == 0))
                EndModal(wxID_OK);
        }

        PrinterWebView *m_owner{ nullptr };
        wxSimplebook *m_book{ nullptr };
        wxPanel *m_tab_area{ nullptr };
        wxStaticText *m_tab_lbl[3]{ nullptr, nullptr, nullptr };
        wxPanel *m_tab_under[3]{ nullptr, nullptr, nullptr };
        int m_tab_index{ 0 };
        wxPanel *m_auto_page{ nullptr };
        wxPanel *m_ip_page{ nullptr };
        wxPanel *m_manual_page{ nullptr };
        wxStaticText *m_auto_status{ nullptr };
        wxScrolledWindow *m_auto_list{ nullptr };
        wxBoxSizer *m_auto_list_sizer{ nullptr };
        wxTextCtrl *m_ip_field{ nullptr };
        wxStaticText *m_ip_status{ nullptr };
    };

    AddPrinterDialog dlg(this, this);
    dlg.ShowModal();
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

    wxString default_name = _L("Unknown Printer");
    wxTextEntryDialog name_dialog(this, "Bu yazici icin gorunecek bir isim belirleyin.", "Yazici Adi", default_name);
    if (name_dialog.ShowModal() != wxID_OK)
        return;

    wxString name_value = name_dialog.GetValue();
    name_value.Trim(true);
    name_value.Trim(false);
    if (name_value.empty()) {
        wxMessageBox("Yazici adi bos olamaz.", "Yazici Adi", wxOK | wxICON_WARNING, this);
        return;
    }

    BBLocalMachine machine;
    machine.dev_id = dev_id;
    machine.dev_ip = dev_ip;
    machine.dev_name = into_u8(name_value);
    machine.printer_type = "Moonraker";

    finish_add_moonraker_printer(machine, host.rfind("https://", 0) == 0);
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

    m_printers_popup_panel->SetBackgroundColour(wxColour(255, 255, 255));
    m_printers_popup_panel->SetMinSize(wxSize(FromDIP(288), -1));

    auto *printers_popup_sizer = new wxBoxSizer(wxVERTICAL);
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *selected_machine = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const auto my_machines = dev_manager ? dev_manager->get_my_machine_list() : std::map<std::string, MachineObject*>();
    const auto local_machines = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();

    const wxColour k_green(40, 167, 69);
    const wxColour k_muted(120, 120, 120);
    const wxColour k_card_border(232, 232, 232);

    printers_popup_sizer->AddSpacer(FromDIP(10));

    auto select_machine_fn = [this, dev_manager](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const std::string dev_id = machine->get_dev_id();
        dismiss_printers_popup();
        if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->m_monitor != nullptr)
            wxGetApp().mainframe->m_monitor->select_machine(dev_id);
        else if (dev_manager != nullptr)
            dev_manager->set_selected_machine(dev_id);
        m_has_active_printer_connection = true;
        machine->command_request_push_all(true);
        refresh_layer_info_from_selected_machine();
    };

    std::map<std::string, MachineObject*> all_by_id;
    for (const auto &entry : my_machines) {
        if (entry.second != nullptr)
            all_by_id[entry.first] = entry.second;
    }
    for (const auto &entry : local_machines) {
        if (entry.second != nullptr && all_by_id.find(entry.first) == all_by_id.end())
            all_by_id[entry.first] = entry.second;
    }
    if (selected_machine != nullptr)
        all_by_id[selected_machine->get_dev_id()] = selected_machine;

    std::vector<MachineObject *> sorted;
    sorted.reserve(all_by_id.size());
    for (const auto &entry : all_by_id) {
        if (entry.second != nullptr)
            sorted.push_back(entry.second);
    }
    std::sort(sorted.begin(), sorted.end(), [](MachineObject *a, MachineObject *b) {
        if (a == nullptr || b == nullptr)
            return a != nullptr;
        return a->get_dev_name() < b->get_dev_name();
    });

    std::vector<MachineObject *> online_list;
    std::vector<MachineObject *> offline_list;
    for (MachineObject *m : sorted) {
        if (m == nullptr)
            continue;
        if (m->is_online())
            online_list.push_back(m);
        else
            offline_list.push_back(m);
    }

    auto add_section_title = [&](const wxString &text) {
        auto *lab = new wxStaticText(m_printers_popup_panel, wxID_ANY, text);
        lab->SetForegroundColour(k_muted);
        wxFont f = lab->GetFont();
        f.SetPointSize((std::max)(8, f.GetPointSize() - 1));
        lab->SetFont(f);
        printers_popup_sizer->Add(lab, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
    };

    auto add_printer_card = [&](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const bool online = machine->is_online();
        auto *card = new StaticBox(m_printers_popup_panel, wxID_ANY);
        card->SetCornerRadius(static_cast<double>(FromDIP(10)));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(k_card_border);
        card->SetBackgroundColorNormal(wxColour(255, 255, 255));
        card->SetBackgroundColour(wxColour(255, 255, 255));
        card->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *hs = new wxBoxSizer(wxHORIZONTAL);
        hs->AddSpacer(FromDIP(10));
        auto *printer_bmp = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap(k_cprint_printer_nav_bitmap, this, 22));
        hs->Add(printer_bmp, 0, wxALIGN_CENTER_VERTICAL);
        hs->AddSpacer(FromDIP(10));
        auto *vs = new wxBoxSizer(wxVERTICAL);
        auto *name_lbl = new wxStaticText(card, wxID_ANY, from_u8(machine->get_dev_name()), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        wxFont nf = name_lbl->GetFont();
        nf.SetWeight(wxFONTWEIGHT_BOLD);
        name_lbl->SetFont(nf);
        name_lbl->SetForegroundColour(wxColour(28, 28, 28));
        vs->Add(name_lbl, 0);
        auto *status_row = new wxBoxSizer(wxHORIZONTAL);
        auto *dot = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F"));
        dot->SetForegroundColour(online ? k_green : wxColour(180, 180, 180));
        status_row->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        auto *st = new wxStaticText(card, wxID_ANY, online ? _L("Connected") : _L("Offline"));
        st->SetForegroundColour(online ? k_green : k_muted);
        status_row->Add(st, 0, wxALIGN_CENTER_VERTICAL);
        vs->Add(status_row, 0);
        hs->Add(vs, 1, wxALIGN_CENTER_VERTICAL);
        auto *more = new wxStaticText(card, wxID_ANY, "...");
        more->SetForegroundColour(k_muted);
        more->SetCursor(wxCursor(wxCURSOR_HAND));
        hs->Add(more, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        auto *chev = new wxStaticText(card, wxID_ANY, ">");
        chev->SetForegroundColour(wxColour(160, 160, 160));
        chev->SetCursor(wxCursor(wxCURSOR_HAND));
        hs->Add(chev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        auto *card_outer = new wxBoxSizer(wxVERTICAL);
        // Extra vertical padding (+30px at 96 DPI via FromDIP) for taller cards
        card_outer->Add(hs, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10) + FromDIP(15));
        card->SetSizer(card_outer);

        auto pick = [select_machine_fn, machine](wxMouseEvent &) { select_machine_fn(machine); };
        card->Bind(wxEVT_LEFT_DOWN, pick);
        name_lbl->Bind(wxEVT_LEFT_DOWN, pick);
        st->Bind(wxEVT_LEFT_DOWN, pick);
        dot->Bind(wxEVT_LEFT_DOWN, pick);
        printer_bmp->Bind(wxEVT_LEFT_DOWN, pick);
        chev->Bind(wxEVT_LEFT_DOWN, pick);
        more->Bind(wxEVT_LEFT_DOWN, [this, more, machine](wxMouseEvent &evt) {
            evt.StopPropagation();
            show_printer_card_actions_menu(more, machine);
        });
        printers_popup_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    };

    if (!online_list.empty()) {
        add_section_title(_L("Active printers"));
        for (MachineObject *m : online_list)
            add_printer_card(m);
    }
    if (!offline_list.empty()) {
        add_section_title(_L("Offline printers"));
        for (MachineObject *m : offline_list)
            add_printer_card(m);
    }
    if (sorted.empty()) {
        auto *empty = new wxStaticText(m_printers_popup_panel, wxID_ANY, _L("No printers found. Use + Add or add by IP."));
        empty->SetForegroundColour(k_muted);
        printers_popup_sizer->Add(empty, 0, wxALL, FromDIP(14));
    }

    auto *add_printer_wrap = new StaticBox(m_printers_popup_panel, wxID_ANY);
    add_printer_wrap->SetCornerRadius(static_cast<double>(FromDIP(10)));
    add_printer_wrap->SetBorderWidth(FromDIP(1));
    add_printer_wrap->SetBorderStyle(wxPENSTYLE_SHORT_DASH);
    add_printer_wrap->SetBorderColorNormal(wxColour(200, 200, 204));
    add_printer_wrap->SetBackgroundColorNormal(wxColour(255, 255, 255));
    add_printer_wrap->SetBackgroundColour(wxColour(255, 255, 255));
    add_printer_wrap->SetMinSize(wxSize(-1, FromDIP(40)));
    add_printer_wrap->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *add_printer_sizer = new wxBoxSizer(wxHORIZONTAL);
    add_printer_sizer->AddStretchSpacer(1);
    auto *add_printer_lbl = new wxStaticText(add_printer_wrap, wxID_ANY, _L("+ Add Printer"));
    add_printer_lbl->SetForegroundColour(k_green);
    add_printer_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
    add_printer_sizer->Add(add_printer_lbl, 0, wxALIGN_CENTER_VERTICAL);
    add_printer_sizer->AddStretchSpacer(1);
    add_printer_wrap->SetSizer(add_printer_sizer);
    const auto open_add_printer = [this](wxMouseEvent &) { show_add_printer_dialog(); };
    add_printer_wrap->Bind(wxEVT_LEFT_DOWN, open_add_printer);
    add_printer_lbl->Bind(wxEVT_LEFT_DOWN, open_add_printer);
    printers_popup_sizer->Add(add_printer_wrap, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));

    printers_popup_sizer->AddSpacer(FromDIP(12));

    m_printers_popup_panel->SetSizer(printers_popup_sizer);
    printers_popup_sizer->Fit(m_printers_popup_panel);
    m_printers_popup_panel->Layout();

    const wxSize popup_size = m_printers_popup_panel->GetBestSize();
    m_printers_popup_panel->SetSize(popup_size);
    m_printers_popup->SetClientSize(popup_size);
    m_printers_popup->SetSize(popup_size);
    m_printers_popup->Layout();
    rebuild_sidebar_printer_list();
}

wxString PrinterWebView::sidebar_display_name_for(const MachineObject *machine) const
{
    if (machine == nullptr)
        return _L("Unknown Printer");

    const std::string dev_name = machine->get_dev_name();
    const std::string dev_ip   = machine->get_dev_ip();

    if (!dev_name.empty() && dev_name != dev_ip && !looks_like_network_identifier(dev_name))
        return from_u8(dev_name);

    const wxString model_name = machine->get_printer_type_display_str();
    if (!model_name.empty() && model_name != "N/A")
        return model_name;

    return _L("Unknown Printer");
}

void PrinterWebView::show_sidebar_root_view()
{
    if (m_sidebar_root_panel != nullptr)
        m_sidebar_root_panel->Show();
    if (m_sidebar_printer_list_panel != nullptr)
        m_sidebar_printer_list_panel->Hide();
    if (m_sidebar_add_printer_panel != nullptr)
        m_sidebar_add_printer_panel->Hide();
    if (m_sidebar_header_panel != nullptr)
        m_sidebar_header_panel->Hide();

    Layout();
}

void PrinterWebView::set_sidebar_user_avatar(const wxBitmap &avatar_bitmap)
{
    m_sidebar_user_avatar_bitmap = avatar_bitmap;
    if (m_sidebar_user_avatar_panel != nullptr)
        m_sidebar_user_avatar_panel->Refresh();
}

void PrinterWebView::begin_moonraker_lan_scan()
{
    if (m_lan_scan_in_progress) {
        m_lan_rescan_requested = true;
        return;
    }

    m_lan_scan_in_progress = true;
    m_lan_rescan_requested = false;
    m_discovered_moonraker_printers.clear();

    std::thread([this]() {
        const auto subnets = local_ipv4_subnets();
        std::vector<std::string> candidates;
        for (const auto &subnet : subnets) {
            for (uint32_t host = subnet.network + 1; host < subnet.broadcast; ++host)
                candidates.push_back(uint_to_ipv4(host));
        }

        std::atomic<size_t> next_index{ 0 };
        const size_t worker_count = (std::min<size_t>)(32, (std::max<size_t>)(1, candidates.size()));
        std::vector<std::thread> workers;
        std::vector<BBLocalMachine> discovered;
        std::mutex discovered_mutex;
        workers.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i) {
            workers.emplace_back([&]() {
                while (true) {
                    const size_t index = next_index.fetch_add(1);
                    if (index >= candidates.size())
                        break;
                    BBLocalMachine machine;
                    if (probe_moonraker_host(candidates[index], machine)) {
                        std::lock_guard<std::mutex> lock(discovered_mutex);
                        const auto duplicate = std::find_if(
                            discovered.begin(),
                            discovered.end(),
                            [&](const BBLocalMachine &existing) { return existing.dev_ip == machine.dev_ip; });
                        if (duplicate == discovered.end())
                            discovered.push_back(machine);
                    }
                }
            });
        }
        for (auto &worker : workers)
            worker.join();

        CallAfter([this, discovered = std::move(discovered)]() mutable {
            discovered.erase(
                std::remove_if(discovered.begin(), discovered.end(), [](const BBLocalMachine &machine) {
                    const std::string normalized_name = to_lower_ascii(machine.dev_name);
                    return normalized_name != "co-print" && normalized_name != "coprint";
                }),
                discovered.end());
            std::sort(discovered.begin(), discovered.end(), [](const BBLocalMachine &a, const BBLocalMachine &b) {
                return a.dev_name < b.dev_name;
            });
            m_discovered_moonraker_printers = std::move(discovered);
            m_lan_scan_in_progress = false;
            if (m_sidebar_add_printer_panel != nullptr && m_sidebar_add_printer_panel->IsShown() &&
                m_sidebar_add_tab_index == 0)
                show_sidebar_add_printer_view();
            if (m_lan_rescan_requested)
                begin_moonraker_lan_scan();
        });
    }).detach();
}

void PrinterWebView::show_sidebar_printers_view()
{
    if (m_sidebar_root_panel != nullptr)
        m_sidebar_root_panel->Hide();
    if (m_sidebar_printer_list_panel != nullptr)
        m_sidebar_printer_list_panel->Show();
    if (m_sidebar_add_printer_panel != nullptr)
        m_sidebar_add_printer_panel->Hide();
    if (m_sidebar_header_back != nullptr)
        m_sidebar_header_back->Show();
    if (m_sidebar_header_title != nullptr)
        m_sidebar_header_title->SetLabelText(_L("Printers"));
    if (m_sidebar_header_add != nullptr)
        m_sidebar_header_add->Show();
    if (m_sidebar_header_panel != nullptr)
        m_sidebar_header_panel->Show();

    rebuild_sidebar_printer_list();
    Layout();
}

void PrinterWebView::show_sidebar_add_printer_view()
{
    if (m_sidebar_add_printer_panel == nullptr)
        return;

    if (m_sidebar_root_panel != nullptr)
        m_sidebar_root_panel->Hide();
    if (m_sidebar_printer_list_panel != nullptr)
        m_sidebar_printer_list_panel->Hide();
    m_sidebar_add_printer_panel->Show();
    if (m_sidebar_header_back != nullptr)
        m_sidebar_header_back->Show();
    if (m_sidebar_header_title != nullptr)
        m_sidebar_header_title->SetLabelText(_L("Add Printer"));
    if (m_sidebar_header_add != nullptr)
        m_sidebar_header_add->Hide();
    if (m_sidebar_header_panel != nullptr)
        m_sidebar_header_panel->Show();

    if (auto *old_sizer = m_sidebar_add_printer_panel->GetSizer()) {
        m_sidebar_add_printer_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_sidebar_add_printer_panel->DestroyChildren();
    auto *root = new wxBoxSizer(wxVERTICAL);

    const wxColour k_text("#F1F3F4");
    const wxColour k_muted("#A7ADB5");
    const wxColour k_green("#35AD27");
    const wxColour k_card("#232527");
    const wxColour k_border("#3A3D40");

    auto *tabs = new wxFlexGridSizer(1, 3, 0, 0);
    tabs->AddGrowableCol(0, 1);
    tabs->AddGrowableCol(1, 1);
    tabs->AddGrowableCol(2, 1);
    const std::array<wxString, 3> tab_names = { _L("Auto Connect"), _L("IP Connect"), _L("Manuel Setup") };
    for (size_t i = 0; i < tab_names.size(); ++i) {
        auto *tab = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, tab_names[i], wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        tab->SetForegroundColour(static_cast<int>(i) == m_sidebar_add_tab_index ? k_green : k_text);
        tab->SetCursor(wxCursor(wxCURSOR_HAND));
        {
            wxFont f = tab->GetFont();
            f.SetPointSize(8);
            tab->SetFont(f);
        }
        tabs->Add(tab, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        tab->Bind(wxEVT_LEFT_DOWN, [this, i](wxMouseEvent &) {
            m_sidebar_add_tab_index = static_cast<int>(i);
            show_sidebar_add_printer_view();
        });
    }
    root->Add(tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *tab_line = new wxPanel(m_sidebar_add_printer_panel, wxID_ANY);
    tab_line->SetMinSize(wxSize(-1, FromDIP(1)));
    tab_line->SetBackgroundColour(k_border);
    root->Add(tab_line, 0, wxEXPAND | wxTOP, FromDIP(8));

    wxStaticText *status = nullptr;
    wxTextCtrl *ip_input = nullptr;
    Button *add_btn = nullptr;

    if (m_sidebar_add_tab_index == 0) {
        auto *search_row = new wxBoxSizer(wxHORIZONTAL);
        auto *searching = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, _L("Searching for printers on your network..."));
        searching->SetForegroundColour(k_text);
        search_row->Add(searching, 1, wxALIGN_CENTER_VERTICAL);
        auto *refresh = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, wxString::FromUTF8("\xE2\x9F\xB3"));
        refresh->SetForegroundColour(k_text);
        refresh->SetCursor(wxCursor(wxCURSOR_HAND));
        search_row->Add(refresh, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(search_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        if (m_discovered_moonraker_printers.empty() && !m_lan_scan_in_progress)
            begin_moonraker_lan_scan();

        auto *auto_list_row = new wxBoxSizer(wxHORIZONTAL);
        auto *auto_list = new wxScrolledWindow(m_sidebar_add_printer_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_auto_connect_list_window = auto_list;
        auto_list->SetBackgroundColour(wxColour("#2A2C2E"));
        auto_list->SetScrollRate(0, FromDIP(8));
        auto_list->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
        auto_list->SetMinSize(wxSize(-1, FromDIP(150)));
        auto *auto_list_sizer = new wxBoxSizer(wxVERTICAL);
        auto_list->SetSizer(auto_list_sizer);
        auto_list_row->Add(auto_list, 1, wxEXPAND);

        auto *scroll_track = new wxPanel(m_sidebar_add_printer_panel, wxID_ANY);
        m_auto_connect_scroll_track = scroll_track;
        scroll_track->SetMinSize(wxSize(FromDIP(8), -1));
        scroll_track->SetMaxSize(wxSize(FromDIP(8), -1));
        scroll_track->SetBackgroundStyle(wxBG_STYLE_PAINT);
        scroll_track->Bind(wxEVT_PAINT, [scroll_track](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(scroll_track);
            dc.SetBackground(wxBrush(scroll_track->GetParent()->GetBackgroundColour()));
            dc.Clear();
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour("#34373A")));
            const wxSize sz = scroll_track->GetClientSize();
            dc.DrawRoundedRectangle(scroll_track->FromDIP(2), 0, scroll_track->FromDIP(4), sz.GetHeight(), scroll_track->FromDIP(2));
        });
        auto *scroll_thumb = new wxPanel(scroll_track, wxID_ANY);
        m_auto_connect_scroll_thumb = scroll_thumb;
        scroll_thumb->SetBackgroundStyle(wxBG_STYLE_PAINT);
        scroll_thumb->Bind(wxEVT_PAINT, [scroll_thumb](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(scroll_thumb);
            dc.SetBackground(wxBrush(scroll_thumb->GetParent()->GetBackgroundColour()));
            dc.Clear();
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour("#7A8088")));
            const wxSize sz = scroll_thumb->GetClientSize();
            dc.DrawRoundedRectangle(0, 0, sz.GetWidth(), sz.GetHeight(), sz.GetWidth() / 2.0);
        });
        auto_list_row->Add(scroll_track, 0, wxEXPAND | wxLEFT, FromDIP(4));

        auto rebuild_auto_cards = [this, auto_list, auto_list_sizer, k_text, k_muted, k_card, k_border]() {
            if (m_discovered_moonraker_printers.empty()) {
                auto *empty = new wxStaticText(auto_list, wxID_ANY,
                                               m_lan_scan_in_progress ? _L("Scanning your network...") : _L("No printers discovered yet."));
                empty->SetForegroundColour(k_muted);
                auto_list_sizer->Add(empty, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
                auto_list->FitInside();
                auto_list->Layout();
                return;
            }
            for (const auto &machine_info : m_discovered_moonraker_printers) {
                auto *card = new StaticBox(auto_list, wxID_ANY);
                card->SetCornerRadius(FromDIP(8));
                card->SetBorderWidth(1);
                card->SetBorderColorNormal(k_border);
                card->SetBackgroundColorNormal(k_card);
                card->SetBackgroundColour(k_card);
                card->SetMinSize(wxSize(-1, FromDIP(72)));
                auto *row = new wxBoxSizer(wxHORIZONTAL);
                row->AddSpacer(FromDIP(12));
                auto *copy = new wxBoxSizer(wxVERTICAL);
                auto *name = new wxStaticText(card, wxID_ANY, from_u8(machine_info.dev_name));
                name->SetForegroundColour(k_text);
                wxFont nf = name->GetFont();
                nf.SetPointSize(9);
                nf.SetWeight(wxFONTWEIGHT_BOLD);
                name->SetFont(nf);
                copy->Add(name, 0);
                const std::string display_ip = machine_info.dev_ip.substr(0, machine_info.dev_ip.find(':'));
                auto *ip = new wxStaticText(card, wxID_ANY, from_u8(display_ip));
                ip->SetForegroundColour(k_muted);
                {
                    wxFont ipf = ip->GetFont();
                    ipf.SetPointSize((std::max)(1, ipf.GetPointSize() - 2));
                    ip->SetFont(ipf);
                }
                copy->Add(ip, 0);
                row->Add(copy, 1, wxALIGN_CENTER_VERTICAL);
                auto *add = new Button(card, _L("Add"));
                add->SetMinSize(wxSize(FromDIP(58), FromDIP(32)));
                row->Add(add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
                card->SetSizer(row);
                add->Bind(wxEVT_BUTTON, [this, machine_info](wxCommandEvent &) {
                    if (finish_add_moonraker_printer(machine_info, false))
                        show_sidebar_printers_view();
                });
                auto_list_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
            }
            auto_list->FitInside();
            auto_list->Layout();
        };
        rebuild_auto_cards();
        auto update_custom_scrollbar = [this]() {
            if (m_auto_connect_list_window == nullptr || m_auto_connect_scroll_track == nullptr ||
                m_auto_connect_scroll_thumb == nullptr)
                return;
            int x = 0, y = 0;
            m_auto_connect_list_window->GetViewStart(&x, &y);
            int ux = 0, uy = 0;
            m_auto_connect_list_window->GetScrollPixelsPerUnit(&ux, &uy);
            const int content_height = m_auto_connect_list_window->GetVirtualSize().GetHeight();
            const int viewport_height = m_auto_connect_list_window->GetClientSize().GetHeight();
            const int track_height = m_auto_connect_scroll_track->GetClientSize().GetHeight();
            if (content_height <= viewport_height || track_height <= 0) {
                m_auto_connect_scroll_track->Hide();
                return;
            }
            m_auto_connect_scroll_track->Show();
            const int thumb_height = (std::max)(FromDIP(28), track_height * viewport_height / content_height);
            const int max_scroll_px = (std::max)(1, content_height - viewport_height);
            const int scroll_px = y * uy;
            const int thumb_y = (track_height - thumb_height) * scroll_px / max_scroll_px;
            m_auto_connect_scroll_thumb->SetSize(FromDIP(6), thumb_height);
            m_auto_connect_scroll_thumb->SetPosition(wxPoint(FromDIP(1), thumb_y));
            m_auto_connect_scroll_track->Refresh();
            m_auto_connect_scroll_thumb->Refresh();
        };
        auto on_scroll = [update_custom_scrollbar](wxScrollWinEvent &evt) {
            evt.Skip();
            update_custom_scrollbar();
        };
        auto_list->Bind(wxEVT_SCROLLWIN_TOP, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_BOTTOM, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_LINEUP, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_LINEDOWN, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_PAGEUP, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_PAGEDOWN, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_THUMBTRACK, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, on_scroll);
        auto_list->Bind(wxEVT_MOUSEWHEEL, [update_custom_scrollbar](wxMouseEvent &evt) {
            evt.Skip();
            update_custom_scrollbar();
        });
        auto_list->Bind(wxEVT_SIZE, [update_custom_scrollbar](wxSizeEvent &evt) {
            evt.Skip();
            update_custom_scrollbar();
        });
        root->Add(auto_list_row, 1, wxEXPAND | wxTOP, FromDIP(4));
        CallAfter(update_custom_scrollbar);
        refresh->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
            m_discovered_moonraker_printers.clear();
            begin_moonraker_lan_scan();
            show_sidebar_add_printer_view();
        });
    } else if (m_sidebar_add_tab_index == 1) {
        auto *prompt = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, _L("Enter your printer's IP address"));
        prompt->SetForegroundColour(k_text);
        {
            wxFont f = prompt->GetFont();
            f.SetPointSize((std::max)(1, f.GetPointSize() - 2));
            prompt->SetFont(f);
        }
        root->Add(prompt, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto *input_row = new wxBoxSizer(wxHORIZONTAL);
        ip_input = new wxTextCtrl(m_sidebar_add_printer_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        ip_input->SetHint(_L("Type IP address..."));
        ip_input->SetMinSize(wxSize(-1, FromDIP(32)));
        {
            wxFont f = ip_input->GetFont();
            f.SetPointSize((std::max)(1, f.GetPointSize() - 2));
            ip_input->SetFont(f);
        }
        input_row->Add(ip_input, 1, wxRIGHT, FromDIP(8));
        add_btn = new Button(m_sidebar_add_printer_panel, _L("Add"));
        add_btn->SetMinSize(wxSize(FromDIP(58), FromDIP(32)));
        input_row->Add(add_btn, 0);
        root->Add(input_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        status = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, _L("Connecting the printer..."));
        status->SetForegroundColour(k_muted);
        status->Hide();
        root->Add(status, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    } else {
        auto *manual = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, _L("Manual setup will be added here."));
        manual->SetForegroundColour(k_muted);
        root->Add(manual, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    }

    auto *hint_box = new StaticBox(m_sidebar_add_printer_panel, wxID_ANY);
    hint_box->SetCornerRadius(FromDIP(8));
    hint_box->SetBorderWidth(1);
    hint_box->SetBorderColorNormal(k_border);
    hint_box->SetBackgroundColorNormal(k_card);
    hint_box->SetBackgroundColour(k_card);
    hint_box->SetMinSize(wxSize(-1, FromDIP(56)));
    auto *hint_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *hint_icon = new wxStaticBitmap(hint_box, wxID_ANY, create_scaled_bitmap("device_sidebar_idea_light", this, 20));
    hint_sz->Add(hint_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    hint_sz->AddSpacer(FromDIP(10));
    auto *hint = new wxStaticText(hint_box, wxID_ANY, _L("Make sure your printer is powered on\nand connected the same network."));
    hint->SetForegroundColour(k_text);
    {
        wxFont f = hint->GetFont();
        f.SetPointSize((std::max)(1, f.GetPointSize() - 1));
        hint->SetFont(f);
    }
    hint_sz->Add(hint, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(12));
    hint_box->SetSizer(hint_sz);
    root->Add(hint_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    if (m_sidebar_add_tab_index == 0) {
        auto *fallback = new StaticBox(m_sidebar_add_printer_panel, wxID_ANY);
        fallback->SetCornerRadius(FromDIP(8));
        fallback->SetBorderWidth(1);
        fallback->SetBorderColorNormal(wxColour("#35512F"));
        fallback->SetBackgroundColorNormal(wxColour("#293126"));
        fallback->SetBackgroundColour(wxColour("#293126"));
        fallback->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *fallback_sizer = new wxBoxSizer(wxVERTICAL);
        auto *title = new wxStaticText(fallback, wxID_ANY, _L("Can't find your printer?"));
        title->SetForegroundColour(k_green);
        wxFont tf = title->GetFont();
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(tf);
        auto *sub = new wxStaticText(fallback, wxID_ANY, _L("Try IP Address or Manuel Setup"));
        sub->SetForegroundColour(k_green);
        fallback_sizer->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
        fallback_sizer->Add(sub, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(10));
        fallback->SetSizer(fallback_sizer);
        auto go_ip = [this](wxMouseEvent &) {
            m_sidebar_add_tab_index = 1;
            show_sidebar_add_printer_view();
        };
        fallback->Bind(wxEVT_LEFT_DOWN, go_ip);
        title->Bind(wxEVT_LEFT_DOWN, go_ip);
        sub->Bind(wxEVT_LEFT_DOWN, go_ip);
        root->Add(fallback, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    }

    auto submit_ip = [this, ip_input, status](wxCommandEvent &) {
        if (ip_input == nullptr || status == nullptr)
            return;
        wxString value = ip_input->GetValue();
        value.Trim(true);
        value.Trim(false);
        if (value.empty()) {
            status->Show();
            status->SetLabelText(_L("IP address cannot be empty."));
            m_sidebar_add_printer_panel->Layout();
            return;
        }

        std::string host = into_u8(value);
        const bool has_scheme = host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0;
        const std::string normalized_host = MachineObject::dev_id_from_address(host);
        std::string dev_ip = normalized_host;
        if (!has_scheme && normalized_host.find(':') == std::string::npos)
            dev_ip += ":7125";

        BBLocalMachine machine;
        machine.dev_id = dev_ip;
        machine.dev_ip = dev_ip;
        machine.dev_name = "Unknown Printer";
        machine.printer_type = "Moonraker";

        status->Show();
        status->SetLabelText(_L("Connecting the printer..."));
        if (finish_add_moonraker_printer(machine, host.rfind("https://", 0) == 0))
            show_sidebar_printers_view();
        else {
            status->SetLabelText(_L("Could not connect to the printer."));
            m_sidebar_add_printer_panel->Layout();
        }
    };
    if (add_btn != nullptr)
        add_btn->Bind(wxEVT_BUTTON, submit_ip);
    if (ip_input != nullptr)
        ip_input->Bind(wxEVT_TEXT_ENTER, submit_ip);

    m_sidebar_add_printer_panel->SetSizer(root);
    m_sidebar_add_printer_panel->Layout();
    Layout();
}

void PrinterWebView::rebuild_sidebar_printer_list()
{
    if (m_sidebar_printer_list_panel == nullptr || m_sidebar_printer_list_sizer == nullptr)
        return;

    m_sidebar_printer_list_panel->DestroyChildren();
    m_sidebar_printer_list_sizer->Clear(false);

    auto *dev_manager = wxGetApp().getDeviceManager();
    const auto my_machines = dev_manager ? dev_manager->get_my_machine_list() : std::map<std::string, MachineObject*>();
    const auto local_machines = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();
    auto *selected_machine = dev_manager ? dev_manager->get_selected_machine() : nullptr;

    auto normalize_host = [](std::string value) {
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        trim(value);
        const auto scheme_pos = value.find("://");
        if (scheme_pos != std::string::npos)
            value = value.substr(scheme_pos + 3);
        const auto slash_pos = value.find('/');
        if (slash_pos != std::string::npos)
            value = value.substr(0, slash_pos);
        const auto question_pos = value.find('?');
        if (question_pos != std::string::npos)
            value = value.substr(0, question_pos);
        const auto at_pos = value.find('@');
        if (at_pos != std::string::npos)
            value = value.substr(at_pos + 1);
        if (std::count(value.begin(), value.end(), ':') == 1) {
            const auto colon_pos = value.rfind(':');
            if (colon_pos != std::string::npos)
                value = value.substr(0, colon_pos);
        }
        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };

    auto same_machine_identity = [&](const MachineObject *a, const MachineObject *b) {
        if (a == nullptr || b == nullptr)
            return false;
        if ((!a->get_dev_id().empty() && a->get_dev_id() == b->get_dev_id()) ||
            (!a->get_dev_ip().empty() && a->get_dev_ip() == b->get_dev_ip()) ||
            (!a->get_dev_id().empty() && a->get_dev_id() == b->get_dev_ip()) ||
            (!a->get_dev_ip().empty() && a->get_dev_ip() == b->get_dev_id()))
            return true;
        const std::string a_host = normalize_host(!a->get_dev_ip().empty() ? a->get_dev_ip() : a->get_dev_id());
        const std::string b_host = normalize_host(!b->get_dev_ip().empty() ? b->get_dev_ip() : b->get_dev_id());
        return !a_host.empty() && a_host == b_host;
    };

    auto has_local_machine_record = [&](const MachineObject *machine) {
        if (machine == nullptr)
            return false;
        for (const auto &entry : local_machines) {
            MachineObject *local = entry.second;
            if (local != nullptr && same_machine_identity(machine, local))
                return true;
            const std::string target_host = normalize_host(!machine->get_dev_ip().empty() ? machine->get_dev_ip() : machine->get_dev_id());
            if (!target_host.empty() &&
                (normalize_host(entry.first) == target_host ||
                 (local != nullptr && normalize_host(local->get_dev_id()) == target_host) ||
                 (local != nullptr && normalize_host(local->get_dev_ip()) == target_host)))
                return true;
        }
        return false;
    };

    std::map<std::string, MachineObject*> all_by_id;
    for (const auto &entry : my_machines)
        if (entry.second != nullptr && (!entry.second->is_lan_mode_printer() || has_local_machine_record(entry.second)))
            all_by_id[entry.first] = entry.second;
    for (const auto &entry : local_machines)
        if (entry.second != nullptr && all_by_id.find(entry.first) == all_by_id.end())
            all_by_id[entry.first] = entry.second;
    if (selected_machine != nullptr &&
        (!selected_machine->is_lan_mode_printer() || has_local_machine_record(selected_machine)))
        all_by_id[selected_machine->get_dev_id()] = selected_machine;

    std::vector<MachineObject *> online_list;
    std::vector<MachineObject *> offline_list;
    for (const auto &entry : all_by_id) {
        auto *machine = entry.second;
        if (machine == nullptr)
            continue;
        (machine->is_online() ? online_list : offline_list).push_back(machine);
    }
    auto by_name = [](MachineObject *a, MachineObject *b) {
        if (a == nullptr || b == nullptr)
            return a != nullptr;
        return a->get_dev_name() < b->get_dev_name();
    };
    std::sort(online_list.begin(), online_list.end(), by_name);
    std::sort(offline_list.begin(), offline_list.end(), by_name);

    const wxColour k_text("#F1F3F4");
    const wxColour k_muted("#A7ADB5");
    const wxColour k_green("#35AD27");
    const wxColour k_card_bg("#232527");
    const wxColour k_card_border("#3A3D40");

    auto add_section_title = [&](const wxString &text) {
        auto *label = new wxStaticText(m_sidebar_printer_list_panel, wxID_ANY, text);
        label->SetForegroundColour(k_muted);
        {
            wxFont f = label->GetFont();
            f.SetPointSize(10);
            label->SetFont(f);
        }
        m_sidebar_printer_list_sizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    };

    auto *login_card = new StaticBox(m_sidebar_printer_list_panel, wxID_ANY);
    login_card->SetMinSize(wxSize(-1, FromDIP(60)));
    login_card->SetMaxSize(wxSize(-1, FromDIP(60)));
    login_card->SetCornerRadius(FromDIP(8));
    login_card->SetBorderWidth(1);
    login_card->SetBorderColorNormal(k_card_border);
    login_card->SetBackgroundColorNormal(k_card_bg);
    login_card->SetBackgroundColour(k_card_bg);
    login_card->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *login_row = new wxBoxSizer(wxHORIZONTAL);
    login_row->AddSpacer(FromDIP(12));
    auto *avatar = new wxPanel(login_card, wxID_ANY);
    m_sidebar_user_avatar_panel = avatar;
    avatar->SetMinSize(wxSize(FromDIP(38), FromDIP(38)));
    avatar->SetMaxSize(wxSize(FromDIP(38), FromDIP(38)));
    avatar->SetBackgroundStyle(wxBG_STYLE_PAINT);
    avatar->Bind(wxEVT_PAINT, [this, avatar](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(avatar);
        dc.SetBackground(wxBrush(avatar->GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize size = avatar->GetClientSize();
        const int radius = (std::min)(size.GetWidth(), size.GetHeight()) / 2;
        if (!m_sidebar_user_avatar_bitmap.IsOk()) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour("#D9D9D9")));
            dc.DrawCircle(size.GetWidth() / 2, size.GetHeight() / 2, radius);
        } else {
            dc.DrawBitmap(m_sidebar_user_avatar_bitmap, 0, 0, true);
        }
    });
    login_row->Add(avatar, 0, wxALIGN_CENTER_VERTICAL);
    login_row->AddSpacer(FromDIP(10));
    auto *login_copy = new wxBoxSizer(wxVERTICAL);
    auto *user_lbl = new wxStaticText(login_card, wxID_ANY, _L("User:"));
    user_lbl->SetForegroundColour(k_muted);
    {
        wxFont f = user_lbl->GetFont();
        f.SetPointSize(9);
        user_lbl->SetFont(f);
    }
    auto *login_lbl = new wxStaticText(login_card, wxID_ANY, _L("Login/Sign Up"));
    login_lbl->SetForegroundColour(k_green);
    {
        wxFont f = login_lbl->GetFont();
        f.SetPointSize(11);
        login_lbl->SetFont(f);
    }
    login_copy->Add(user_lbl, 0);
    login_copy->Add(login_lbl, 0);
    login_row->Add(login_copy, 1, wxALIGN_CENTER_VERTICAL);
    auto *enter_icon = new wxStaticBitmap(login_card, wxID_ANY, create_scaled_bitmap("device_sidebar_enter_white", this, 25));
    login_row->Add(enter_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    login_card->SetSizer(login_row);
    m_sidebar_printer_list_sizer->Add(login_card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto select_machine_fn = [this, dev_manager](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const bool was_online = machine->is_online();
        if (!was_online) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": reconnecting offline sidebar machine "
                                    << machine->get_dev_id();
            machine->connect(machine->local_use_ssl);
        }
        const std::string dev_id = machine->get_dev_id();
        // Update the real selected machine synchronously so the sidebar can reflect
        // the device page that is actually active, even before MonitorPanel's queued
        // notification is processed.
        if (dev_manager != nullptr)
            dev_manager->set_selected_machine(dev_id);
        if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->m_monitor != nullptr)
            wxGetApp().mainframe->m_monitor->select_machine(dev_id);
        m_has_active_printer_connection = was_online;
        if (was_online) {
            machine->command_request_push_all(true);
        } else {
            // connect() is asynchronous — request a push once the connection is ready
            // by scheduling it slightly after the connect attempt starts.
            CallAfter([machine]() {
                if (machine->is_online())
                    machine->command_request_push_all(true);
            });
        }
        refresh_layer_info_from_selected_machine();
        // Rebuild after the click event unwinds. Destroying the card tree while one
        // of its children is still handling the event can lead to use-after-free crashes.
        CallAfter([this]() { rebuild_sidebar_printer_list(); });
    };

    auto open_machine_device_page_fn = [this, select_machine_fn](MachineObject *machine) {
        select_machine_fn(machine);
        select_tab(PrinterWebViewTab::Status);
        show_sidebar_root_view();
    };

    auto add_printer_card = [&](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const bool online = machine->is_online();
        const bool can_forget = has_local_machine_record(machine);
        const bool selected = selected_machine != nullptr &&
                              selected_machine->get_dev_id() == machine->get_dev_id();
        auto *card = new StaticBox(m_sidebar_printer_list_panel, wxID_ANY);
        card->SetMinSize(wxSize(-1, FromDIP(70)));
        card->SetMaxSize(wxSize(-1, FromDIP(70)));
        card->SetCornerRadius(FromDIP(8));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(selected ? wxColour("#4B8C43") : k_card_border);
        card->SetBackgroundColorNormal(selected ? wxColour("#293126") : k_card_bg);
        card->SetBackgroundColour(selected ? wxColour("#293126") : k_card_bg);
        card->SetCursor(wxCursor(wxCURSOR_HAND));

        auto *outer = new wxBoxSizer(wxHORIZONTAL);
        outer->AddSpacer(FromDIP(10));
        auto *icon = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap(k_cprint_printer_nav_bitmap, this, 46));
        outer->Add(icon, 0, wxALIGN_CENTER_VERTICAL);
        outer->AddSpacer(FromDIP(10));

        auto *content = new wxBoxSizer(wxVERTICAL);
        auto *status_row = new wxBoxSizer(wxHORIZONTAL);
        auto *dot = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F"));
        dot->SetForegroundColour(online ? k_green : wxColour("#767C84"));
        auto *status = new wxStaticText(card, wxID_ANY, online ? _L("Connected") : _L("Offline"));
        status->SetForegroundColour(online ? k_green : k_muted);
        {
            wxFont f = status->GetFont();
            f.SetPointSize(7);
            status->SetFont(f);
        }
        status_row->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        status_row->Add(status, 0, wxALIGN_CENTER_VERTICAL);
        if (can_forget) {
            auto *forget_icon = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap("device_sidebar_forget", this, 12));
            forget_icon->SetCursor(wxCursor(wxCURSOR_HAND));
            status_row->AddSpacer(FromDIP(8));
            status_row->Add(forget_icon, 0, wxALIGN_CENTER_VERTICAL);
            forget_icon->Bind(wxEVT_LEFT_DOWN, [this, machine](wxMouseEvent &evt) {
                evt.StopPropagation();
                if (confirm_forget_printer())
                    forget_local_printer(machine);
            });
        }
        content->Add(status_row, 0);

        // Real device name: this comes from MachineObject / DeviceManager, not from UI mock text.
        auto *name = new wxStaticText(card, wxID_ANY, sidebar_display_name_for(machine), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        name->SetForegroundColour(k_text);
        wxFont nf = name->GetFont();
        nf.SetPointSize(11);
        nf.SetWeight(wxFONTWEIGHT_BOLD);
        name->SetFont(nf);
        content->Add(name, 0, wxEXPAND);

        auto *ip = new wxStaticText(card, wxID_ANY, from_u8(machine->get_dev_ip()));
        ip->SetForegroundColour(k_muted);
        {
            wxFont f = ip->GetFont();
            f.SetPointSize(9);
            ip->SetFont(f);
        }
        content->Add(ip, 0);
        outer->Add(content, 1, wxALIGN_CENTER_VERTICAL);

        if (online) {
            auto *check = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap("device_sidebar_connected", this, 15));
            outer->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        }
        auto *chev = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap("device_sidebar_chevron_white", this, 18));
        outer->Add(chev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        card->SetSizer(outer);
        auto pick = [open_machine_device_page_fn, machine](wxMouseEvent &) { open_machine_device_page_fn(machine); };
        card->Bind(wxEVT_LEFT_DOWN, pick);
        icon->Bind(wxEVT_LEFT_DOWN, pick);
        dot->Bind(wxEVT_LEFT_DOWN, pick);
        status->Bind(wxEVT_LEFT_DOWN, pick);
        ip->Bind(wxEVT_LEFT_DOWN, pick);
        chev->Bind(wxEVT_LEFT_DOWN, [open_machine_device_page_fn, machine](wxMouseEvent &evt) {
            evt.StopPropagation();
            open_machine_device_page_fn(machine);
        });
        m_sidebar_printer_list_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    };

    if (!online_list.empty()) {
        add_section_title(_L("Active printers"));
        for (auto *machine : online_list)
            add_printer_card(machine);
    }
    if (!offline_list.empty()) {
        add_section_title(_L("Offline printers"));
        for (auto *machine : offline_list)
            add_printer_card(machine);
    }

    auto *add_printer_wrap = new StaticBox(m_sidebar_printer_list_panel, wxID_ANY);
    add_printer_wrap->SetCornerRadius(FromDIP(8));
    add_printer_wrap->SetBorderWidth(1);
    add_printer_wrap->SetBorderStyle(wxPENSTYLE_SHORT_DASH);
    add_printer_wrap->SetBorderColorNormal(wxColour("#4B8C43"));
    add_printer_wrap->SetBackgroundColorNormal(wxColour("#2A2C2E"));
    add_printer_wrap->SetBackgroundColour(wxColour("#2A2C2E"));
    add_printer_wrap->SetMinSize(wxSize(-1, FromDIP(54)));
    add_printer_wrap->SetMaxSize(wxSize(-1, FromDIP(54)));
    auto *add_sz = new wxBoxSizer(wxHORIZONTAL);
    add_sz->AddStretchSpacer(1);
    auto *add_label = new wxStaticText(add_printer_wrap, wxID_ANY, _L("+ Add Printer"));
    add_label->SetForegroundColour(k_green);
    add_sz->Add(add_label, 0, wxALIGN_CENTER_VERTICAL);
    add_sz->AddStretchSpacer(1);
    add_printer_wrap->SetSizer(add_sz);
    const auto open_add = [this](wxMouseEvent &) { show_sidebar_add_printer_view(); };
    add_printer_wrap->Bind(wxEVT_LEFT_DOWN, open_add);
    add_label->Bind(wxEVT_LEFT_DOWN, open_add);
    m_sidebar_printer_list_sizer->Add(add_printer_wrap, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto add_secondary_nav_row = [&](const wxString &label,
                                     const std::string &icon_name,
                                     const std::function<void()> &on_activate,
                                     bool add_top_divider) {
        if (add_top_divider) {
            auto *divider = new wxPanel(m_sidebar_printer_list_panel, wxID_ANY);
            divider->SetMinSize(wxSize(-1, FromDIP(1)));
            divider->SetMaxSize(wxSize(-1, FromDIP(1)));
            divider->SetBackgroundColour(wxColour("#34373A"));
            m_sidebar_printer_list_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }

        auto *row = new wxPanel(m_sidebar_printer_list_panel, wxID_ANY);
        row->SetBackgroundColour(wxColour("#2A2C2E"));
        row->SetMinSize(wxSize(-1, FromDIP(44)));
        row->SetMaxSize(wxSize(-1, FromDIP(44)));
        row->SetCursor(wxCursor(wxCURSOR_HAND));

        auto *sz = new wxBoxSizer(wxHORIZONTAL);
        sz->AddSpacer(FromDIP(16));
        auto *icon = new wxStaticBitmap(row, wxID_ANY, create_scaled_bitmap(icon_name, row, 14));
        sz->Add(icon, 0, wxALIGN_CENTER_VERTICAL);
        sz->AddSpacer(FromDIP(10));

        auto *text = new wxStaticText(row, wxID_ANY, label);
        text->SetForegroundColour(wxColour("#D8DCE0"));
        {
            wxFont f = text->GetFont();
            f.SetPointSize(10);
            text->SetFont(f);
        }
        sz->Add(text, 1, wxALIGN_CENTER_VERTICAL);

        auto *chev = new wxStaticText(row, wxID_ANY, ">");
        chev->SetForegroundColour(wxColour("#B7BCC2"));
        sz->Add(chev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
        row->SetSizer(sz);

        auto on_click = [on_activate](wxMouseEvent &) { on_activate(); };
        row->Bind(wxEVT_LEFT_DOWN, on_click);
        icon->Bind(wxEVT_LEFT_DOWN, on_click);
        text->Bind(wxEVT_LEFT_DOWN, on_click);
        chev->Bind(wxEVT_LEFT_DOWN, on_click);

        m_sidebar_printer_list_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));
    };

    add_secondary_nav_row(_L("System Upgrade"), "monitor_upgrade_online",
                          [this]() {
                              select_tab(PrinterWebViewTab::Update);
                              show_sidebar_root_view();
                          },
                          true);
    add_secondary_nav_row(_L("Media"), "monitor_sdcard_thumbnail",
                          [this]() {
                              select_tab(PrinterWebViewTab::Storage);
                              show_sidebar_root_view();
                          },
                          false);

    m_sidebar_printer_list_panel->Layout();
    if (auto *scrolled = dynamic_cast<wxScrolledWindow *>(m_sidebar_printer_list_panel))
        scrolled->FitInside();
    if (m_sidebar_printer_list_panel->GetParent() != nullptr)
        m_sidebar_printer_list_panel->GetParent()->Layout();
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

    const std::array<wxString, 4> extruder_labels = { "T1", "T2", "T3", "T4" };
    for (int i = 0; i < (int)extruder_labels.size(); ++i) {
        auto *label = new wxStaticText(popup_box, wxID_ANY, extruder_labels[i]);
        label->SetForegroundColour(i == m_selected_extruder_index ? wxColour(44, 182, 125) : wxColour(220, 220, 220));
        label->SetCursor(wxCursor(wxCURSOR_HAND));
        const wxSize label_size = label->GetBestSize();
        const int row_height = FromDIP(45);
        const int label_x = (popup_width - label_size.GetWidth()) / 2;
        const int label_y = i * row_height + (row_height - label_size.GetHeight()) / 2;
        label->SetPosition(wxPoint(label_x, label_y));
        label->Bind(wxEVT_LEFT_DOWN, [this, i](wxMouseEvent &) {
            apply_printer_status_tool_selection(i);
            dismiss_extruder_popup();
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

    const std::array<wxString, 4> fan_labels = { "T1", "T2", "T3", "T4" };
    for (int i = 0; i < (int)fan_labels.size(); ++i) {
        auto *label = new wxStaticText(popup_box, wxID_ANY, fan_labels[i]);
        label->SetForegroundColour(i == m_selected_extruder_index ? wxColour(44, 182, 125) : wxColour(220, 220, 220));
        label->SetCursor(wxCursor(wxCURSOR_HAND));
        const wxSize label_size = label->GetBestSize();
        const int row_height = FromDIP(45);
        const int label_x = (popup_width - label_size.GetWidth()) / 2;
        const int label_y = i * row_height + (row_height - label_size.GetHeight()) / 2;
        label->SetPosition(wxPoint(label_x, label_y));
        label->Bind(wxEVT_LEFT_DOWN, [this, i](wxMouseEvent &) {
            apply_printer_status_tool_selection(i);
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

void PrinterWebView::toggle_filament_tool_popup()
{
    if (m_filament_tool_popup == nullptr || m_filament_tool_popup_button == nullptr)
        return;
    if (m_filament_tool_selector != nullptr && !m_filament_tool_selector->IsEnabled())
        return;

    rebuild_filament_tool_popup();

    if (m_filament_tool_popup->IsShown()) {
        m_filament_tool_popup->Dismiss();
        return;
    }

    const wxPoint screen_pos =
        m_filament_tool_popup_button->ClientToScreen(wxPoint(0, m_filament_tool_popup_button->GetSize().GetHeight() + FromDIP(6)));
    m_filament_tool_popup->Position(screen_pos, wxSize(0, 0));
    m_filament_tool_popup->Popup(m_filament_tool_popup_button);
}

void PrinterWebView::dismiss_filament_tool_popup()
{
    if (m_filament_tool_popup != nullptr && m_filament_tool_popup->IsShown())
        m_filament_tool_popup->Dismiss();
}

void PrinterWebView::apply_filament_preview_rows(const std::array<wxColour, 4> &model_colors,
                                                 const std::array<wxString, 4> &materials,
                                                 const std::array<wxString, 4> &weights,
                                                 const std::array<int, 4> &assigned_tools,
                                                 const std::array<wxColour, 4> &assigned_colors)
{
    for (int i = 0; i < 4; ++i) {
        const int ui_tool = assigned_tools[i] >= 1 && assigned_tools[i] <= 4 ? assigned_tools[i] : i + 1;
        m_filament_assigned_tool_mapping[i] = ui_tool;
        if (auto *p = dynamic_cast<LeftRoundedColourPanel *>(m_filament_model_color_panels[i])) {
            p->SetFillColour(model_colors[i]);
        }
        if (m_filament_material_labels[i] != nullptr)
            m_filament_material_labels[i]->SetLabelText(materials[i].empty() ? wxString("PLA") : materials[i]);
        if (m_filament_weight_labels[i] != nullptr)
            m_filament_weight_labels[i]->SetLabelText(weights[i].empty() ? wxString("--") : weights[i]);

        if (auto *p = dynamic_cast<LeftRoundedColourPanel *>(m_filament_assigned_color_panels[i])) {
            p->SetFillColour(assigned_colors[i]);
        }
        if (m_filament_assigned_tool_labels[i] != nullptr) {
            m_filament_assigned_tool_labels[i]->SetLabelText(wxString::Format("T%d", ui_tool));
        }
    }

    if (m_status_page != nullptr) {
        m_status_page->Layout();
        m_status_page->Refresh();
    } else {
        Layout();
        Refresh();
    }

    // Keep the closed Manage Filament selector in sync with the loaded tool colors.
    // The popup rows are rebuilt from m_filament_loaded_tool_colors when opened, but
    // the selected preview swatch needs an explicit refresh after Moonraker DB colors arrive.
    apply_filament_tool_selection(m_selected_filament_tool);
}

void PrinterWebView::set_filament_assigned_tool(int model_slot_index, int ui_tool, bool send_mapping_command)
{
    if (model_slot_index < 0 || model_slot_index >= 4)
        return;
    if (ui_tool < 1 || ui_tool > 4)
        ui_tool = model_slot_index + 1;

    m_filament_assigned_tool_mapping[model_slot_index] = ui_tool;

    if (auto *p = dynamic_cast<LeftRoundedColourPanel *>(m_filament_assigned_color_panels[model_slot_index]))
        p->SetFillColour(m_filament_loaded_tool_colors[ui_tool - 1]);
    if (m_filament_assigned_tool_labels[model_slot_index] != nullptr)
        m_filament_assigned_tool_labels[model_slot_index]->SetLabelText(wxString::Format("T%d", ui_tool));

    if (m_status_page != nullptr) {
        m_status_page->Layout();
        m_status_page->Refresh();
    }

    if (send_mapping_command)
        send_tool_map_command(model_slot_index, ui_tool);
}

void PrinterWebView::send_tool_map_command(int model_slot_index, int ui_tool)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return;

    const int logical_index = std::max(0, std::min(3, model_slot_index));
    const int physical_index = std::max(0, std::min(3, ui_tool - 1));
    const std::string script = "SET_TOOL_MAP LOGICAL=" + std::to_string(logical_index) +
                               " PHYSICAL=" + std::to_string(physical_index);

    BOOST_LOG_TRIVIAL(info) << "PrinterWebView: sending tool map command: " << script;

    std::thread([base, script]() {
        nlohmann::json payload;
        payload["script"] = script;
        Http::post(base + "/printer/gcode/script")
            .header("Content-Type", "application/json")
            .set_post_body(payload.dump())
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([](std::string, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: SET_TOOL_MAP status=" << status;
            })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: SET_TOOL_MAP failed status=" << status << " error=" << error;
            })
            .perform_sync();
    }).detach();
}

void PrinterWebView::apply_filament_preview_fallback()
{
    // If we have colors synced from the Plater (set at upload time), use them
    // instead of full defaults so that the Model Colors section keeps showing
    // the uploaded model's colors even while the printer is idle.
    if (m_has_plater_synced_colors) {
        apply_filament_preview_rows(
            m_plater_synced_colors,
            m_plater_synced_materials,
            default_filament_preview_weights(),
            default_filament_preview_assigned_tools(),
            m_plater_synced_colors);
        return;
    }
    const auto colors = default_filament_preview_colors();
    apply_filament_preview_rows(
        colors,
        default_filament_preview_materials(),
        default_filament_preview_weights(),
        default_filament_preview_assigned_tools(),
        colors);
}

void PrinterWebView::sync_model_colors_from_plater()
{
    auto model_colors = default_filament_preview_colors();
    auto materials    = default_filament_preview_materials();
    bool got_colors   = false;

    // Preferred source: slice_filaments_info from the current plate.
    // This contains the ACTUAL colors embedded in the sliced gcode, which
    // reflect the model's real color groups — not just the filament preset colors.
    if (auto *plater = wxGetApp().plater()) {
        PartPlate *plate = plater->get_partplate_list().get_curr_plate();
        if (plate != nullptr && !plate->get_slice_filaments_info().empty()) {
            for (const auto &fi : plate->get_slice_filaments_info()) {
                const int idx = fi.id; // 0-based extruder index
                if (idx >= 0 && idx < 4) {
                    if (!fi.color.empty())
                        model_colors[idx] = colour_from_hex(fi.color, model_colors[idx]);
                    if (!fi.type.empty())
                        materials[idx] = wxString::FromUTF8(fi.type);
                    got_colors = true;
                }
            }
        }
    }

    // Fallback: filament_colour from the project config (preset colors).
    if (!got_colors) {
        auto *preset_bundle = wxGetApp().preset_bundle;
        if (preset_bundle == nullptr)
            return;
        const DynamicPrintConfig &project_config = preset_bundle->project_config;
        const auto *color_opt = project_config.option<ConfigOptionStrings>("filament_colour");
        const auto *type_opt  = preset_bundle->full_config().option<ConfigOptionStrings>("filament_type");
        if (color_opt == nullptr || color_opt->values.empty())
            return;
        for (size_t i = 0; i < std::min<size_t>(4, color_opt->values.size()); ++i) {
            const std::string &hex = color_opt->values[i];
            if (!hex.empty()) {
                model_colors[i] = colour_from_hex(hex, model_colors[i]);
                got_colors = true;
            }
        }
        if (type_opt != nullptr) {
            for (size_t i = 0; i < std::min<size_t>(4, type_opt->values.size()); ++i) {
                if (!type_opt->values[i].empty())
                    materials[i] = wxString::FromUTF8(type_opt->values[i]);
            }
        }
    }

    if (!got_colors)
        return;

    // Keep the assigned (loaded tool) colors already fetched from the device,
    // or fall back to the model colors themselves when no device data is present.
    const bool has_tool_colors = std::any_of(
        m_filament_loaded_tool_colors.begin(), m_filament_loaded_tool_colors.end(),
        [](const wxColour &c) { return c.IsOk() && c != wxColour(0, 0, 0) && c != wxColour(255, 255, 255); });

    std::array<wxColour, 4> assigned_colors = has_tool_colors
        ? m_filament_loaded_tool_colors
        : model_colors;

    std::array<int, 4> assigned_tools = has_tool_colors
        ? nearest_unique_tool_assignment(model_colors, assigned_colors)
        : default_filament_preview_assigned_tools();

    if (has_tool_colors) {
        std::array<wxColour, 4> row_colors = assigned_colors;
        for (int i = 0; i < 4; ++i)
            row_colors[i] = assigned_colors[assigned_tools[i] - 1];
        assigned_colors = row_colors;
    }

    // Save for use as fallback when the printer is idle (no active file metadata)
    m_plater_synced_colors    = model_colors;
    m_plater_synced_materials = materials;
    m_has_plater_synced_colors = true;

    apply_filament_preview_rows(model_colors, materials,
                                default_filament_preview_weights(),
                                assigned_tools, assigned_colors);

    // Invalidate the fetch key so the next full device refresh re-fetches metadata
    m_filament_preview_fetch_key.clear();
}

void PrinterWebView::refresh_filament_preview_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    const wxString display_file_name = active_file_name_text(obj);
    const wxString metadata_file_path = active_file_metadata_path(obj);
    const bool has_file = !metadata_file_path.empty();

    if (obj == nullptr || !obj->is_online() || base.empty()) {
        m_filament_preview_fetch_key.clear();
        m_filament_preview_fetch_in_progress = false;
        apply_filament_preview_fallback();
        return;
    }

    if (!has_file && !m_has_plater_synced_colors)
        sync_model_colors_from_plater();

    const wxString key = from_u8(base) + "|" + (has_file ? metadata_file_path : display_file_name);
    if (m_filament_preview_fetch_in_progress || key == m_filament_preview_fetch_key)
        return;

    m_filament_preview_fetch_in_progress = true;
    m_filament_preview_fetch_key = key;

    const std::string metadata_url = has_file
        ? base + "/server/files/metadata?filename=" + url_encode_component(metadata_file_path)
        : std::string();
    const std::string db_url = base + "/server/database/item?namespace=coprint&key=filament_selections";

    std::thread([this, key, metadata_url, db_url]() {
        auto fetch_json_text = [](const std::string &url) {
            std::string body;
            if (url.empty())
                return body;
            Http::get(url)
                .timeout_connect(2)
                .timeout_max(4)
                .on_complete([&](std::string response, unsigned status) {
                    if (status == 200)
                        body = std::move(response);
                })
                .on_error([](std::string, std::string, unsigned) {})
                .perform_sync();
            return body;
        };

        const std::string metadata_body = fetch_json_text(metadata_url);
        const std::string db_body = fetch_json_text(db_url);

        CallAfter([this, key, metadata_body, db_body]() {
            m_filament_preview_fetch_in_progress = false;
            if (key != m_filament_preview_fetch_key)
                return;

            auto model_colors = default_filament_preview_colors();
            auto assigned_colors = default_filament_preview_colors();
            auto materials = default_filament_preview_materials();
            auto weights = default_filament_preview_weights();
            auto assigned_tools = default_filament_preview_assigned_tools();
            auto loaded_tool_materials = default_filament_preview_materials();
            bool has_loaded_tool_colors = false;
            bool metadata_colors_parsed = false;

            auto parse_json = [](const std::string &body) {
                if (body.empty())
                    return nlohmann::json();
                auto parsed = nlohmann::json::parse(body, nullptr, false, true);
                return parsed.is_discarded() ? nlohmann::json() : parsed;
            };
            auto split_metadata_string = [](const std::string &raw) {
                wxString s = wxString::FromUTF8(raw);
                wxChar sep = ',';
                if (s.Find(';') != wxNOT_FOUND)
                    sep = ';';
                wxArrayString parts = wxSplit(s, sep);
                for (auto &part : parts) {
                    part.Trim(true);
                    part.Trim(false);
                    if (part.StartsWith("\"") && part.EndsWith("\"") && part.length() >= 2)
                        part = part.Mid(1, part.length() - 2);
                }
                return parts;
            };

            auto metadata = parse_json(metadata_body);
            if (metadata.contains("result"))
                metadata = metadata["result"];
            if (metadata.is_object()) {
                if (auto it = metadata.find("filament_colors"); it != metadata.end() && it->is_array()) {
                    for (size_t i = 0; i < std::min<size_t>(4, it->size()); ++i) {
                        if ((*it)[i].is_string()) {
                            model_colors[i] = colour_from_hex((*it)[i].get<std::string>(), model_colors[i]);
                            metadata_colors_parsed = true;
                        }
                    }
                } else if (auto it = metadata.find("filament_colors"); it != metadata.end() && it->is_string()) {
                    auto colors = split_metadata_string(it->get<std::string>());
                    for (size_t i = 0; i < std::min<size_t>(4, colors.size()); ++i) {
                        model_colors[i] = colour_from_hex(into_u8(colors[i]), model_colors[i]);
                        metadata_colors_parsed = true;
                    }
                }

                if (auto it = metadata.find("filament_weights"); it != metadata.end() && it->is_array()) {
                    for (size_t i = 0; i < std::min<size_t>(4, it->size()); ++i) {
                        if ((*it)[i].is_number())
                            weights[i] = wxString::Format("%.1fg", (*it)[i].get<double>());
                    }
                } else if (auto it = metadata.find("filament_weights"); it != metadata.end() && it->is_string()) {
                    auto parts = split_metadata_string(it->get<std::string>());
                    for (size_t i = 0; i < std::min<size_t>(4, parts.size()); ++i) {
                        double value = 0.0;
                        if (parts[i].ToDouble(&value))
                            weights[i] = wxString::Format("%.1fg", value);
                    }
                } else if (auto total = metadata.find("filament_weight_total"); total != metadata.end() && total->is_number()) {
                    const double each = total->get<double>() / 4.0;
                    for (auto &w : weights)
                        w = wxString::Format("%.1fg", each);
                }

                if (auto it = metadata.find("filament_type"); it != metadata.end()) {
                    if (it->is_array()) {
                        for (size_t i = 0; i < std::min<size_t>(4, it->size()); ++i)
                            if ((*it)[i].is_string())
                                materials[i] = wxString::FromUTF8((*it)[i].get<std::string>());
                    } else if (it->is_string()) {
                        const std::string raw = it->get<std::string>();
                        auto parsed_types = nlohmann::json::parse(raw, nullptr, false, true);
                        if (!parsed_types.is_discarded() && parsed_types.is_array()) {
                            for (size_t i = 0; i < std::min<size_t>(4, parsed_types.size()); ++i)
                                if (parsed_types[i].is_string())
                                    materials[i] = wxString::FromUTF8(parsed_types[i].get<std::string>());
                        } else {
                            wxArrayString parts = split_metadata_string(raw);
                            for (size_t i = 0; i < std::min<size_t>(4, parts.size()); ++i) {
                                if (!parts[i].empty())
                                    materials[i] = parts[i];
                            }
                        }
                    }
                }
            }

            auto db = parse_json(db_body);
            if (db.contains("result"))
                db = db["result"];
            if (db.is_object() && db.contains("value"))
                db = db["value"];
            if (db.is_object() && db.contains("filament_selections"))
                db = db["filament_selections"];

            if (db.is_object()) {
                for (int ui_tool = 1; ui_tool <= 4; ++ui_tool) {
                    const std::string key_name = "toolhead_" + std::to_string(ui_tool);
                    const std::string zero_based_key_name = "toolhead_" + std::to_string(ui_tool - 1);
                    const nlohmann::json *tool_ptr = nullptr;
                    if (db.contains(key_name) && db[key_name].is_object())
                        tool_ptr = &db[key_name];
                    else if (db.contains(zero_based_key_name) && db[zero_based_key_name].is_object())
                        tool_ptr = &db[zero_based_key_name];
                    if (tool_ptr == nullptr)
                        continue;
                    const auto &tool = *tool_ptr;
                    if (auto it = tool.find("color_hex"); it != tool.end() && it->is_string()) {
                        assigned_colors[ui_tool - 1] = colour_from_hex(it->get<std::string>(), assigned_colors[ui_tool - 1]);
                        has_loaded_tool_colors = true;
                    }
                    if (auto it = tool.find("type"); it != tool.end() && it->is_string())
                        loaded_tool_materials[ui_tool - 1] = wxString::FromUTF8(it->get<std::string>());
                }
            }

            m_filament_loaded_tool_colors = assigned_colors;
            m_filament_loaded_tool_materials = loaded_tool_materials;
            if (has_loaded_tool_colors) {
                assigned_tools = nearest_unique_tool_assignment(model_colors, assigned_colors);
                std::array<wxColour, 4> row_assigned_colors = assigned_colors;
                for (int i = 0; i < 4; ++i)
                    row_assigned_colors[i] = assigned_colors[assigned_tools[i] - 1];
                assigned_colors = row_assigned_colors;
            }

            // If metadata returned no model colors, keep the colors synced from
            // the Plater, but still apply the loaded-filament / Assigned Tools
            // information fetched from Moonraker DB above. Previously this
            // returned early and accidentally dropped the printer's loaded
            // filament colors.
            if (!metadata_colors_parsed && m_has_plater_synced_colors) {
                model_colors = m_plater_synced_colors;
                materials = m_plater_synced_materials;
                if (has_loaded_tool_colors) {
                    const auto loaded_colors = m_filament_loaded_tool_colors;
                    assigned_tools = nearest_unique_tool_assignment(model_colors, loaded_colors);
                    for (int i = 0; i < 4; ++i)
                        assigned_colors[i] = loaded_colors[assigned_tools[i] - 1];
                }
                apply_filament_preview_rows(model_colors, materials, weights, assigned_tools, assigned_colors);
                return;
            }

            if (metadata_colors_parsed)
                m_has_plater_synced_colors = false;

            apply_filament_preview_rows(model_colors, materials, weights, assigned_tools, assigned_colors);
        });
    }).detach();
}

void PrinterWebView::prompt_and_save_filament_selection_then_load()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->is_in_printing())
        return;

    const int ui_tool = m_selected_filament_tool + 1;
    wxTextEntryDialog material_dialog(
        this,
        wxString::Format(_L("Tool %d icin filament tipini girin."), ui_tool),
        _L("Filament Type"),
        m_filament_loaded_tool_materials[m_selected_filament_tool].empty()
            ? wxString("PLA")
            : m_filament_loaded_tool_materials[m_selected_filament_tool]);
    if (material_dialog.ShowModal() != wxID_OK)
        return;

    wxString material = material_dialog.GetValue();
    material.Trim(true);
    material.Trim(false);
    if (material.empty())
        material = "PLA";

    wxTextEntryDialog color_dialog(
        this,
        wxString::Format(_L("Tool %d icin filament rengini HEX olarak girin."), ui_tool),
        _L("Filament Color"),
        hex_from_colour(m_filament_loaded_tool_colors[m_selected_filament_tool]));
    if (color_dialog.ShowModal() != wxID_OK)
        return;

    wxString color_hex = color_dialog.GetValue();
    color_hex.Trim(true);
    color_hex.Trim(false);
    if (!color_hex.StartsWith("#"))
        color_hex = "#" + color_hex;
    if (!looks_like_hex_colour(color_hex)) {
        wxMessageBox(_L("Renk formati #RRGGBB seklinde olmali."), _L("Filament Color"), wxOK | wxICON_WARNING, this);
        return;
    }

    m_filament_loaded_tool_materials[m_selected_filament_tool] = material;
    m_filament_loaded_tool_colors[m_selected_filament_tool] = colour_from_hex(into_u8(color_hex), m_filament_loaded_tool_colors[m_selected_filament_tool]);
    apply_filament_tool_selection(m_selected_filament_tool);
    save_filament_selection_to_moonraker(ui_tool, material, color_hex);
    show_filament_load_wizard();
}

void PrinterWebView::save_filament_selection_to_moonraker(int ui_tool, const wxString &material, const wxString &color_hex)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return;

    ui_tool = std::max(1, std::min(4, ui_tool));
    const std::string material_utf8 = into_u8(material);
    const std::string color_utf8 = into_u8(color_hex);

    std::thread([this, base, ui_tool, material_utf8, color_utf8]() {
        nlohmann::json value = nlohmann::json::object();
        std::string body;
        Http::get(base + "/server/database/item?namespace=coprint&key=filament_selections")
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        if (!body.empty()) {
            auto parsed = nlohmann::json::parse(body, nullptr, false, true);
            if (!parsed.is_discarded()) {
                if (parsed.contains("result"))
                    parsed = parsed["result"];
                if (parsed.is_object() && parsed.contains("value") && parsed["value"].is_object())
                    value = parsed["value"];
            }
        }

        value.erase("toolhead_" + std::to_string(ui_tool - 1));
        value["toolhead_" + std::to_string(ui_tool)] = {
            { "type", material_utf8 },
            { "color_hex", color_utf8 }
        };

        nlohmann::json payload;
        payload["namespace"] = "coprint";
        payload["key"] = "filament_selections";
        payload["value"] = value;

        Http::post(base + "/server/database/item")
            .header("Content-Type", "application/json")
            .set_post_body(payload.dump())
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([](std::string, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: filament selection saved status=" << status;
            })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: filament selection save failed status=" << status << " error=" << error;
            })
            .perform_sync();

        CallAfter([this]() {
            m_filament_preview_fetch_key.clear();
            refresh_filament_preview_from_selected_machine();
        });
    }).detach();
}

void PrinterWebView::clear_filament_selection_from_moonraker(int ui_tool)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return;

    ui_tool = std::max(1, std::min(4, ui_tool));
    std::thread([this, base, ui_tool]() {
        nlohmann::json value = nlohmann::json::object();
        std::string body;
        Http::get(base + "/server/database/item?namespace=coprint&key=filament_selections")
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        if (!body.empty()) {
            auto parsed = nlohmann::json::parse(body, nullptr, false, true);
            if (!parsed.is_discarded()) {
                if (parsed.contains("result"))
                    parsed = parsed["result"];
                if (parsed.is_object() && parsed.contains("value") && parsed["value"].is_object())
                    value = parsed["value"];
            }
        }

        value.erase("toolhead_" + std::to_string(ui_tool));
        value.erase("toolhead_" + std::to_string(ui_tool - 1));

        nlohmann::json payload;
        payload["namespace"] = "coprint";
        payload["key"] = "filament_selections";
        payload["value"] = value;

        Http::post(base + "/server/database/item")
            .header("Content-Type", "application/json")
            .set_post_body(payload.dump())
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([](std::string, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: filament selection cleared status=" << status;
            })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: filament selection clear failed status=" << status << " error=" << error;
            })
            .perform_sync();

        CallAfter([this]() {
            m_filament_preview_fetch_key.clear();
            refresh_filament_preview_from_selected_machine();
        });
    }).detach();
}

void PrinterWebView::refresh_moonraker_status_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    const std::string machine_id = obj != nullptr ? obj->get_dev_id() : std::string();

    if (obj == nullptr || !obj->is_online() || base.empty()) {
        m_has_moonraker_status = false;
        m_moonraker_status_fetch_in_progress = false;
        m_moonraker_status_machine_id.clear();
        return;
    }

    if (m_moonraker_status_machine_id != machine_id) {
        m_has_moonraker_status = false;
        m_moonraker_status_fetch_in_progress = false;
        m_moonraker_status_machine_id = machine_id;
    }

    if (m_moonraker_status_fetch_in_progress)
        return;

    m_moonraker_status_fetch_in_progress = true;
    const std::string query_url = base +
        "/printer/objects/query?extruder=temperature,target"
        "&extruder1=temperature,target"
        "&extruder2=temperature,target"
        "&extruder3=temperature,target"
        "&heater_bed=temperature,target"
        "&fan=speed";

    std::thread([this, machine_id, query_url]() {
        std::string body;
        Http::get(query_url)
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        CallAfter([this, machine_id, body]() {
            m_moonraker_status_fetch_in_progress = false;
            auto *dev_manager = wxGetApp().getDeviceManager();
            MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
            if (obj == nullptr || obj->get_dev_id() != machine_id)
                return;

            auto parsed = nlohmann::json::parse(body, nullptr, false, true);
            if (parsed.is_discarded())
                return;
            if (parsed.contains("result"))
                parsed = parsed["result"];
            if (!parsed.is_object() || !parsed.contains("status") || !parsed["status"].is_object())
                return;

            const auto &status = parsed["status"];
            bool got_any = false;
            for (int i = 0; i < 4; ++i) {
                const std::string object_name = i == 0 ? "extruder" : "extruder" + std::to_string(i);
                if (!status.contains(object_name) || !status[object_name].is_object())
                    continue;
                const auto &tool = status[object_name];
                if (tool.contains("temperature") && tool["temperature"].is_number()) {
                    m_moonraker_nozzle_current[i] = tool["temperature"].get<double>();
                    got_any = true;
                }
                if (tool.contains("target") && tool["target"].is_number()) {
                    m_moonraker_nozzle_target[i] = tool["target"].get<double>();
                    got_any = true;
                }
            }

            if (status.contains("heater_bed") && status["heater_bed"].is_object()) {
                const auto &bed = status["heater_bed"];
                if (bed.contains("temperature") && bed["temperature"].is_number()) {
                    m_moonraker_bed_current = bed["temperature"].get<double>();
                    got_any = true;
                }
                if (bed.contains("target") && bed["target"].is_number()) {
                    m_moonraker_bed_target = bed["target"].get<double>();
                    got_any = true;
                }
            }

            if (status.contains("fan") && status["fan"].is_object()) {
                const auto &fan = status["fan"];
                if (fan.contains("speed") && fan["speed"].is_number()) {
                    m_moonraker_fan_percent = std::clamp(
                        static_cast<int>(std::round(fan["speed"].get<double>() * 100.0)), 0, 100);
                    got_any = true;
                }
            }

            m_has_moonraker_status = got_any;
            if (got_any && m_status_page != nullptr)
                m_status_page->Refresh();
        });
    }).detach();
}

void PrinterWebView::rebuild_filament_tool_popup()
{
    if (m_filament_tool_popup == nullptr || m_filament_tool_popup_panel == nullptr)
        return;

    if (auto *old_sizer = m_filament_tool_popup_panel->GetSizer()) {
        m_filament_tool_popup_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_filament_tool_popup_panel->DestroyChildren();

    m_filament_tool_popup_panel->SetBackgroundColour(wxColour(22, 24, 29));

    const int popup_width = FromDIP(280);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    auto *popup_box = new StaticBox(m_filament_tool_popup_panel, wxID_ANY);
    popup_box->SetMinSize(wxSize(popup_width, -1));
    popup_box->SetCornerRadius(FromDIP(12));
    popup_box->SetBorderWidth(1);
    popup_box->SetBorderColorNormal(wxColour(55, 58, 64));
    popup_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    popup_box->SetBackgroundColour(wxColour(22, 24, 29));

    auto *box_sz = new wxBoxSizer(wxVERTICAL);

    for (int i = 0; i < 4; ++i) {
        auto *row = new wxPanel(popup_box, wxID_ANY);
        row->SetBackgroundColour(wxColour(35, 38, 43));
        row->SetMinSize(wxSize(-1, FromDIP(48)));
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *hs = new wxBoxSizer(wxHORIZONTAL);

        auto *dot = new StaticBox(row, wxID_ANY);
        dot->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
        dot->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
        dot->SetCornerRadius(FromDIP(10));
        dot->SetBorderWidth(0);
        dot->SetBackgroundColorNormal(m_filament_loaded_tool_colors[i]);
        dot->SetBackgroundColour(m_filament_loaded_tool_colors[i]);

        auto *lbl = new wxStaticText(row, wxID_ANY, wxString::Format("Tool %d", i + 1));
        lbl->SetForegroundColour(wxColour(220, 220, 220));
        {
            wxFont f = lbl->GetFont();
            if (f.GetPointSize() > 1)
                f.SetPointSize(f.GetPointSize() + 2);
            f.SetWeight(wxFONTWEIGHT_BOLD);
            lbl->SetFont(f);
        }
        lbl->SetCursor(wxCursor(wxCURSOR_HAND));

        hs->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
        hs->Add(lbl, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
        hs->AddSpacer(FromDIP(16));
        row->SetSizer(hs);

        const auto on_pick = [this, i](wxMouseEvent &) {
            apply_filament_tool_selection(i);
            dismiss_filament_tool_popup();
        };
        row->Bind(wxEVT_LEFT_DOWN, on_pick);
        lbl->Bind(wxEVT_LEFT_DOWN, on_pick);
        dot->Bind(wxEVT_LEFT_DOWN, on_pick);

        box_sz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(i == 0 ? 14 : 8));
    }

    box_sz->AddSpacer(FromDIP(14));
    popup_box->SetSizer(box_sz);

    outer->Add(popup_box, 1, wxEXPAND | wxALL, FromDIP(8));
    m_filament_tool_popup_panel->SetSizer(outer);
    outer->Fit(m_filament_tool_popup_panel);
    m_filament_tool_popup_panel->Layout();

    const wxSize popup_size = m_filament_tool_popup_panel->GetBestSize();
    m_filament_tool_popup_panel->SetSize(popup_size);
    m_filament_tool_popup->SetClientSize(popup_size);
    m_filament_tool_popup->SetSize(popup_size);
    m_filament_tool_popup->Layout();
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

    const wxColour c = m_filament_loaded_tool_colors[tool_index];

    if (m_filament_tool_color_dot != nullptr) {
        m_filament_tool_color_dot->SetBackgroundColorNormal(c);
        m_filament_tool_color_dot->SetBackgroundColour(c);
        m_filament_tool_color_dot->Refresh();
    }
    if (m_filament_tool_name_lbl != nullptr)
        m_filament_tool_name_lbl->SetLabelText(wxString::Format("Tool %d", tool_index + 1));
}

void PrinterWebView::apply_printer_status_tool_selection(int tool_index)
{
    if (tool_index < 0 || tool_index > 3)
        tool_index = 0;

    m_selected_extruder_index = tool_index;
    m_selected_extruder = wxString::Format("T%d", tool_index + 1);
    m_selected_fan = m_selected_extruder;

    if (m_extruder_display_label != nullptr)
        m_extruder_display_label->SetLabelText(m_selected_extruder);
    if (m_fan_display_label != nullptr)
        m_fan_display_label->SetLabelText(m_selected_fan);

    for (int i = 0; i < 4; ++i) {
        const bool active = i == m_selected_extruder_index;
        if (m_ps_tool_cards[i] != nullptr) {
            m_ps_tool_cards[i]->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
            m_ps_tool_cards[i]->Refresh();
        }
        if (m_ps_tool_headers[i] != nullptr) {
            m_ps_tool_headers[i]->SetForegroundColour(active ? wxColour(220, 220, 220) : wxColour(120, 125, 135));
            m_ps_tool_headers[i]->Refresh();
        }
        if (m_axis_tool_buttons[i] != nullptr) {
            m_axis_tool_buttons[i]->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(61, 64, 68));
            m_axis_tool_buttons[i]->SetBackgroundColorNormal(wxColour(61, 64, 68));
            m_axis_tool_buttons[i]->SetTextColorNormal(active ? wxColour(235, 235, 235) : wxColour(215, 215, 215));
            m_axis_tool_buttons[i]->Refresh();
        }
    }

    refresh_fan_value_display();
    refresh_layer_info_from_selected_machine();
    Layout();
}

void PrinterWebView::show_toolhead_temperature_dialog(int active_extruder_index)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->GetExtderSystem() == nullptr)
        return;

    active_extruder_index = std::max(0, std::min(3, active_extruder_index));

    long min_t = 0;
    long max_t = 300;
    if (obj->nozzle_temp_range.size() >= 2) {
        min_t = obj->nozzle_temp_range[0];
        max_t = obj->nozzle_temp_range[1];
    }

    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(wxColour("#000000"));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *dialog_shell = new StaticBox(&dlg, wxID_ANY);
    dialog_shell->SetCornerRadius(FromDIP(10));
    dialog_shell->SetBorderWidth(1);
    dialog_shell->SetBorderColorNormal(wxColour("#D9DBDB"));
    dialog_shell->SetBackgroundColorNormal(wxColour("#F7F7F5"));
    dialog_shell->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *shell_sz = new wxBoxSizer(wxVERTICAL);

    auto *title_bar = new wxPanel(dialog_shell, wxID_ANY);
    title_bar->SetBackgroundColour(wxColour("#252D31"));
    auto *title_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *title_txt = new wxStaticText(title_bar, wxID_ANY, _L("Change Toolhead Temperature"));
    title_txt->SetForegroundColour(*wxWHITE);
    {
        wxFont tf = title_txt->GetFont();
        if (tf.GetPointSize() > 1)
            tf.SetPointSize(tf.GetPointSize() + 1);
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title_txt->SetFont(tf);
    }
    title_sz->Add(title_txt, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
    auto *close_btn = new wxButton(title_bar, wxID_ANY, wxString::FromUTF8("\u00D7"), wxDefaultPosition, wxSize(FromDIP(34), FromDIP(34)), wxBORDER_NONE);
    close_btn->SetBackgroundColour(wxColour("#252D31"));
    close_btn->SetForegroundColour(*wxWHITE);
    title_sz->Add(close_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    title_bar->SetSizer(title_sz);
    title_bar->SetMinSize(wxSize(-1, FromDIP(38)));
    shell_sz->Add(title_bar, 0, wxEXPAND);

    auto *body = new wxPanel(dialog_shell);
    body->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *body_sz = new wxBoxSizer(wxHORIZONTAL);

    auto *left = new wxPanel(body, wxID_ANY);
    left->SetBackgroundColour(wxColour("#F7F7F5"));
    left->SetMinSize(wxSize(FromDIP(190), -1));
    auto *left_sz = new wxBoxSizer(wxVERTICAL);

    auto *active_card = new StaticBox(left, wxID_ANY);
    active_card->SetMinSize(wxSize(FromDIP(152), FromDIP(98)));
    active_card->SetCornerRadius(FromDIP(10));
    active_card->SetBorderWidth(1);
    active_card->SetBorderColorNormal(wxColour("#ECEDEC"));
    active_card->SetBackgroundColorNormal(wxColour("#FBFBFA"));
    active_card->SetBackgroundColour(wxColour("#FBFBFA"));
    auto *active_card_sz = new wxBoxSizer(wxVERTICAL);

    auto *tool_hdr_wrap = new wxPanel(active_card, wxID_ANY);
    tool_hdr_wrap->SetBackgroundColour(wxColour("#F2F3F1"));
    auto *thw_sz = new wxBoxSizer(wxVERTICAL);
    auto *tool_hdr = new wxStaticText(tool_hdr_wrap, wxID_ANY, wxString::Format("Tool %d", active_extruder_index + 1));
    tool_hdr->SetBackgroundColour(wxColour("#F2F3F1"));
    tool_hdr->SetForegroundColour(wxColour("#4C4E50"));
    {
        wxFont hf = tool_hdr->GetFont();
        hf.SetWeight(wxFONTWEIGHT_BOLD);
        tool_hdr->SetFont(hf);
    }
    thw_sz->Add(tool_hdr, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(10));
    tool_hdr_wrap->SetSizer(thw_sz);
    active_card_sz->Add(tool_hdr_wrap, 0, wxEXPAND);

    const float cur_f = obj->GetExtderSystem()->GetNozzleTempCurrent(active_extruder_index);
    const float tgt_f = obj->GetExtderSystem()->GetNozzleTempTarget(active_extruder_index);

    auto *temp_row = new wxBoxSizer(wxHORIZONTAL);
    wxBitmap therm = create_scaled_bitmap("tool_temperature_popup", &dlg, 24);
    if (therm.IsOk())
        temp_row->Add(new wxStaticBitmap(active_card, wxID_ANY, therm), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto *big_cur = new wxStaticText(active_card, wxID_ANY, wxString::Format("%.0f", static_cast<double>(cur_f)));
    {
        wxFont bf = big_cur->GetFont();
        bf.SetPointSize(std::max(18, bf.GetPointSize() + 8));
        bf.SetWeight(wxFONTWEIGHT_BOLD);
        big_cur->SetFont(bf);
    }
    big_cur->SetForegroundColour(wxColour("#596068"));
    temp_row->Add(big_cur, 0, wxALIGN_CENTER_VERTICAL);
    temp_row->Add(new wxStaticText(active_card, wxID_ANY, "/"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(2));
    auto *big_tgt = new wxStaticText(active_card, wxID_ANY,
                                     wxString::Format("%.0f %s", static_cast<double>(tgt_f), wxString::FromUTF8("\xC2\xB0""C")));
    {
        wxFont sf = big_tgt->GetFont();
        if (sf.GetPointSize() > 1)
            sf.SetPointSize(sf.GetPointSize() + 1);
        big_tgt->SetFont(sf);
    }
    big_tgt->SetForegroundColour(wxColour("#596068"));
    temp_row->Add(big_tgt, 0, wxALIGN_CENTER_VERTICAL);
    active_card_sz->Add(temp_row, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(14));
    active_card->SetSizer(active_card_sz);
    left_sz->Add(active_card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    wxString init_val = wxString::Format("%.0f%s", static_cast<double>(tgt_f), wxString::FromUTF8("\xC2\xB0""C"));
    auto *input_wrap = new StaticBox(left, wxID_ANY);
    input_wrap->SetCornerRadius(FromDIP(8));
    input_wrap->SetBorderWidth(1);
    input_wrap->SetBorderColorNormal(wxColour("#E0E2E2"));
    input_wrap->SetBackgroundColorNormal(*wxWHITE);
    input_wrap->SetBackgroundColour(*wxWHITE);
    input_wrap->SetMinSize(wxSize(FromDIP(112), FromDIP(29)));
    auto *input_wrap_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *inp = new wxTextCtrl(input_wrap, wxID_ANY, init_val, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | wxBORDER_NONE);
    inp->SetBackgroundColour(*wxWHITE);
    inp->SetForegroundColour(wxColour("#4F555A"));
    input_wrap_sz->Add(inp, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));
    input_wrap->SetSizer(input_wrap_sz);

    auto *set_btn = new StaticBox(left, wxID_ANY);
    set_btn->SetMinSize(wxSize(FromDIP(50), FromDIP(29)));
    set_btn->SetCornerRadius(FromDIP(7));
    set_btn->SetBorderWidth(0);
    set_btn->SetBackgroundColorNormal(wxColour("#4E4E4E"));
    set_btn->SetBackgroundColour(wxColour("#4E4E4E"));
    set_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *set_btn_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *set_label = new wxStaticText(set_btn, wxID_ANY, _L("Set"));
    set_label->SetForegroundColour(*wxWHITE);
    set_label->SetCursor(wxCursor(wxCURSOR_HAND));
    set_btn_sz->AddStretchSpacer(1);
    set_btn_sz->Add(set_label, 0, wxALIGN_CENTER_VERTICAL);
    set_btn_sz->AddStretchSpacer(1);
    set_btn->SetSizer(set_btn_sz);
    auto *inp_row = new wxBoxSizer(wxHORIZONTAL);
    inp_row->Add(input_wrap, 0, wxALIGN_CENTER_VERTICAL);
    inp_row->Add(set_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    left_sz->Add(inp_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    left_sz->AddStretchSpacer(1);
    left->SetSizer(left_sz);

    auto *right = new wxPanel(body, wxID_ANY);
    right->SetBackgroundColour(wxColour("#F7F7F5"));
    right->SetMinSize(wxSize(FromDIP(190), -1));
    auto *right_sz = new wxBoxSizer(wxVERTICAL);

    constexpr int k_select_tool_base_id = wxID_HIGHEST + 300;
    for (int i = 0; i < 4; ++i) {
        if (i == active_extruder_index)
            continue;
        const float oc = obj->GetExtderSystem()->GetNozzleTempCurrent(i);
        const float ot = obj->GetExtderSystem()->GetNozzleTempTarget(i);
        auto *row = new StaticBox(right, wxID_ANY);
        row->SetMinSize(wxSize(FromDIP(190), FromDIP(31)));
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        row->SetCornerRadius(FromDIP(8));
        row->SetBorderWidth(1);
        row->SetBorderColorNormal(wxColour("#ECEDEC"));
        row->SetBackgroundColorNormal(wxColour("#FBFBFA"));
        row->SetBackgroundColour(wxColour("#FBFBFA"));
        auto *rs = new wxBoxSizer(wxHORIZONTAL);

        auto *pill = new wxPanel(row, wxID_ANY);
        pill->SetCursor(wxCursor(wxCURSOR_HAND));
        pill->SetBackgroundColour(wxColour("#F2F3F1"));
        auto *ps = new wxBoxSizer(wxVERTICAL);
        auto *pn = new wxStaticText(pill, wxID_ANY, wxString::Format("Tool %d", i + 1));
        pn->SetCursor(wxCursor(wxCURSOR_HAND));
        pn->SetBackgroundColour(wxColour("#F2F3F1"));
        pn->SetForegroundColour(wxColour("#4C4E50"));
        {
            wxFont pf = pn->GetFont();
            pf.SetWeight(wxFONTWEIGHT_BOLD);
            pn->SetFont(pf);
        }
        ps->Add(pn, 0, wxALL, FromDIP(8));
        pill->SetSizer(ps);
        rs->Add(pill, 0, wxALIGN_CENTER_VERTICAL);
        rs->AddSpacer(FromDIP(10));
        wxBitmap sm = create_scaled_bitmap("tool_temperature_popup", &dlg, 16);
        wxStaticBitmap *sm_icon = nullptr;
        if (sm.IsOk())
            sm_icon = new wxStaticBitmap(row, wxID_ANY, sm);
        if (sm_icon != nullptr) {
            sm_icon->SetCursor(wxCursor(wxCURSOR_HAND));
            rs->Add(sm_icon, 0, wxALIGN_CENTER_VERTICAL);
        }
        auto *tx = new wxStaticText(row, wxID_ANY,
                                    wxString::Format("%.0f/%.0f %s", static_cast<double>(oc), static_cast<double>(ot),
                                                     wxString::FromUTF8("\xC2\xB0""C")));
        tx->SetCursor(wxCursor(wxCURSOR_HAND));
        tx->SetForegroundColour(wxColour("#596068"));
        rs->Add(tx, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        row->SetSizer(rs);
        right_sz->Add(row, 0, wxEXPAND | wxTOP, FromDIP(8));
        const int tool_index = i;
        auto select_tool = [&dlg, tool_index, k_select_tool_base_id](wxMouseEvent &evt) {
            evt.StopPropagation();
            dlg.EndModal(k_select_tool_base_id + tool_index);
        };
        row->Bind(wxEVT_LEFT_DOWN, select_tool);
        pill->Bind(wxEVT_LEFT_DOWN, select_tool);
        pn->Bind(wxEVT_LEFT_DOWN, select_tool);
        if (sm_icon != nullptr)
            sm_icon->Bind(wxEVT_LEFT_DOWN, select_tool);
        tx->Bind(wxEVT_LEFT_DOWN, select_tool);
    }
    right_sz->AddStretchSpacer(1);
    right->SetSizer(right_sz);

    body_sz->Add(left, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, FromDIP(14));
    body_sz->AddSpacer(FromDIP(14));
    body_sz->Add(right, 0, wxEXPAND | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(14));
    body->SetSizer(body_sz);
    shell_sz->Add(body, 1, wxEXPAND);

    dialog_shell->SetSizer(shell_sz);
    root->Add(dialog_shell, 1, wxEXPAND | wxALL, FromDIP(1));
    dlg.SetSizer(root);

    const auto apply_temp = [&]() {
        wxString raw = inp->GetValue();
        raw.Trim(true);
        raw.Trim(false);
        raw.Replace(wxString::FromUTF8("\xC2\xB0""C"), wxEmptyString, true);
        raw.Replace("C", wxEmptyString, true);
        raw.Trim(true);
        raw.Trim(false);
        long v = 0;
        if (!raw.ToLong(&v)) {
            wxMessageBox(_L("Please enter a valid temperature."), _L("Change Toolhead Temperature"), wxOK | wxICON_WARNING, &dlg);
            return;
        }
        v = std::max(min_t, std::min(max_t, v));
        obj->command_set_nozzle_new(active_extruder_index, static_cast<int>(v));
        dlg.EndModal(wxID_OK);
    };

    close_btn->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    set_btn->Bind(wxEVT_LEFT_DOWN, [&apply_temp](wxMouseEvent &) { apply_temp(); });
    set_label->Bind(wxEVT_LEFT_DOWN, [&apply_temp](wxMouseEvent &) { apply_temp(); });
    inp->Bind(wxEVT_TEXT_ENTER, [&apply_temp](wxCommandEvent &) { apply_temp(); });
    dlg.Bind(wxEVT_CLOSE_WINDOW, [&dlg](wxCloseEvent &e) {
        if (dlg.IsModal())
            dlg.EndModal(wxID_CANCEL);
        else
            e.Skip();
    });

    dlg.Fit();
    dlg.SetMinSize(dlg.GetSize());
    {
        const wxSize size = dlg.GetSize();
        wxBitmap shape_bmp(size.GetWidth(), size.GetHeight());
        wxMemoryDC dc(shape_bmp);
        dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
        dc.Clear();
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), FromDIP(10));
        dc.SelectObject(wxNullBitmap);

        wxRegion region(shape_bmp, wxColour(0, 0, 0));
        if (region.IsOk())
            dlg.SetShape(region);
    }
    dlg.CentreOnParent();
    const int modal_result = dlg.ShowModal();
    if (modal_result >= k_select_tool_base_id && modal_result < k_select_tool_base_id + 4) {
        const int selected_tool = modal_result - k_select_tool_base_id;
        CallAfter([this, selected_tool]() {
            show_toolhead_temperature_dialog(selected_tool);
        });
    }
}

void PrinterWebView::prompt_ps_target_temperature(bool is_bed, int extruder_index)
{
    if (!is_bed) {
        show_toolhead_temperature_dialog(extruder_index);
        return;
    }

    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online())
        return;

    long min_t = 0;
    long max_t = 300;
    long cur = 0;

    if (obj->GetBed() != nullptr)
        cur = static_cast<long>(std::nearbyint(static_cast<double>(obj->GetBed()->GetBedTempTarget())));
    max_t = obj->get_bed_temperature_limit();
    if (obj->bed_temp_range.size() >= 2) {
        min_t = obj->bed_temp_range[0];
        max_t = obj->bed_temp_range[1];
    }

    wxTextEntryDialog dlg(this, _L("Enter new bed target temperature (°C)."), _L("Bed target temperature"), wxString::Format("%ld", cur));
    if (dlg.ShowModal() != wxID_OK)
        return;
    long v = 0;
    if (!dlg.GetValue().ToLong(&v))
        return;
    v = std::max(min_t, std::min(max_t, v));
    obj->command_set_bed(static_cast<int>(v));
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
    dismiss_filament_tool_popup();
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
            const wxSize target = preview_thumbnail_target_size(m_preview_thumbnail, FromDIP(120), FromDIP(120));
            m_preview_thumbnail->SetBitmap(wxBitmap(scale_preview_thumbnail(plate_image, target.GetWidth(), target.GetHeight())));
            Layout();
            return;
        }
        }
    }

    const wxString logo_path = from_u8(Slic3r::resources_dir() + "/images/logo.jpg");
    wxImage logo_image;
    if (logo_image.LoadFile(logo_path, wxBITMAP_TYPE_JPEG)) {
        const wxSize target = preview_thumbnail_target_size(m_preview_thumbnail, FromDIP(110), FromDIP(60));
        wxImage resized = scale_preview_thumbnail(logo_image, target.GetWidth(), target.GetHeight());
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
            const wxSize target = preview_thumbnail_target_size(m_preview_thumbnail, FromDIP(120), FromDIP(120));
            wxImage resized = scale_preview_thumbnail(m_thumbnail_image, target.GetWidth(), target.GetHeight());
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
            m_thumbnail_image = local_image;
            const wxSize target = preview_thumbnail_target_size(m_preview_thumbnail, FromDIP(120), FromDIP(120));
            m_preview_thumbnail->SetBitmap(wxBitmap(scale_preview_thumbnail(local_image, target.GetWidth(), target.GetHeight())));
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


void PrinterWebView::handle_dashboard_command(const DeviceDashboard::DeviceCommand &command)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online())
        return;

    switch (command.kind) {
    case DeviceDashboard::DeviceCommandKind::PausePrint:
        if (obj->can_resume())
            obj->command_task_resume();
        else
            obj->command_task_pause();
        break;
    case DeviceDashboard::DeviceCommandKind::ResumePrint:
        obj->command_task_resume();
        break;
    case DeviceDashboard::DeviceCommandKind::StopPrint:
        obj->command_task_abort();
        break;
    case DeviceDashboard::DeviceCommandKind::MoveAxis: {
        std::string axis;
        int speed = 3000;
        switch (command.axis) {
        case DeviceDashboard::Axis::X: axis = "X"; break;
        case DeviceDashboard::Axis::Y: axis = "Y"; break;
        case DeviceDashboard::Axis::Z:
            axis = "Z";
            speed = 900;
            break;
        }
        if (!axis.empty())
            obj->command_axis_control(axis, 1.0, command.value, speed);
        break;
    }
    default:
        break;
    }
}

void PrinterWebView::refresh_layer_info_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    refresh_moonraker_status_from_selected_machine();
    m_dashboard_state_store.set_state(DeviceDashboard::DashboardStateAdapter::from_machine(obj));
    if (m_dashboard_print_status_panel != nullptr)
        m_dashboard_print_status_panel->apply_state(m_dashboard_state_store.state().print_job);
    bool printer_status_values_changed = false;
    auto set_label_if_changed = [](wxStaticText *label, const wxString &text) -> bool {
        if (label == nullptr || label->GetLabelText() == text)
            return false;
        label->SetLabelText(text);
        return true;
    };

    if (m_filament_load_btn != nullptr && m_filament_unload_btn != nullptr) {
        const bool allow_filament_ops = obj != nullptr && obj->is_online() && !obj->is_in_printing();
        m_filament_load_btn->Enable(allow_filament_ops);
        m_filament_unload_btn->Enable(allow_filament_ops);
        if (m_filament_tool_selector != nullptr)
            m_filament_tool_selector->Enable(allow_filament_ops);
        if (m_manage_filament_title != nullptr)
            m_manage_filament_title->Enable(allow_filament_ops);
    }

    update_preview_thumbnail(obj);
    refresh_filament_preview_from_selected_machine();

    if (m_connected_printer_panel != nullptr && m_connected_printer_status_label != nullptr && m_connected_printer_logout_label != nullptr) {
        const bool has_connected_printer = m_has_active_printer_connection && obj != nullptr && obj->is_online();
        bool connected_header_changed = false;
        wxFont st_font = m_connected_printer_status_label->GetFont();
        if (has_connected_printer) {
            connected_header_changed |= set_label_if_changed(m_connected_printer_status_label, from_u8(obj->get_dev_name()));
            m_connected_printer_status_label->SetForegroundColour(wxColour(235, 235, 235));
            st_font.SetWeight(wxFONTWEIGHT_BOLD);
        } else {
            connected_header_changed |= set_label_if_changed(m_connected_printer_status_label, wxString::FromUTF8("Ba\xC4\x9Fl\xC4\xB1 yaz\xC4\xB1""c\xC4\xB1 yok"));
            m_connected_printer_status_label->SetForegroundColour(wxColour(150, 156, 166));
            st_font.SetWeight(wxFONTWEIGHT_NORMAL);
        }
        m_connected_printer_status_label->SetFont(st_font);
        connected_header_changed |= m_connected_printer_logout_label->IsShown() != has_connected_printer;
        m_connected_printer_logout_label->Show(has_connected_printer);
        if (connected_header_changed && m_connected_printer_panel->GetParent() != nullptr) {
            m_connected_printer_panel->GetParent()->Layout();
            m_connected_printer_panel->GetParent()->Refresh();
        }
    }

    if (m_bed_temp_value != nullptr) {
        wxString bed_text = "__ / __";
        if (m_has_moonraker_status)
            bed_text = wxString::Format("%.1f / %.1f", m_moonraker_bed_current, m_moonraker_bed_target);
        else if (obj != nullptr && obj->GetBed() != nullptr)
            bed_text = wxString::Format("%.1f / %.1f", obj->GetBed()->GetBedTemp(), obj->GetBed()->GetBedTempTarget());
        set_label_if_changed(m_bed_temp_value, bed_text);
    }

    if (m_extruder_temp_value != nullptr) {
        wxString nozzle_text = "__ / __";
        if (m_has_moonraker_status)
            nozzle_text = wxString::Format("%.1f / %.1f",
                m_moonraker_nozzle_current[m_selected_extruder_index],
                m_moonraker_nozzle_target[m_selected_extruder_index]);
        else if (obj != nullptr && obj->GetExtderSystem() != nullptr)
            nozzle_text = wxString::Format(
                "%.1f / %.1f",
                static_cast<double>(obj->GetExtderSystem()->GetNozzleTempCurrent(m_selected_extruder_index)),
                static_cast<double>(obj->GetExtderSystem()->GetNozzleTempTarget(m_selected_extruder_index)));
        set_label_if_changed(m_extruder_temp_value, nozzle_text);
    }

    if (m_fan_value_label != nullptr && m_has_moonraker_status) {
        m_selected_fan_value = wxString::Format("%d", m_moonraker_fan_percent);
        set_label_if_changed(m_fan_value_label, m_selected_fan_value);
    } else if (m_fan_value_label != nullptr && obj != nullptr && obj->GetFan() != nullptr) {
        m_selected_fan_value = wxString::Format("%d", (int)std::round(obj->GetFan()->GetCoolingFanSpeed() / 25.5f));
        set_label_if_changed(m_fan_value_label, m_selected_fan_value);
    }

    if (m_ps_bed_temp_label != nullptr) {
        wxString bed_text = "-- / --";
        if (m_has_moonraker_status)
            bed_text = wxString::Format("%.1f / %.1f", m_moonraker_bed_current, m_moonraker_bed_target);
        else if (obj != nullptr && obj->GetBed() != nullptr)
            bed_text = wxString::Format("%.1f / %.1f", obj->GetBed()->GetBedTemp(), obj->GetBed()->GetBedTempTarget());
        printer_status_values_changed |= set_label_if_changed(m_ps_bed_temp_label, bed_text);
    }
    std::array<wxStaticText *, 4> ps_temp_labels { m_ps_t1_temp_label, m_ps_t2_temp_label, m_ps_t3_temp_label, m_ps_t4_temp_label };
    std::array<wxStaticText *, 4> ps_fan_labels { m_ps_t1_fan_label, m_ps_t2_fan_label, m_ps_t3_fan_label, m_ps_t4_fan_label };
    for (int i = 0; i < 4; ++i) {
        if (ps_temp_labels[i] != nullptr) {
            wxString temp_text = "-- / --";
            if (m_has_moonraker_status)
                temp_text = wxString::Format("%.1f / %.1f", m_moonraker_nozzle_current[i], m_moonraker_nozzle_target[i]);
            else if (obj != nullptr && obj->GetExtderSystem() != nullptr)
                temp_text = wxString::Format("%.1f / %.1f",
                    static_cast<double>(obj->GetExtderSystem()->GetNozzleTempCurrent(i)),
                    static_cast<double>(obj->GetExtderSystem()->GetNozzleTempTarget(i)));
            const bool changed = set_label_if_changed(ps_temp_labels[i], temp_text);
            printer_status_values_changed |= changed;
            if (changed && ps_temp_labels[i]->GetParent() != nullptr)
                ps_temp_labels[i]->GetParent()->Layout();
        }
        if (ps_fan_labels[i] != nullptr) {
            wxString fan_text = "--%";
            if (m_has_moonraker_status) {
                fan_text = wxString::Format("%d%%", m_moonraker_fan_percent);
            } else if (obj != nullptr && obj->GetExtderSystem() != nullptr) {
                const float per_tool_fan = obj->GetExtderSystem()->GetNozzleFanSpeed(i);
                if (per_tool_fan >= 0.0f) {
                    // Per-tool fan speed available (e.g. Quadro fan_generic fan_t0..t3), 0.0–1.0
                    fan_text = wxString::Format("%d%%", (int)std::round(per_tool_fan * 100.0f));
                } else if (obj->GetFan() != nullptr) {
                    // Fall back to single shared fan (standard single-extruder printer)
                    fan_text = wxString::Format("%d%%", (int)std::round(obj->GetFan()->GetCoolingFanSpeed() / 25.5f));
                }
            }
            const bool changed = set_label_if_changed(ps_fan_labels[i], fan_text);
            printer_status_values_changed |= changed;
            if (changed && ps_fan_labels[i]->GetParent() != nullptr)
                ps_fan_labels[i]->GetParent()->Layout();
        }
    }
    if (printer_status_values_changed && m_status_page != nullptr) {
        m_status_page->Refresh();
    }

    if (m_printer_name_value != nullptr)
        set_label_if_changed(m_printer_name_value, obj != nullptr ? from_u8(obj->get_dev_name()) : "N/A");
    if (m_printer_model_value != nullptr)
        set_label_if_changed(m_printer_model_value, obj != nullptr ? obj->get_printer_type_display_str() : "N/A");
    if (m_printer_serial_value != nullptr) {
        wxString serial = obj != nullptr ? from_u8(obj->get_dev_id()) : "N/A";
        set_label_if_changed(m_printer_serial_value, serial.MakeUpper());
    }
    if (m_printer_firmware_value != nullptr) {
        wxString version = "N/A";
        if (obj != nullptr) {
            auto ota_it = obj->module_vers.find("ota");
            version = ota_it != obj->module_vers.end() ? ota_it->second.sw_ver : from_u8(obj->get_ota_version());
            if (version.empty())
                version = "N/A";
        }
        set_label_if_changed(m_printer_firmware_value, version);
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
