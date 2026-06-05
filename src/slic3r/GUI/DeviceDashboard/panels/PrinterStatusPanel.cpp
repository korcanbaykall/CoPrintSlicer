#include "PrinterStatusPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../Widgets/StaticBox.hpp"

#include <utility>

#include <wx/sizer.h>
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

PrinterStatusPanel::PrinterStatusPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Printer Status"));

    auto* content = new wxPanel(m_frame->content_parent(), wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* grid = new wxBoxSizer(wxHORIZONTAL);

    // Tool kartları: her biri StaticBox — tool seçimi ve sıcaklık diyaloğu için tıklanabilir
    for (int i = 0; i < MaxDashboardTools; ++i) {
        if (i > 0)
            grid->AddSpacer(FromDIP(10));

        auto* card = new StaticBox(content, wxID_ANY);
        card->SetMinSize(wxSize(-1, FromDIP(90)));
        card->SetCornerRadius(FromDIP(10));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(i == 0 ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
        card->SetBackgroundColorNormal(DeviceUiStyle::control_background());
        card->SetBackgroundColour(DeviceUiStyle::control_background());
        card->SetCursor(wxCursor(wxCURSOR_HAND));
        m_tools[i].card = card;

        auto* card_sizer = new wxBoxSizer(wxVERTICAL);

        m_tools[i].title = new wxStaticText(card, wxID_ANY, wxString::Format("Tool %d", i + 1),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL | wxST_NO_AUTORESIZE);
        m_tools[i].title->SetForegroundColour(i == 0 ? DeviceUiStyle::text_primary() : DeviceUiStyle::text_muted());
        wxFont title_font = m_tools[i].title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        m_tools[i].title->SetFont(title_font);

        m_tools[i].temperature = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("-- / --"),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_tools[i].temperature->SetForegroundColour(i == 0 ? DeviceUiStyle::text_primary() : DeviceUiStyle::text_muted());
        m_tools[i].temperature->SetCursor(wxCursor(wxCURSOR_HAND));

        m_tools[i].fan = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("--%"),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_tools[i].fan->SetForegroundColour(DeviceUiStyle::text_muted());

        card_sizer->AddSpacer(FromDIP(8));
        card_sizer->Add(m_tools[i].title,       0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        card_sizer->AddSpacer(FromDIP(6));
        card_sizer->Add(m_tools[i].temperature, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        card_sizer->AddSpacer(FromDIP(6));
        card_sizer->Add(m_tools[i].fan,         0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        card_sizer->AddSpacer(FromDIP(8));
        card->SetSizer(card_sizer);

        // Karta tıklanınca tool seç
        const int idx = i;
        card->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            if (m_tool_select_handler) m_tool_select_handler(idx);
        });
        m_tools[i].title->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            if (m_tool_select_handler) m_tool_select_handler(idx);
        });
        // Sıcaklık etiketine tıklanınca hedef sıcaklık diyaloğu
        m_tools[i].temperature->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            if (m_tool_select_handler)  m_tool_select_handler(idx);
            if (m_nozzle_temp_handler)  m_nozzle_temp_handler(idx);
        });
        m_tools[i].fan->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            if (m_tool_select_handler) m_tool_select_handler(idx);
        });

        grid->Add(card, 1, wxEXPAND);
    }

    // Build Plate kartı
    {
        grid->AddSpacer(FromDIP(10));
        auto* card = new StaticBox(content, wxID_ANY);
        card->SetMinSize(wxSize(-1, FromDIP(90)));
        card->SetCornerRadius(FromDIP(10));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(wxColour(55, 58, 64));
        card->SetBackgroundColorNormal(DeviceUiStyle::control_background());
        card->SetBackgroundColour(DeviceUiStyle::control_background());
        card->SetCursor(wxCursor(wxCURSOR_HAND));

        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        auto* bed_title = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("Build Plate"),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL | wxST_NO_AUTORESIZE);
        bed_title->SetForegroundColour(DeviceUiStyle::text_muted());
        wxFont bt_font = bed_title->GetFont();
        bt_font.SetWeight(wxFONTWEIGHT_BOLD);
        bed_title->SetFont(bt_font);

        m_bed_temperature = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("-- / --"),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_bed_temperature->SetForegroundColour(DeviceUiStyle::text_muted());
        m_bed_temperature->SetCursor(wxCursor(wxCURSOR_HAND));

        card_sizer->AddSpacer(FromDIP(8));
        card_sizer->Add(bed_title,         0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        card_sizer->AddSpacer(FromDIP(6));
        card_sizer->Add(m_bed_temperature, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        card_sizer->AddSpacer(FromDIP(8));
        card->SetSizer(card_sizer);

        card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_bed_temp_handler) m_bed_temp_handler();
        });
        m_bed_temperature->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_bed_temp_handler) m_bed_temp_handler();
        });

        grid->Add(card, 1, wxEXPAND);
    }

    content->SetSizer(grid);
    m_frame->set_content(content);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void PrinterStatusPanel::apply_state(const std::array<ToolState, MaxDashboardTools>& tools, const BedState& bed)
{
    bool layout_needed = false;

    for (int i = 0; i < MaxDashboardTools; ++i) {
        const ToolState& tool = tools[i];
        layout_needed |= set_label_if_changed(m_tools[i].title, tool.label.IsEmpty() ? wxString::Format("Tool %d", i + 1) : tool.label);
        layout_needed |= set_label_if_changed(m_tools[i].temperature, temperature_text(tool.nozzle));
        layout_needed |= set_label_if_changed(m_tools[i].fan, tool.fan.available ? wxString::Format("%d%%", tool.fan.percent) : wxString::FromUTF8("--%"));
    }

    layout_needed |= set_label_if_changed(m_bed_temperature, temperature_text(bed.temperature));

    if (layout_needed) {
        // Freeze/Thaw: tüm label güncellemeleri bittikten sonra
        // tek seferde çiz — ara siyahlaşmayı önler
        Freeze();
        Layout();
        Thaw();
    }
}

void PrinterStatusPanel::set_active_tool(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    if (m_active_tool == tool_index)
        return;

    m_active_tool = tool_index;

    for (int i = 0; i < MaxDashboardTools; ++i) {
        const bool active = i == m_active_tool;
        if (m_tools[i].card != nullptr) {
            m_tools[i].card->SetBorderColorNormal(active ? wxColour(44, 182, 125) : wxColour(55, 58, 64));
            m_tools[i].card->Refresh();
        }
        if (m_tools[i].title != nullptr) {
            m_tools[i].title->SetForegroundColour(active ? DeviceUiStyle::text_primary() : DeviceUiStyle::text_muted());
            m_tools[i].title->Refresh();
        }
        if (m_tools[i].temperature != nullptr) {
            m_tools[i].temperature->SetForegroundColour(active ? DeviceUiStyle::text_primary() : DeviceUiStyle::text_muted());
            m_tools[i].temperature->Refresh();
        }
    }
}

void PrinterStatusPanel::set_tool_select_handler(ToolSelectHandler handler)  { m_tool_select_handler = std::move(handler); }
void PrinterStatusPanel::set_nozzle_temp_handler(NozzleTempHandler handler)  { m_nozzle_temp_handler = std::move(handler); }
void PrinterStatusPanel::set_bed_temp_handler(BedTempHandler handler)        { m_bed_temp_handler    = std::move(handler); }

wxString PrinterStatusPanel::temperature_text(const TemperatureReading& reading)
{
    if (!reading.available)
        return wxString::FromUTF8("N/A");
    return wxString::Format("%.1f / %.1f", reading.current, reading.target);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
