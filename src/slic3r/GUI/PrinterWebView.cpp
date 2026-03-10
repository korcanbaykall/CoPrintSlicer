#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

namespace {
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
    auto *left_container = new wxPanel(this, wxID_ANY);
    left_container->SetBackgroundColour(wxColour(28, 30, 34));
    auto *left_sizer = new wxBoxSizer(wxVERTICAL);
    auto *preview_box = new StaticBox(left_container, wxID_ANY);
    preview_box->SetCornerRadius(FromDIP(10));
    preview_box->SetBorderWidth(1);
    preview_box->SetBorderColorNormal(wxColour(55, 58, 64));
    preview_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    preview_box->SetBackgroundColour(wxColour(28, 30, 34));
    preview_box->SetMinSize(wxSize(-1, FromDIP(545)));
    preview_box->SetMaxSize(wxSize(-1, FromDIP(545)));
    auto *preview_row = new wxBoxSizer(wxHORIZONTAL);
    preview_row->AddSpacer(FromDIP(240));
    preview_row->Add(preview_box, 1, wxEXPAND);
    preview_row->AddSpacer(FromDIP(163));
    left_sizer->Add(preview_row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));

    auto *progress_box = new StaticBox(left_container, wxID_ANY);
    progress_box->SetCornerRadius(FromDIP(10));
    progress_box->SetBorderWidth(1);
    progress_box->SetBorderColorNormal(wxColour(55, 58, 64));
    progress_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    progress_box->SetBackgroundColour(wxColour(28, 30, 34));
    progress_box->SetMinSize(wxSize(-1, FromDIP(270)));
    progress_box->SetMaxSize(wxSize(-1, FromDIP(270)));
    auto *progress_box_sizer = new wxBoxSizer(wxVERTICAL);
    auto *progress_title = new wxStaticText(progress_box, wxID_ANY, _L("Yazdırma ilerlemesi"));
    progress_title->SetForegroundColour(wxColour(150, 156, 166));
    progress_box_sizer->Add(progress_title, 0, wxLEFT | wxTOP, FromDIP(25));
    progress_box_sizer->AddSpacer(FromDIP(15));
    auto *progress_top_line = new wxPanel(progress_box, wxID_ANY);
    progress_top_line->SetMinSize(wxSize(-1, FromDIP(1)));
    progress_top_line->SetMaxSize(wxSize(-1, FromDIP(1)));
    progress_top_line->SetBackgroundColour(wxColour(96, 100, 108));
    progress_box_sizer->Add(progress_top_line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    progress_box_sizer->AddSpacer(FromDIP(15));
    auto *progress_content_row = new wxBoxSizer(wxHORIZONTAL);
    auto *progress_thumb_box = new StaticBox(progress_box, wxID_ANY);
    progress_thumb_box->SetMinSize(wxSize(FromDIP(140), FromDIP(140)));
    progress_thumb_box->SetMaxSize(wxSize(FromDIP(140), FromDIP(140)));
    progress_thumb_box->SetCornerRadius(FromDIP(4));
    progress_thumb_box->SetBorderWidth(0);
    progress_thumb_box->SetBackgroundColorNormal(wxColour(0, 0, 0));
    progress_thumb_box->SetBackgroundColour(wxColour(0, 0, 0));
    auto *progress_thumb_sizer = new wxBoxSizer(wxVERTICAL);
    m_preview_thumbnail = new wxStaticBitmap(progress_thumb_box, wxID_ANY, wxNullBitmap);
    m_preview_thumbnail->SetBackgroundColour(wxColour(0, 0, 0));
    m_preview_thumbnail->SetMinSize(wxSize(FromDIP(120), FromDIP(120)));
    m_preview_thumbnail->SetMaxSize(wxSize(FromDIP(120), FromDIP(120)));
    set_fallback_preview_thumbnail();
    progress_thumb_sizer->AddStretchSpacer(1);
    progress_thumb_sizer->Add(m_preview_thumbnail, 0, wxALIGN_CENTER_HORIZONTAL);
    progress_thumb_sizer->AddStretchSpacer(1);
    progress_thumb_box->SetSizer(progress_thumb_sizer);
    progress_content_row->Add(progress_thumb_box, 0, wxLEFT, FromDIP(15));
    progress_content_row->AddSpacer(FromDIP(15));

    auto *controls_col = new wxBoxSizer(wxVERTICAL);
    controls_col->AddSpacer(FromDIP(35));
    
    auto *progress_controls_row = new wxBoxSizer(wxHORIZONTAL);
    auto *progress_bar = new wxGauge(progress_box, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(12)), wxGA_SMOOTH);
    progress_bar->SetValue(0);
    progress_controls_row->Add(progress_bar, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(60));
    auto *pause_icon = new wxStaticBitmap(progress_box, wxID_ANY, create_scaled_bitmap("pause", this, 20));
    progress_controls_row->Add(pause_icon, 0, wxALIGN_CENTER_VERTICAL);
    progress_controls_row->AddSpacer(FromDIP(10));
    auto *stop_icon = new wxStaticBitmap(progress_box, wxID_ANY, create_scaled_bitmap("stop", this, 20));
    progress_controls_row->Add(stop_icon, 0, wxALIGN_CENTER_VERTICAL);
    controls_col->Add(progress_controls_row, 0, wxEXPAND | wxTOP, FromDIP(10));

    auto *layer_info_row = new wxBoxSizer(wxHORIZONTAL);
    m_layer_label = new wxStaticText(progress_box, wxID_ANY, _L("Katman:"));
    m_layer_label->SetForegroundColour(wxColour(150, 156, 166));
    m_layer_printer_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_layer_printer_value->SetForegroundColour(wxColour(220, 220, 220));
    m_layer_file_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_layer_file_value->SetForegroundColour(wxColour(220, 220, 220));
    layer_info_row->Add(m_layer_label, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddSpacer(FromDIP(8));
    layer_info_row->Add(m_layer_printer_value, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddSpacer(FromDIP(30));
    layer_info_row->Add(m_layer_file_value, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddStretchSpacer(1);
    m_estimated_finish_label = new wxStaticText(progress_box, wxID_ANY, _L("Tahmini bitiş süresi:"));
    m_estimated_finish_label->SetForegroundColour(wxColour(150, 156, 166));
    m_estimated_finish_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_estimated_finish_value->SetForegroundColour(wxColour(220, 220, 220));
    layer_info_row->Add(m_estimated_finish_label, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddSpacer(FromDIP(8));
    layer_info_row->Add(m_estimated_finish_value, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddSpacer(FromDIP(70));
    controls_col->Add(layer_info_row, 0, wxTOP, FromDIP(12));

    controls_col->AddStretchSpacer(1);
    progress_content_row->Add(controls_col, 1, wxRIGHT | wxEXPAND, FromDIP(15));

    progress_box_sizer->Add(progress_content_row, 1, wxEXPAND);
    progress_box_sizer->AddStretchSpacer(1);
    progress_box->SetSizer(progress_box_sizer);
    auto *progress_row = new wxBoxSizer(wxHORIZONTAL);
    progress_row->AddSpacer(FromDIP(240));
    progress_row->Add(progress_box, 1, wxEXPAND);
    progress_row->AddSpacer(FromDIP(163));
    left_sizer->Add(progress_row, 0, wxEXPAND | wxTOP, FromDIP(5));
    left_sizer->AddSpacer(FromDIP(2));

    left_container->SetSizer(left_sizer);

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
    const int side_h = FromDIP(210);
    const int left_side_inset_x = FromDIP(0);
    const int top_center_x = center_pos + (center_size - top_w) / 2;
    const int side_shift_y = FromDIP(8);
    const int side_left_x_raw = center_pos - gap - side_w + left_side_inset_x;
    const int side_left_x = side_left_x_raw < 0 ? 0 : side_left_x_raw;
    const int side_right_x_raw = center_pos + center_size + gap;
    const int side_right_x = (side_right_x_raw + side_w > xy_square) ? (xy_square - side_w) : side_right_x_raw;

    auto *xy_area = new wxPanel(right_container, wxID_ANY, wxDefaultPosition, wxSize(xy_square, xy_square));
    xy_area->SetMinSize(wxSize(xy_square, xy_square));
    xy_area->SetMaxSize(wxSize(xy_square, xy_square));
    xy_area->SetBackgroundColour(wxColour(28, 30, 34));

    auto add_axis_icon = [this](wxWindow *parent, const std::string &icon_key, int x, int y, int box_w, int box_h, const wxColour &holder_bg = wxColour(28, 30, 34), bool snug_to_bitmap = false) {
        if (icon_key.empty())
            return;
        const int pad = this->FromDIP(0);
        int holder_w = box_w - pad * 2;
        int holder_h = box_h - pad * 2;
        if (holder_w < this->FromDIP(1)) holder_w = this->FromDIP(1);
        if (holder_h < this->FromDIP(1)) holder_h = this->FromDIP(1);

        const int icon_target = holder_h;
        const int icon_px = this->ToDIP(wxSize(0, icon_target)).GetHeight();
        auto bmp = create_scaled_bitmap(icon_key, this, icon_px > 0 ? icon_px : 1);

        wxPoint holder_pos(x + pad, y + pad);
        if (snug_to_bitmap && bmp.IsOk()) {
            const wxSize bmp_sz = bmp.GetScaledSize();
            holder_w = std::max(this->FromDIP(1), std::min(holder_w, bmp_sz.GetWidth()));
            holder_h = std::max(this->FromDIP(1), std::min(holder_h, bmp_sz.GetHeight()));
            holder_pos.x = x + (box_w - holder_w) / 2;
            holder_pos.y = y + (box_h - holder_h) / 2;
        }

        auto *holder = new wxPanel(parent, wxID_ANY, holder_pos, wxSize(holder_w, holder_h));
        holder->SetBackgroundColour(holder_bg);

        auto *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->AddStretchSpacer(1);
        auto *icon = new wxStaticBitmap(holder, wxID_ANY, bmp);
        sizer->Add(icon, 0, wxALIGN_CENTER);
        sizer->AddStretchSpacer(1);
        holder->SetSizer(sizer);
    };

    add_axis_icon(xy_area, resolve_icon("vector10", ""), side_left_x, (xy_square - side_h) / 2 + side_shift_y, side_w, side_h, wxColour(28, 30, 34), true);
    add_axis_icon(xy_area, resolve_icon("vector12", ""), side_right_x, (xy_square - side_h) / 2 + side_shift_y, side_w, side_h, wxColour(28, 30, 34), true);
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

    main_sizer->Add(left_container, 1, wxEXPAND);
    main_sizer->Add(right_container, 0, wxEXPAND | wxALL, FromDIP(10));
    SetSizer(main_sizer);

    m_browser = nullptr;
    m_zoomFactor = 100;
    Bind(wxEVT_WEBREQUEST_STATE, &PrinterWebView::on_thumbnail_webrequest_state, this);
    m_layer_refresh_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_layer_info_from_selected_machine(); }, m_layer_refresh_timer->GetId());
    m_layer_refresh_timer->Start(1000);
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);
 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
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

void PrinterWebView::set_fallback_preview_thumbnail()
{
    if (m_preview_thumbnail == nullptr)
        return;

    m_preview_thumbnail_url.clear();
    const wxString logo_path = from_u8(Slic3r::resources_dir() + "/images/logo.jpg");
    wxImage logo_image;
    if (logo_image.LoadFile(logo_path, wxBITMAP_TYPE_JPEG)) {
        const int max_width = FromDIP(110);
        const int max_height = FromDIP(60);
        const double width_ratio = static_cast<double>(max_width) / static_cast<double>(logo_image.GetWidth());
        const double height_ratio = static_cast<double>(max_height) / static_cast<double>(logo_image.GetHeight());
        const double scale_ratio = std::min(width_ratio, height_ratio);
        const int scaled_width = std::max(1, static_cast<int>(logo_image.GetWidth() * scale_ratio));
        const int scaled_height = std::max(1, static_cast<int>(logo_image.GetHeight() * scale_ratio));
        wxImage resized = logo_image.Scale(scaled_width, scaled_height, wxIMAGE_QUALITY_HIGH);
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
            wxImage resized = m_thumbnail_image.Scale(FromDIP(120), FromDIP(120), wxIMAGE_QUALITY_HIGH);
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
    if (obj == nullptr || obj->slice_info == nullptr || obj->slice_info->thumbnail_url.empty()) {
        if (m_thumbnail_web_request.IsOk())
            m_thumbnail_web_request.Cancel();
        set_fallback_preview_thumbnail();
        return;
    }

    const wxString next_url = wxString(obj->slice_info->thumbnail_url);
    if (next_url == m_preview_thumbnail_url)
        return;

    if (m_thumbnail_web_request.IsOk())
        m_thumbnail_web_request.Cancel();

    m_preview_thumbnail_url = next_url;
    m_thumbnail_web_request = wxWebSession::GetDefault().CreateRequest(this, m_preview_thumbnail_url);
    if (!m_thumbnail_web_request.IsOk()) {
        set_fallback_preview_thumbnail();
        return;
    }

    m_thumbnail_web_request.Start();
}

void PrinterWebView::refresh_layer_info_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;

    set_active_file_name(active_file_name_text(obj));
    update_preview_thumbnail(obj);

    const int printer_layer = (obj != nullptr && obj->curr_layer > 0) ? obj->curr_layer : -1;
    const int file_layer = (obj != nullptr && obj->total_layers > 0) ? obj->total_layers : -1;
    set_layer_info(printer_layer, file_layer);

    int remaining_seconds = -1;
    if (obj != nullptr) {
        const int total_duration_seconds = (obj->slice_info != nullptr && obj->slice_info->prediction > 0) ? obj->slice_info->prediction : -1;
        if (total_duration_seconds > 0 && obj->mc_print_percent >= 0 && obj->mc_print_percent <= 100) {
            const int elapsed_seconds = static_cast<int>((static_cast<long long>(total_duration_seconds) * obj->mc_print_percent) / 100);
            remaining_seconds = total_duration_seconds - elapsed_seconds;
        } else if (obj->mc_left_time > 0) {
            remaining_seconds = obj->mc_left_time;
        }
    }
    set_estimated_remaining_seconds(remaining_seconds);
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
