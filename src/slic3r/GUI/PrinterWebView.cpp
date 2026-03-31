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

#include <array>

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
    auto *preview_menu_panel = new wxPanel(this, wxID_ANY);
    preview_menu_panel->SetBackgroundColour(wxColour(28, 30, 34));
    preview_menu_panel->SetMinSize(wxSize(FromDIP(220), FromDIP(545)));
    preview_menu_panel->SetMaxSize(wxSize(FromDIP(220), FromDIP(545)));
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

    preview_menu_sizer->AddSpacer(FromDIP(35));
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
    preview_box->SetCornerRadius(FromDIP(10));
    preview_box->SetBorderWidth(1);
    preview_box->SetBorderColorNormal(wxColour(55, 58, 64));
    preview_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    preview_box->SetBackgroundColour(wxColour(28, 30, 34));
    preview_box->SetMinSize(wxSize(FromDIP(910), FromDIP(545)));
    preview_box->SetMaxSize(wxSize(FromDIP(910), FromDIP(545)));
    auto *preview_box_sizer = new wxBoxSizer(wxVERTICAL);
    preview_box_sizer->AddSpacer(FromDIP(15));
    auto *camera_label = new wxStaticText(preview_box, wxID_ANY, _L("Kamera"));
    camera_label->SetForegroundColour(wxColour(150, 156, 166));
    preview_box_sizer->Add(camera_label, 0, wxLEFT, FromDIP(20));
    auto *preview_top_divider = new wxPanel(preview_box, wxID_ANY);
    preview_top_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    preview_top_divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    preview_top_divider->SetBackgroundColour(wxColour(96, 100, 108));
    preview_box_sizer->Add(preview_top_divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    auto *preview_middle_frame = new wxBoxSizer(wxHORIZONTAL);
    auto *preview_left_frame = new wxPanel(preview_box, wxID_ANY);
    preview_left_frame->SetMinSize(wxSize(FromDIP(1), -1));
    preview_left_frame->SetMaxSize(wxSize(FromDIP(1), -1));
    preview_left_frame->SetBackgroundColour(wxColour(96, 100, 108));
    auto *preview_right_frame = new wxPanel(preview_box, wxID_ANY);
    preview_right_frame->SetMinSize(wxSize(FromDIP(1), -1));
    preview_right_frame->SetMaxSize(wxSize(FromDIP(1), -1));
    preview_right_frame->SetBackgroundColour(wxColour(96, 100, 108));
    preview_middle_frame->Add(preview_left_frame, 0, wxEXPAND);
    preview_middle_frame->AddStretchSpacer(1);
    preview_middle_frame->Add(preview_right_frame, 0, wxEXPAND);
    preview_box_sizer->Add(preview_middle_frame, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    auto *preview_bottom_divider = new wxPanel(preview_box, wxID_ANY);
    preview_bottom_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    preview_bottom_divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    preview_bottom_divider->SetBackgroundColour(wxColour(96, 100, 108));
    preview_box_sizer->Add(preview_bottom_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    auto *preview_bottom_panel = new wxPanel(preview_box, wxID_ANY);
    preview_bottom_panel->SetBackgroundColour(wxColour(28, 30, 34));
    preview_bottom_panel->SetMinSize(wxSize(-1, FromDIP(70)));
    preview_bottom_panel->SetMaxSize(wxSize(-1, FromDIP(70)));
    auto *preview_bottom_sizer = new wxBoxSizer(wxVERTICAL);
    preview_bottom_sizer->AddStretchSpacer(1);
    auto *preview_bottom_row = new wxBoxSizer(wxHORIZONTAL);
    preview_bottom_row->AddSpacer(FromDIP(5));
    auto *play_icon = new wxStaticBitmap(preview_bottom_panel, wxID_ANY, create_scaled_bitmap("play", this, 30));
    play_icon->SetMinSize(wxSize(FromDIP(30), FromDIP(30)));
    play_icon->SetMaxSize(wxSize(FromDIP(30), FromDIP(30)));
    preview_bottom_row->Add(play_icon, 0, wxALIGN_CENTER_VERTICAL);
    preview_bottom_sizer->Add(preview_bottom_row, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
    preview_bottom_panel->SetSizer(preview_bottom_sizer);
    preview_box_sizer->Add(preview_bottom_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));
    preview_box->SetSizer(preview_box_sizer);
    top_row->Add(preview_box, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));

    auto *progress_box = new StaticBox(left_container, wxID_ANY);
    progress_box->SetCornerRadius(FromDIP(10));
    progress_box->SetBorderWidth(1);
    progress_box->SetBorderColorNormal(wxColour(55, 58, 64));
    progress_box->SetBackgroundColorNormal(wxColour(22, 24, 29));
    progress_box->SetBackgroundColour(wxColour(28, 30, 34));
    progress_box->SetMinSize(wxSize(FromDIP(910), FromDIP(270)));
    progress_box->SetMaxSize(wxSize(FromDIP(910), FromDIP(270)));
    auto *progress_box_sizer = new wxBoxSizer(wxVERTICAL);
    auto *progress_title = new wxStaticText(progress_box, wxID_ANY, wxString::FromUTF8("Yazd\xC4\xB1rma ilerlemesi"));
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
    controls_col->AddSpacer(FromDIP(55));
    m_active_file_name_value = new wxStaticText(progress_box, wxID_ANY, "N/A", wxDefaultPosition, wxSize(FromDIP(420), -1), wxST_ELLIPSIZE_END);
    m_active_file_name_value->SetForegroundColour(wxColour(97, 211, 124));
    controls_col->Add(m_active_file_name_value, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    
    auto *progress_controls_row = new wxBoxSizer(wxHORIZONTAL);
    auto *progress_bar = new wxGauge(progress_box, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(520), FromDIP(12)), wxGA_SMOOTH);
    progress_bar->SetValue(0);
    progress_controls_row->Add(progress_bar, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(45));
    auto *pause_icon = new wxStaticBitmap(progress_box, wxID_ANY, create_scaled_bitmap("pause", this, 20));
    progress_controls_row->Add(pause_icon, 0, wxALIGN_CENTER_VERTICAL);
    progress_controls_row->AddSpacer(FromDIP(10));
    auto *stop_icon = new wxStaticBitmap(progress_box, wxID_ANY, create_scaled_bitmap("stop", this, 20));
    progress_controls_row->Add(stop_icon, 0, wxALIGN_CENTER_VERTICAL);
    progress_controls_row->AddSpacer(FromDIP(35));
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
    layer_info_row->AddSpacer(FromDIP(315));
    m_estimated_finish_label = new wxStaticText(progress_box, wxID_ANY, wxString::FromUTF8("Tahmini biti\xC5\x9F s\xC3\xBCresi:"));
    m_estimated_finish_label->SetForegroundColour(wxColour(150, 156, 166));
    m_estimated_finish_value = new wxStaticText(progress_box, wxID_ANY, "N/A");
    m_estimated_finish_value->SetForegroundColour(wxColour(220, 220, 220));
    layer_info_row->Add(m_estimated_finish_label, 0, wxALIGN_CENTER_VERTICAL);
    layer_info_row->AddSpacer(FromDIP(8));
    layer_info_row->Add(m_estimated_finish_value, 0, wxALIGN_CENTER_VERTICAL);
    controls_col->Add(layer_info_row, 0, wxTOP, FromDIP(4));

    controls_col->AddStretchSpacer(1);
    progress_content_row->Add(controls_col, 0, wxRIGHT, FromDIP(15));

    progress_box_sizer->Add(progress_content_row, 1, wxEXPAND);
    progress_box_sizer->AddStretchSpacer(1);
    progress_box->SetSizer(progress_box_sizer);

    auto make_upper_placeholder_box = [this, left_container]() {
        auto *box = new StaticBox(left_container, wxID_ANY);
        box->SetMinSize(wxSize(FromDIP(550), FromDIP(283)));
        box->SetMaxSize(wxSize(FromDIP(550), FromDIP(283)));
        box->SetCornerRadius(FromDIP(12));
        box->SetBorderWidth(1);
        box->SetBorderColorNormal(wxColour(55, 58, 64));
        box->SetBackgroundColorNormal(wxColour(22, 24, 29));
        box->SetBackgroundColour(wxColour(28, 30, 34));
        return box;
    };

    auto make_lower_placeholder_box = [this, left_container]() {
        auto *box = new StaticBox(left_container, wxID_ANY);
        box->SetMinSize(wxSize(FromDIP(550), FromDIP(225)));
        box->SetMaxSize(wxSize(FromDIP(550), FromDIP(225)));
        box->SetCornerRadius(FromDIP(10));
        box->SetBorderWidth(1);
        box->SetBorderColorNormal(wxColour(55, 58, 64));
        box->SetBackgroundColorNormal(wxColour(22, 24, 29));
        box->SetBackgroundColour(wxColour(28, 30, 34));
        return box;
    };

    auto *upper_placeholder_box = make_upper_placeholder_box();
    auto *lower_placeholder_box = make_lower_placeholder_box();

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
    upper_placeholder_box->SetSizer(upper_placeholder_sizer);

    auto *lower_placeholder_sizer = new wxBoxSizer(wxVERTICAL);
    auto *lower_placeholder_header = new wxBoxSizer(wxHORIZONTAL);
    auto *printer_title = new wxStaticText(lower_placeholder_box, wxID_ANY, wxString::FromUTF8("Yaz\xC4\xB1""c\xC4\xB1"));
    printer_title->SetForegroundColour(wxColour(220, 220, 220));
    lower_placeholder_header->Add(printer_title, 0, wxALIGN_CENTER_VERTICAL);
    lower_placeholder_header->AddSpacer(FromDIP(30));

    auto *printer_button = new Button(lower_placeholder_box, wxString::FromUTF8("Yazdirma Se\xC3\xA7""enekleri"));
    printer_button->SetMinSize(wxSize(FromDIP(180), FromDIP(40)));
    printer_button->SetMaxSize(wxSize(FromDIP(180), FromDIP(40)));
    printer_button->SetCornerRadius(FromDIP(12));
    printer_button->SetBorderWidth(1);
    printer_button->SetBorderColorNormal(wxColour(78, 129, 255));
    printer_button->SetBackgroundColorNormal(wxColour(28, 30, 34));
    printer_button->SetTextColorNormal(wxColour(120, 170, 255));
    lower_placeholder_header->Add(printer_button, 0, wxALIGN_CENTER_VERTICAL);
    lower_placeholder_header->AddSpacer(FromDIP(20));

    auto *printer_menu_text = new wxStaticText(lower_placeholder_box, wxID_ANY, "...");
    printer_menu_text->SetForegroundColour(wxColour(180, 180, 180));
    lower_placeholder_header->Add(printer_menu_text, 0, wxALIGN_CENTER_VERTICAL);

    lower_placeholder_sizer->Add(lower_placeholder_header, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
    lower_placeholder_sizer->AddSpacer(FromDIP(10));
    auto *lower_placeholder_divider = new wxPanel(lower_placeholder_box, wxID_ANY);
    lower_placeholder_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    lower_placeholder_divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    lower_placeholder_divider->SetBackgroundColour(wxColour(55, 58, 64));
    lower_placeholder_sizer->Add(lower_placeholder_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    lower_placeholder_sizer->AddSpacer(FromDIP(20));
    auto *printer_info_row = new wxBoxSizer(wxHORIZONTAL);

    auto *printer_photo_box = new StaticBox(lower_placeholder_box, wxID_ANY);
    printer_photo_box->SetMinSize(wxSize(FromDIP(100), FromDIP(110)));
    printer_photo_box->SetMaxSize(wxSize(FromDIP(100), FromDIP(110)));
    printer_photo_box->SetCornerRadius(FromDIP(6));
    printer_photo_box->SetBorderWidth(1);
    printer_photo_box->SetBorderColorNormal(wxColour(55, 58, 64));
    printer_photo_box->SetBackgroundColorNormal(wxColour(20, 22, 26));
    printer_photo_box->SetBackgroundColour(wxColour(20, 22, 26));
    printer_info_row->Add(printer_photo_box, 0, wxALIGN_TOP);
    printer_info_row->AddSpacer(FromDIP(20));

    auto *printer_text_col = new wxBoxSizer(wxVERTICAL);

    auto *printer_name = new wxStaticText(lower_placeholder_box, wxID_ANY, "Anycubic Kobra 3");
    printer_name->SetForegroundColour(wxColour(235, 235, 235));
    wxFont printer_name_font = printer_name->GetFont();
    printer_name_font.SetWeight(wxFONTWEIGHT_BOLD);
    printer_name_font.SetPointSize(printer_name_font.GetPointSize() + 1);
    printer_name->SetFont(printer_name_font);
    printer_text_col->Add(printer_name, 0, wxALIGN_TOP);
    printer_text_col->AddSpacer(FromDIP(10));

    auto *printer_model_row = new wxBoxSizer(wxHORIZONTAL);
    auto *printer_model_label = new wxStaticText(lower_placeholder_box, wxID_ANY, wxString::FromUTF8("Model \xC4\xB0smi:"));
    printer_model_label->SetForegroundColour(wxColour(180, 180, 180));
    printer_model_row->Add(printer_model_label, 0, wxALIGN_TOP);
    printer_model_row->AddSpacer(FromDIP(10));

    auto *printer_model_value = new wxStaticText(
        lower_placeholder_box,
        wxID_ANY,
        "Anycubic Kobra 3 0.4mm nozzle",
        wxDefaultPosition,
        wxSize(FromDIP(300), -1),
        wxST_ELLIPSIZE_END);
    printer_model_value->SetForegroundColour(wxColour(220, 220, 220));
    printer_model_row->Add(printer_model_value, 0, wxALIGN_TOP);

    printer_text_col->Add(printer_model_row, 0, wxALIGN_TOP);
    printer_text_col->AddSpacer(FromDIP(6));

    auto *printer_serial_row = new wxBoxSizer(wxHORIZONTAL);
    auto *printer_serial_label = new wxStaticText(lower_placeholder_box, wxID_ANY, wxString::FromUTF8("Seri No:"));
    printer_serial_label->SetForegroundColour(wxColour(180, 180, 180));
    printer_serial_row->Add(printer_serial_label, 0, wxALIGN_TOP);
    printer_serial_row->AddSpacer(FromDIP(10));

    auto *printer_serial_value = new wxStaticText(
        lower_placeholder_box,
        wxID_ANY,
        "0937-A27E-89E5-",
        wxDefaultPosition,
        wxSize(FromDIP(300), -1),
        wxST_ELLIPSIZE_END);
    printer_serial_value->SetForegroundColour(wxColour(220, 220, 220));
    printer_serial_row->Add(printer_serial_value, 0, wxALIGN_TOP);

    printer_text_col->Add(printer_serial_row, 0, wxALIGN_TOP);
    printer_text_col->AddSpacer(FromDIP(6));

    auto *printer_firmware_row = new wxBoxSizer(wxHORIZONTAL);
    auto *printer_firmware_label = new wxStaticText(lower_placeholder_box, wxID_ANY, wxString::FromUTF8("Yaz\xC4\xB1l\xC4\xB1m S\xC3\xBCr\xC3\xBCm\xC3\xBC:"));
    printer_firmware_label->SetForegroundColour(wxColour(180, 180, 180));
    printer_firmware_row->Add(printer_firmware_label, 0, wxALIGN_TOP);
    printer_firmware_row->AddSpacer(FromDIP(10));

    auto *printer_firmware_value = new wxStaticText(
        lower_placeholder_box,
        wxID_ANY,
        "2.4.5",
        wxDefaultPosition,
        wxSize(FromDIP(160), -1),
        wxST_ELLIPSIZE_END);
    printer_firmware_value->SetForegroundColour(wxColour(220, 220, 220));
    printer_firmware_row->Add(printer_firmware_value, 0, wxALIGN_TOP);

    printer_text_col->Add(printer_firmware_row, 0, wxALIGN_TOP);
    printer_info_row->Add(printer_text_col, 0, wxALIGN_TOP);

    lower_placeholder_sizer->Add(printer_info_row, 0, wxLEFT | wxBOTTOM, FromDIP(25));

    lower_placeholder_sizer->AddStretchSpacer(1);
    lower_placeholder_box->SetSizer(lower_placeholder_sizer);

    auto *progress_row = new wxBoxSizer(wxHORIZONTAL);
    auto *progress_box_col = new wxBoxSizer(wxVERTICAL);
    progress_box_col->AddSpacer(FromDIP(15));
    progress_box_col->Add(progress_box, 0, wxEXPAND);
    progress_row->Add(progress_box_col, 0, wxEXPAND);
    progress_row->AddSpacer(FromDIP(20));

    auto *lower_placeholder_col = new wxBoxSizer(wxVERTICAL);
    lower_placeholder_col->AddSpacer(FromDIP(40));
    lower_placeholder_col->Add(lower_placeholder_box, 0, wxEXPAND);
    progress_row->Add(lower_placeholder_col, 0, wxRIGHT | wxALIGN_TOP, FromDIP(20));

    auto *right_container = new wxPanel(left_container, wxID_ANY);
    right_container->SetBackgroundColour(wxColour(28, 30, 34));
    const int right_container_width = FromDIP(760);
    const int right_container_height = FromDIP(360);
    right_container->SetSize(wxSize(right_container_width, right_container_height));
    right_container->SetMinSize(wxSize(right_container_width, -1));
    right_container->SetMaxSize(wxSize(right_container_width, right_container_height));
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
    right_sizer->Add(step_bg, 0, wxTOP | wxLEFT, FromDIP(24));

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
    auto *bottom_split_host = new wxPanel(right_container, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(89), FromDIP(75)));
    bottom_split_host->SetMinSize(wxSize(FromDIP(89), FromDIP(75)));
    bottom_split_host->SetMaxSize(wxSize(FromDIP(89), FromDIP(75)));
    bottom_split_host->SetBackgroundColour(wxColour(28, 30, 34));
    auto *bottom_split_bitmap = new wxStaticBitmap(bottom_split_host, wxID_ANY, create_scaled_bitmap("rectangle_12", this, 75));
    const wxSize bottom_split_bitmap_size = bottom_split_bitmap->GetBestSize();
    bottom_split_bitmap->SetPosition(wxPoint(
        (bottom_split_host->GetMinSize().GetWidth() - bottom_split_bitmap_size.GetWidth()) / 2,
        (bottom_split_host->GetMinSize().GetHeight() - bottom_split_bitmap_size.GetHeight()) / 2));
    auto *bottom_divider = new wxPanel(bottom_split_host, wxID_ANY, wxPoint(FromDIP(44), 0), wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetMinSize(wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetMaxSize(wxSize(FromDIP(1), FromDIP(75)));
    bottom_divider->SetBackgroundColour(wxColour(70, 74, 82));
    z_col->Add(bottom_split_host, 0, wxLEFT, FromDIP(10));
    content_row->Add(z_col, 0, wxALIGN_CENTER_VERTICAL);

    right_sizer->Add(content_row, 0, wxALL, FromDIP(12));

    auto *right_placeholder_box = new StaticBox(right_container, wxID_ANY);
    const int right_placeholder_width = FromDIP(190);
    const int right_placeholder_height = FromDIP(320);
    const int right_placeholder_x = FromDIP(529);
    right_placeholder_box->SetSize(wxRect(wxPoint(right_placeholder_x, FromDIP(32)), wxSize(right_placeholder_width, right_placeholder_height)));
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
    add_placeholder_line(240);

    auto *bottom_row_center_divider = new wxPanel(right_placeholder_box, wxID_ANY);
    bottom_row_center_divider->SetSize(wxRect(
        wxPoint(right_placeholder_width / 2, FromDIP(240)),
        wxSize(FromDIP(1), right_placeholder_height - FromDIP(240))));
    bottom_row_center_divider->SetMinSize(wxSize(FromDIP(1), right_placeholder_height - FromDIP(240)));
    bottom_row_center_divider->SetMaxSize(wxSize(FromDIP(1), right_placeholder_height - FromDIP(240)));
    bottom_row_center_divider->SetBackgroundColour(wxColour(55, 58, 64));

    auto *bed_label = new wxStaticText(right_placeholder_box, wxID_ANY, "Bed");
    bed_label->SetPosition(wxPoint(FromDIP(12), FromDIP(20)));
    bed_label->SetForegroundColour(wxColour(220, 220, 220));

    auto *bed_value = new wxStaticText(right_placeholder_box, wxID_ANY, "__ / __");
    bed_value->SetPosition(wxPoint(FromDIP(107), FromDIP(20)));
    bed_value->SetForegroundColour(wxColour(220, 220, 220));

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

    auto *extruder_unit = new wxStaticText(right_placeholder_box, wxID_ANY, wxString::FromUTF8("\xC2\xB0""C"));
    extruder_unit->SetForegroundColour(wxColour(220, 220, 220));
    extruder_unit->SetCursor(wxCursor(wxCURSOR_HAND));

    m_extruder_popup_button = extruder_label;
    auto extruder_popup_handler = [this](wxMouseEvent &) { toggle_extruder_popup(); };
    extruder_label->Bind(wxEVT_LEFT_DOWN, extruder_popup_handler);
    extruder_value->Bind(wxEVT_LEFT_DOWN, extruder_popup_handler);
    extruder_unit->Bind(wxEVT_LEFT_DOWN, extruder_popup_handler);

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

    const int right_value_margin = FromDIP(5);
    const int inter_value_gap = FromDIP(5);
    const int value_right_edge = right_placeholder_width - right_value_margin;

    const int bed_unit_x = value_right_edge - bed_unit->GetBestSize().GetWidth();
    bed_unit->SetPosition(wxPoint(bed_unit_x, FromDIP(20)));
    const int bed_value_x = bed_unit_x - inter_value_gap - bed_value->GetBestSize().GetWidth();
    bed_value->SetPosition(wxPoint(bed_value_x, FromDIP(20)));

    const int extruder_unit_x = value_right_edge - extruder_unit->GetBestSize().GetWidth();
    extruder_unit->SetPosition(wxPoint(extruder_unit_x, FromDIP(80)));
    const int extruder_value_x = extruder_unit_x - inter_value_gap - extruder_value->GetBestSize().GetWidth();
    extruder_value->SetPosition(wxPoint(extruder_value_x, FromDIP(80)));

    const int fan_row_y = FromDIP(140);
    const int fan_unit_x = value_right_edge - fan_unit->GetBestSize().GetWidth();
    const int fan_value_width = fan_value->GetBestSize().GetWidth();
    const int fan_value_x = fan_unit_x - inter_value_gap - fan_value_width;
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
    const int speed_value_x = std::min(fan_value_x, value_right_edge - speed_value->GetBestSize().GetWidth());
    speed_value->SetPosition(wxPoint(speed_value_x, speed_row_y));
    m_speed_display_label = speed_value;
    m_speed_popup_button = speed_value;

    auto speed_popup_handler = [this](wxMouseEvent &) { toggle_speed_popup(); };
    speed_label->SetCursor(wxCursor(wxCURSOR_HAND));
    speed_label->Bind(wxEVT_LEFT_DOWN, speed_popup_handler);
    speed_value->Bind(wxEVT_LEFT_DOWN, speed_popup_handler);

    auto *right_placeholder_right_border = new wxPanel(right_container, wxID_ANY);
    right_placeholder_right_border->SetSize(wxRect(
        wxPoint(right_placeholder_x + right_placeholder_width - FromDIP(1), FromDIP(44)),
        wxSize(FromDIP(1), right_placeholder_height - FromDIP(24))));
    right_placeholder_right_border->SetMinSize(wxSize(FromDIP(1), right_placeholder_height - FromDIP(24)));
    right_placeholder_right_border->SetMaxSize(wxSize(FromDIP(1), right_placeholder_height - FromDIP(24)));
    right_placeholder_right_border->SetBackgroundColour(wxColour(70, 74, 82));

    auto *bottom_left_placeholder_icon = new wxStaticBitmap(
        right_placeholder_box,
        wxID_ANY,
        create_scaled_bitmap("idea", this, 40));
    const wxSize bottom_left_icon_size = bottom_left_placeholder_icon->GetBestSize();
    const int bottom_cell_width = right_placeholder_width / 2;
    const int bottom_cell_height = right_placeholder_height - FromDIP(240);
    const int bottom_left_icon_x = (bottom_cell_width - bottom_left_icon_size.GetWidth()) / 2;
    const int bottom_left_icon_y = FromDIP(240) + (bottom_cell_height - bottom_left_icon_size.GetHeight()) / 2;
    bottom_left_placeholder_icon->SetPosition(wxPoint(bottom_left_icon_x, bottom_left_icon_y));

    auto *clear_all_button = new wxButton(
        right_placeholder_box,
        wxID_ANY,
        "Clear All",
        wxDefaultPosition,
        wxSize(FromDIP(84), FromDIP(32)));
    clear_all_button->SetMinSize(wxSize(FromDIP(84), FromDIP(32)));
    clear_all_button->SetMaxSize(wxSize(FromDIP(84), FromDIP(32)));
    clear_all_button->SetBackgroundColour(wxColour(28, 30, 34));
    clear_all_button->SetForegroundColour(wxColour(220, 220, 220));
    clear_all_button->SetWindowStyleFlag(wxBORDER_SIMPLE);
    const int bottom_right_cell_x = bottom_cell_width;
    const int clear_all_x = bottom_right_cell_x + (bottom_cell_width - clear_all_button->GetMinSize().GetWidth()) / 2;
    const int clear_all_y = FromDIP(240) + (bottom_cell_height - clear_all_button->GetMinSize().GetHeight()) / 2;
    clear_all_button->SetPosition(wxPoint(clear_all_x, clear_all_y));
    clear_all_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { reset_placeholder_selections(); });

    right_container->SetSizer(right_sizer);
    right_container->Layout();

    auto *side_column = new wxBoxSizer(wxVERTICAL);
    side_column->Add(right_container, 0, wxEXPAND);
    side_column->AddSpacer(FromDIP(20));
    side_column->Add(upper_placeholder_box, 0, wxEXPAND);

    top_row->AddSpacer(FromDIP(20));
    top_row->Add(side_column, 0, wxTOP | wxBOTTOM | wxRIGHT, FromDIP(5));
    left_sizer->Add(top_row, 0, wxEXPAND);
    left_sizer->Add(progress_row, 0, wxEXPAND);
    left_sizer->AddSpacer(FromDIP(52));

    left_container->SetSizer(left_sizer);

    status_page_sizer->Add(left_container, 1, wxEXPAND | wxRIGHT, FromDIP(20));
    m_status_page->SetSizer(status_page_sizer);

    m_storage_page = create_placeholder_page(content_host, "Depolama", "Bu alan simdilik hazirlaniyor.");
    m_update_page = create_placeholder_page(content_host, "Guncelle", "Guncelleme icerigi daha sonra baglanacak.");
    m_assistant_page = create_placeholder_page(content_host, "Asistan", "Asistan paneli icin gecici yer tutucu.");

    content_host_sizer->Add(m_status_page, 1, wxEXPAND);
    content_host_sizer->Add(m_storage_page, 1, wxEXPAND);
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
}

void PrinterWebView::refresh_fan_value_display()
{
    if (m_selected_fan.StartsWith("T")) {
        const auto it = m_fan_values.find(m_selected_fan);
        m_selected_fan_value = it != m_fan_values.end() ? it->second : "__";
    }

    if (m_fan_value_label != nullptr) {
        m_fan_value_label->SetLabelText(m_selected_fan_value);
        const int right_placeholder_width = FromDIP(190);
        const int right_value_margin = FromDIP(5);
        const int inter_value_gap = FromDIP(5);
        const int value_right_edge = right_placeholder_width - right_value_margin;
        wxClientDC dc(m_fan_value_label);
        dc.SetFont(m_fan_value_label->GetFont());
        wxCoord fan_unit_width = 0;
        wxCoord fan_unit_height = 0;
        dc.GetTextExtent("%", &fan_unit_width, &fan_unit_height);
        const int fan_unit_x = value_right_edge - fan_unit_width;
        const int fan_value_x = fan_unit_x - inter_value_gap - m_fan_value_label->GetBestSize().GetWidth();
        m_fan_value_label->SetPosition(wxPoint(fan_value_x, FromDIP(140)));
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
    if (m_speed_display_label != nullptr) {
        m_speed_display_label->SetLabelText(m_selected_speed);
        const int right_placeholder_width = FromDIP(190);
        const int right_value_margin = FromDIP(5);
        const int inter_value_gap = FromDIP(5);
        const int value_right_edge = right_placeholder_width - right_value_margin;
        wxClientDC dc(m_speed_display_label);
        dc.SetFont(m_speed_display_label->GetFont());
        wxCoord fan_unit_width = 0;
        wxCoord fan_unit_height = 0;
        wxCoord fan_value_width = 0;
        wxCoord fan_value_height = 0;
        dc.GetTextExtent("%", &fan_unit_width, &fan_unit_height);
        dc.GetTextExtent("__", &fan_value_width, &fan_value_height);
        const int fan_unit_x = value_right_edge - fan_unit_width;
        const int aligned_speed_x = fan_unit_x - inter_value_gap - fan_value_width;
        const int speed_value_x = std::min(aligned_speed_x, value_right_edge - m_speed_display_label->GetBestSize().GetWidth());
        m_speed_display_label->SetPosition(wxPoint(speed_value_x, FromDIP(200)));
    }

    dismiss_extruder_popup();
    dismiss_fan_popup();
    dismiss_speed_popup();
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
    printers_popup_sizer->Add(search_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto add_popup_line = [this, printers_popup_sizer](wxWindow *parent, const wxString &text, const wxColour &color, bool bold = false, int top = 16) {
        auto *line = new wxStaticText(parent, wxID_ANY, text);
        line->SetForegroundColour(color);
        if (bold) {
            wxFont font = line->GetFont();
            font.SetWeight(wxFONTWEIGHT_BOLD);
            line->SetFont(font);
        }
        printers_popup_sizer->Add(line, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(top));
    };

    std::map<std::string, MachineObject*> visible_my_machines;
    for (const auto &entry : my_machines) {
        if (entry.second == nullptr)
            continue;
        if (selected_machine != nullptr && entry.first == selected_machine->get_dev_id())
            continue;
        visible_my_machines.emplace(entry);
    }

    std::map<std::string, MachineObject*> other_local_machines;
    for (const auto &entry : local_machines) {
        if (entry.second == nullptr)
            continue;
        if (visible_my_machines.find(entry.first) != visible_my_machines.end())
            continue;
        other_local_machines.emplace(entry);
    }

    if (!has_any_machine) {
        add_popup_line(m_printers_popup_panel, "Cihaz ekle +", wxColour(20, 20, 20), true, 18);
        add_popup_line(m_printers_popup_panel, "+ IP Adresi ile Baglan", wxColour(20, 20, 20), true, 14);
    } else {
        if (!visible_my_machines.empty()) {
            add_popup_line(m_printers_popup_panel, "Cihazim", wxColour(120, 120, 120), false, 18);
            for (const auto &entry : visible_my_machines) {
                auto *machine = entry.second;
                if (machine == nullptr)
                    continue;
                add_popup_line(m_printers_popup_panel, from_u8(machine->get_dev_name()), wxColour(20, 20, 20), false, 12);
            }
        }

        if (!other_local_machines.empty()) {
            add_popup_line(m_printers_popup_panel, "Diger Cihazlar", wxColour(120, 120, 120), false, 18);
            for (const auto &entry : other_local_machines) {
                auto *machine = entry.second;
                if (machine == nullptr)
                    continue;
                add_popup_line(m_printers_popup_panel, from_u8(machine->get_dev_name()), wxColour(80, 80, 80), false, 12);
            }
        }

        add_popup_line(m_printers_popup_panel, "+ IP Adresi ile Baglan", wxColour(20, 20, 20), true, 16);
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
    for (int i = 0; i < 4; ++i) {
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

    const std::array<wxString, 4> fan_labels = { "T1", "T2", "T3", "T4" };
    for (int i = 0; i < 4; ++i) {
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
        label->Bind(wxEVT_LEFT_DOWN, [this, choice = speed_labels[i]](wxMouseEvent &) {
            m_selected_speed = choice;
            if (m_speed_display_label != nullptr) {
                m_speed_display_label->SetLabelText(m_selected_speed);
                const int right_value_margin = FromDIP(5);
                const int inter_value_gap = FromDIP(5);
                const int right_placeholder_width = FromDIP(190);
                const int value_right_edge = right_placeholder_width - right_value_margin;
                wxClientDC dc(m_speed_display_label);
                dc.SetFont(m_speed_display_label->GetFont());
                wxCoord fan_unit_width = 0;
                wxCoord fan_unit_height = 0;
                wxCoord fan_value_width = 0;
                wxCoord fan_value_height = 0;
                dc.GetTextExtent("%", &fan_unit_width, &fan_unit_height);
                dc.GetTextExtent("__", &fan_value_width, &fan_value_height);
                const int fan_unit_x = value_right_edge - fan_unit_width;
                const int aligned_speed_x = fan_unit_x - inter_value_gap - fan_value_width;
                const int speed_value_x = std::min(aligned_speed_x, value_right_edge - m_speed_display_label->GetBestSize().GetWidth());
                m_speed_display_label->SetPosition(wxPoint(speed_value_x, FromDIP(200)));
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

void PrinterWebView::select_tab(PrinterWebViewTab tab)
{
    m_selected_tab = tab;

    if (m_status_page != nullptr)
        m_status_page->Show(tab == PrinterWebViewTab::Status);
    if (m_storage_page != nullptr)
        m_storage_page->Show(tab == PrinterWebViewTab::Storage);
    if (m_update_page != nullptr)
        m_update_page->Show(tab == PrinterWebViewTab::Update);
    if (m_assistant_page != nullptr)
        m_assistant_page->Show(tab == PrinterWebViewTab::Assistant);

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
