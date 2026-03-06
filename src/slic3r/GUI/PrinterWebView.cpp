#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {
    SetBackgroundColour(wxColour(28, 30, 34));

    auto *main_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *left_empty = new wxPanel(this, wxID_ANY);
    left_empty->SetBackgroundColour(wxColour(28, 30, 34));

    auto *right_container = new wxPanel(this, wxID_ANY);
    right_container->SetBackgroundColour(wxColour(28, 30, 34));
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
        btn->SetMinSize(wxSize(this->FromDIP(92), this->FromDIP(48)));
        btn->SetCornerRadius(this->FromDIP(8));
        btn->SetBorderWidth(0);
        btn->SetBackgroundColorNormal(active ? wxColour(210, 210, 210) : wxColour(61, 64, 68));
        btn->SetTextColorNormal(active ? wxColour(40, 40, 40) : wxColour(215, 215, 215));
        return btn;
    };

    auto *step_bg = new StaticBox(right_container, wxID_ANY);
    step_bg->SetCornerRadius(FromDIP(8));
    step_bg->SetBorderWidth(0);
    step_bg->SetBackgroundColorNormal(wxColour(35, 38, 43));

    auto *step_row = new wxBoxSizer(wxHORIZONTAL);
    step_row->Add(make_step_btn(step_bg, "1mm", true), 0, wxRIGHT, FromDIP(8));
    step_row->Add(make_step_btn(step_bg, "5mm"), 0, wxRIGHT, FromDIP(8));
    step_row->Add(make_step_btn(step_bg, "10mm"), 0);
    auto *step_bg_sizer = new wxBoxSizer(wxVERTICAL);
    step_bg_sizer->Add(step_row, 0, wxALL, FromDIP(8));
    step_bg->SetSizer(step_bg_sizer);
    right_sizer->Add(step_bg, 0, wxTOP | wxLEFT | wxRIGHT | wxALIGN_CENTER_HORIZONTAL, FromDIP(12));

    auto *content_row = new wxBoxSizer(wxHORIZONTAL);

    auto make_tool_btn = [this](wxWindow *parent, const wxString &txt, bool active = false) {
        auto *btn = new Button(parent, txt);
        btn->SetMinSize(wxSize(this->FromDIP(92), this->FromDIP(58)));
        btn->SetCornerRadius(this->FromDIP(10));
        btn->SetBorderWidth(0);
        btn->SetBackgroundColorNormal(active ? wxColour(210, 210, 210) : wxColour(61, 64, 68));
        btn->SetTextColorNormal(active ? wxColour(40, 40, 40) : wxColour(215, 215, 215));
        return btn;
    };

    auto *tool_col = new wxBoxSizer(wxVERTICAL);
    tool_col->Add(make_tool_btn(right_container, "T1", true), 0, wxBOTTOM, FromDIP(10));
    tool_col->Add(make_tool_btn(right_container, "T2"), 0, wxBOTTOM, FromDIP(10));
    tool_col->Add(make_tool_btn(right_container, "T3"), 0, wxBOTTOM, FromDIP(10));
    tool_col->Add(make_tool_btn(right_container, "T4"), 0);
    content_row->Add(tool_col, 0, wxRIGHT, FromDIP(16));

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

    const int xy_square = FromDIP(261);
    const int center_size = FromDIP(80);
    const int center_pos = (xy_square - center_size) / 2;
    const int gap = FromDIP(2);

    const int top_w = FromDIP(220);
    const int top_h = FromDIP(75);
    const int side_w = FromDIP(90);
    const int side_h = FromDIP(220);
    const int left_side_inset_x = FromDIP(0);
    const int top_center_x = center_pos + (center_size - top_w) / 2;
    const int side_shift_y = FromDIP(8);

    auto *xy_area = new wxPanel(right_container, wxID_ANY, wxDefaultPosition, wxSize(xy_square, xy_square));
    xy_area->SetMinSize(wxSize(xy_square, xy_square));
    xy_area->SetMaxSize(wxSize(xy_square, xy_square));
    xy_area->SetBackgroundColour(wxColour(28, 30, 34));

    auto add_axis_icon = [this](wxWindow *parent, const std::string &icon_key, int x, int y, int box_w, int box_h) {
        if (icon_key.empty())
            return;
        const int pad = this->FromDIP(0);
        int holder_w = box_w - pad * 2;
        int holder_h = box_h - pad * 2;
        if (holder_w < this->FromDIP(1)) holder_w = this->FromDIP(1);
        if (holder_h < this->FromDIP(1)) holder_h = this->FromDIP(1);

        auto *holder = new wxPanel(parent, wxID_ANY, wxPoint(x + pad, y + pad), wxSize(holder_w, holder_h));
        holder->SetBackgroundColour(wxColour(28, 30, 34));

        auto *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->AddStretchSpacer(1);
        const int icon_target = holder_h;
        const int icon_px = this->ToDIP(wxSize(0, icon_target)).GetHeight();
        auto bmp = create_scaled_bitmap(icon_key, this, icon_px > 0 ? icon_px : 1);
        auto *icon = new wxStaticBitmap(holder, wxID_ANY, bmp);
        sizer->Add(icon, 0, wxALIGN_CENTER);
        sizer->AddStretchSpacer(1);
        holder->SetSizer(sizer);
    };

    add_axis_icon(xy_area, resolve_icon("vector10", ""), center_pos - gap - side_w + left_side_inset_x, (xy_square - side_h) / 2 + side_shift_y, side_w, side_h);
    add_axis_icon(xy_area, resolve_icon("vector12", ""), center_pos + center_size + gap, (xy_square - side_h) / 2 + side_shift_y, side_w, side_h);
    add_axis_icon(xy_area, resolve_icon("vector11", ""), top_center_x, center_pos - gap - top_h, top_w, top_h);
    add_axis_icon(xy_area, resolve_icon("vector13", ""), top_center_x, center_pos + center_size + gap, top_w, top_h);

    std::string center_icon = resolve_icon("monitor_axis_home_icon", "monitor_axis_home");
    auto *center_btn = new Button(xy_area, "", center_icon.empty() ? wxString() : from_u8(center_icon), 0, 38);
    center_btn->SetSize(wxRect(wxPoint(center_pos, center_pos), wxSize(center_size, center_size)));
    center_btn->SetMinSize(wxSize(center_size, center_size));
    center_btn->SetMaxSize(wxSize(center_size, center_size));
    center_btn->SetCornerRadius(FromDIP(7));
    center_btn->SetBorderWidth(0);
    center_btn->SetBackgroundColorNormal(wxColour(255, 255, 255));
    center_btn->SetBackgroundColour(wxColour(28, 30, 34));

    content_row->Add(xy_area, 0, wxRIGHT, FromDIP(16));

    auto make_icon_btn = [this, &make_btn](const std::string &icon_key, int w, int h, bool transparent_bg = false, int icon_w = -1, int icon_h = -1) {
        auto *btn = make_btn("", w, h, true);
        if (transparent_bg) {
            btn->SetBackgroundColour(wxColour(28, 30, 34));
            btn->SetForegroundColour(wxColour(28, 30, 34));
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
    z_col->Add(top_btn, 0, wxLEFT | wxBOTTOM, FromDIP(10));

    auto *center_home_box = new Button(right_container, "", "home", 0, 40);
    center_home_box->SetMinSize(wxSize(FromDIP(80), FromDIP(75)));
    center_home_box->SetMaxSize(wxSize(FromDIP(80), FromDIP(75)));
    center_home_box->SetCornerRadius(FromDIP(15));
    center_home_box->SetBorderWidth(0);
    center_home_box->SetBackgroundColorNormal(wxColour(255, 255, 255));
    center_home_box->SetBackgroundColour(wxColour(28, 30, 34));

    z_col->Add(center_home_box, 0, wxLEFT | wxBOTTOM, FromDIP(15));
    auto *bottom_btn = make_icon_btn(resolve_icon("rectangle_12", ""), 90, 75, true);
    z_col->Add(bottom_btn, 0, wxLEFT, FromDIP(10));
    content_row->Add(z_col, 0, wxALIGN_CENTER_VERTICAL);

    right_sizer->Add(content_row, 0, wxALL, FromDIP(12));
    right_sizer->AddStretchSpacer(1);
    right_container->SetSizer(right_sizer);

    main_sizer->Add(left_empty, 1, wxEXPAND);
    main_sizer->Add(right_container, 0, wxEXPAND | wxALL, FromDIP(10));
    SetSizer(main_sizer);

    m_browser = nullptr;
    m_zoomFactor = 100;
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);
 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
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
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    return;
}

void PrinterWebView::update_mode()
{
    return;
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

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
