#include "PrintStatusPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../Widgets/Button.hpp"
#include "../../I18N.hpp"

#include <algorithm>
#include <utility>

#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

bool set_label_if_changed(wxStaticText* label, const wxString& text)
{
    if (label == nullptr || label->GetLabelText() == text)
        return false;
    label->SetLabelText(text);
    return true;
}

} // namespace

PrintStatusPanel::PrintStatusPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Print Status"));

    auto* content = new wxPanel(m_frame->content_parent(), wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_thumbnail_host = new wxPanel(content, wxID_ANY);
    m_thumbnail_host->SetBackgroundColour(*wxBLACK);
    m_thumbnail_host->SetMinSize(wxSize(FromDIP(240), FromDIP(170)));
    auto* thumbnail_sizer = new wxBoxSizer(wxVERTICAL);
    m_thumbnail = new wxStaticBitmap(m_thumbnail_host, wxID_ANY, wxNullBitmap);
    thumbnail_sizer->AddStretchSpacer(1);
    thumbnail_sizer->Add(m_thumbnail, 0, wxALIGN_CENTER);
    thumbnail_sizer->AddStretchSpacer(1);
    m_thumbnail_host->SetSizer(thumbnail_sizer);
    content_sizer->Add(m_thumbnail_host, 0, wxEXPAND | wxRIGHT, FromDIP(14));

    auto* details = new wxPanel(content, wxID_ANY);
    details->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* details_sizer = new wxBoxSizer(wxVERTICAL);

    auto* printing_label = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Printing File:"));
    printing_label->SetForegroundColour(DeviceUiStyle::accent());
    m_file_name = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("N/A"));
    m_file_name->SetForegroundColour(DeviceUiStyle::text_primary());
    details_sizer->Add(printing_label, 0, wxBOTTOM, FromDIP(2));
    details_sizer->Add(m_file_name, 0, wxBOTTOM, FromDIP(8));

    m_elapsed_time = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Total: N/A"));
    m_elapsed_time->SetForegroundColour(DeviceUiStyle::text_primary());
    details_sizer->Add(m_elapsed_time, 0, wxBOTTOM, FromDIP(10));

    auto* progress_row = new wxBoxSizer(wxHORIZONTAL);
    m_progress = new wxGauge(details, wxID_ANY, 100);
    m_progress->SetValue(0);
    progress_row->Add(m_progress, 1, wxALIGN_CENTER_VERTICAL);
    m_progress_percent = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("0%"));
    m_progress_percent->SetForegroundColour(DeviceUiStyle::text_primary());
    progress_row->Add(m_progress_percent, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    details_sizer->Add(progress_row, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    auto* lower_row = new wxBoxSizer(wxHORIZONTAL);
    m_layer_info = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Layer: N/A/N/A"));
    m_layer_info->SetForegroundColour(DeviceUiStyle::text_primary());
    lower_row->Add(m_layer_info, 1, wxALIGN_CENTER_VERTICAL);
    m_remaining_time = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Remaining: N/A"));
    m_remaining_time->SetForegroundColour(DeviceUiStyle::text_primary());
    lower_row->Add(m_remaining_time, 0, wxALIGN_CENTER_VERTICAL);
    details_sizer->Add(lower_row, 0, wxEXPAND);

    auto* action_row = new wxBoxSizer(wxHORIZONTAL);
    m_pause_button = new Button(details, _L("Pause"), "print_control_pause_amber", 0, 14);
    m_pause_button->SetMinSize(wxSize(FromDIP(80), FromDIP(40)));
    m_pause_button->SetMaxSize(wxSize(FromDIP(80), FromDIP(40)));
    m_pause_button->SetCornerRadius(FromDIP(8));
    m_pause_button->SetBackgroundColorNormal(wxColour(0xFF, 0xF9, 0xF1));
    m_pause_button->SetBorderColorNormal(wxColour(0xD7, 0xA4, 0x6D));
    m_pause_button->SetTextColorNormal(wxColour(0xD7, 0xA4, 0x6D));
    m_stop_button = new Button(details, _L("Stop"), "print_control_stop_red", 0, 14);
    m_stop_button->SetMinSize(wxSize(FromDIP(80), FromDIP(40)));
    m_stop_button->SetMaxSize(wxSize(FromDIP(80), FromDIP(40)));
    m_stop_button->SetCornerRadius(FromDIP(8));
    m_stop_button->SetBackgroundColorNormal(wxColour(0xFF, 0xF9, 0xF9));
    m_stop_button->SetBorderColorNormal(wxColour(0xFF, 0x7D, 0x72));
    m_stop_button->SetTextColorNormal(wxColour(0xFF, 0x7D, 0x72));
    action_row->Add(m_pause_button, 0, wxRIGHT, FromDIP(10));
    action_row->Add(m_stop_button, 0);
    details_sizer->Add(action_row, 0, wxTOP, FromDIP(14));

    m_pause_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_pause_handler)
            m_pause_handler();
    });
    m_stop_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_stop_handler)
            m_stop_handler();
    });

    details->SetSizer(details_sizer);
    content_sizer->Add(details, 1, wxEXPAND);
    content->SetSizer(content_sizer);

    m_frame->set_content(content);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void PrintStatusPanel::apply_state(const PrintJobState& state)
{
    bool layout_needed = false;

    layout_needed |= set_label_if_changed(m_file_name, state.file_name.IsEmpty() ? wxString::FromUTF8("N/A") : state.file_name);

    const int progress = std::clamp(state.progress_percent, 0, 100);
    if (m_progress != nullptr && m_progress->GetValue() != progress)
        m_progress->SetValue(progress);
    layout_needed |= set_label_if_changed(m_progress_percent, wxString::Format("%d%%", progress));

    layout_needed |= set_label_if_changed(m_elapsed_time, wxString::FromUTF8("Total: ") + time_text(state.elapsed_seconds));
    layout_needed |= set_label_if_changed(m_layer_info, wxString::Format("Layer: %d/%d", state.current_layer, state.total_layers));
    layout_needed |= set_label_if_changed(m_remaining_time, wxString::FromUTF8("Remaining: ") + time_text(state.remaining_seconds));

    if (layout_needed) {
        Freeze();
        Layout();
        Thaw();
    }
}

void PrintStatusPanel::set_pause_handler(ActionHandler handler)
{
    m_pause_handler = std::move(handler);
}

void PrintStatusPanel::set_stop_handler(ActionHandler handler)
{
    m_stop_handler = std::move(handler);
}

wxString PrintStatusPanel::time_text(int seconds)
{
    if (seconds < 0)
        return wxString::FromUTF8("N/A");

    const int minutes = seconds / 60;
    const int hours = minutes / 60;
    const int remaining_minutes = minutes % 60;
    if (hours > 0)
        return wxString::Format("%dh %dm", hours, remaining_minutes);
    return wxString::Format("%dm", remaining_minutes);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
