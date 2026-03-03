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

    auto make_axis_cell = [this, right_container, icon_exists](const wxString &fallback_text, const std::string &primary_icon, const std::string &secondary_icon, int w, int h) {
        auto *cell = new wxPanel(right_container, wxID_ANY, wxDefaultPosition, wxSize(this->FromDIP(w), this->FromDIP(h)));
        cell->SetBackgroundColour(wxColour(28, 30, 34));

        auto *cell_sizer = new wxBoxSizer(wxVERTICAL);
        cell_sizer->AddStretchSpacer(1);
        std::string icon_key;
        if (icon_exists(primary_icon))
            icon_key = primary_icon;
        else if (!secondary_icon.empty() && icon_exists(secondary_icon))
            icon_key = secondary_icon;

        if (!icon_key.empty()) {
            auto bmp = create_scaled_bitmap(icon_key, this, 30);
            auto *icon = new wxStaticBitmap(cell, wxID_ANY, bmp);
            cell_sizer->Add(icon, 0, wxALIGN_CENTER);
        } else {
            auto *txt = new wxStaticText(cell, wxID_ANY, fallback_text);
            txt->SetForegroundColour(wxColour(210, 210, 210));
            cell_sizer->Add(txt, 0, wxALIGN_CENTER);
        }
        cell_sizer->AddStretchSpacer(1);
        cell->SetSizer(cell_sizer);
        return cell;
    };

    auto *xy_grid = new wxGridSizer(3, 3, FromDIP(10), FromDIP(10));
    xy_grid->AddSpacer(FromDIP(10));
    xy_grid->Add(make_axis_cell("Y+", "vector_11", "", 118, 78), 0, wxALIGN_CENTER);
    xy_grid->AddSpacer(FromDIP(10));
    xy_grid->Add(make_axis_cell("X-", "vector_10", "", 92, 130), 0, wxALIGN_CENTER);
    xy_grid->Add(make_btn("\u2302", 92, 92, true), 0, wxALIGN_CENTER);
    xy_grid->Add(make_axis_cell("X+", "12", "vector_12", 92, 130), 0, wxALIGN_CENTER);
    xy_grid->AddSpacer(FromDIP(10));
    xy_grid->Add(make_axis_cell("Y-", "13", "vector_13", 118, 78), 0, wxALIGN_CENTER);
    xy_grid->AddSpacer(FromDIP(10));
    content_row->Add(xy_grid, 0, wxRIGHT, FromDIP(16));

    auto *z_col = new wxBoxSizer(wxVERTICAL);
    z_col->Add(make_btn("Z+", 92, 78, true), 0, wxBOTTOM, FromDIP(10));
    z_col->Add(make_btn("\u2302", 92, 78, true), 0, wxBOTTOM, FromDIP(10));
    z_col->Add(make_btn("Z-", 92, 78, true), 0);
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
