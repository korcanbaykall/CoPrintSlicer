#include "PrinterStatusPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

PrinterStatusPanel::PrinterStatusPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Printer Status"));

    auto* content = new wxPanel(m_frame->content_parent(), wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* grid = new wxFlexGridSizer(1, MaxDashboardTools + 1, FromDIP(10), FromDIP(10));
    for (int col = 0; col < MaxDashboardTools + 1; ++col)
        grid->AddGrowableCol(col, 1);

    for (int i = 0; i < MaxDashboardTools; ++i) {
        auto* tool_box = new wxPanel(content, wxID_ANY);
        tool_box->SetBackgroundColour(DeviceUiStyle::control_background());
        auto* tool_sizer = new wxBoxSizer(wxVERTICAL);

        m_tools[i].title = new wxStaticText(tool_box, wxID_ANY, wxString::Format("Tool %d", i + 1), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_tools[i].temperature = new wxStaticText(tool_box, wxID_ANY, wxString::FromUTF8("0 / 0"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_tools[i].fan = new wxStaticText(tool_box, wxID_ANY, wxString::FromUTF8("0%"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_tools[i].title->SetForegroundColour(DeviceUiStyle::text_primary());
        m_tools[i].temperature->SetForegroundColour(DeviceUiStyle::text_primary());
        m_tools[i].fan->SetForegroundColour(DeviceUiStyle::text_muted());

        tool_sizer->Add(m_tools[i].title, 0, wxEXPAND | wxTOP, FromDIP(8));
        tool_sizer->Add(m_tools[i].temperature, 0, wxEXPAND | wxTOP, FromDIP(8));
        tool_sizer->Add(m_tools[i].fan, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
        tool_box->SetSizer(tool_sizer);
        grid->Add(tool_box, 1, wxEXPAND);
    }

    auto* bed_box = new wxPanel(content, wxID_ANY);
    bed_box->SetBackgroundColour(DeviceUiStyle::control_background());
    auto* bed_sizer = new wxBoxSizer(wxVERTICAL);
    auto* bed_title = new wxStaticText(bed_box, wxID_ANY, wxString::FromUTF8("Build Plate"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    bed_title->SetForegroundColour(DeviceUiStyle::text_primary());
    m_bed_temperature = new wxStaticText(bed_box, wxID_ANY, wxString::FromUTF8("0 / 0"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    m_bed_temperature->SetForegroundColour(DeviceUiStyle::text_muted());
    bed_sizer->Add(bed_title, 0, wxEXPAND | wxTOP, FromDIP(8));
    bed_sizer->Add(m_bed_temperature, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
    bed_box->SetSizer(bed_sizer);
    grid->Add(bed_box, 1, wxEXPAND);

    content->SetSizer(grid);
    m_frame->set_content(content);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void PrinterStatusPanel::apply_state(const std::array<ToolState, MaxDashboardTools>& tools, const BedState& bed)
{
    for (int i = 0; i < MaxDashboardTools; ++i) {
        const ToolState& tool = tools[i];
        if (m_tools[i].title != nullptr)
            m_tools[i].title->SetLabelText(tool.label.IsEmpty() ? wxString::Format("Tool %d", i + 1) : tool.label);
        if (m_tools[i].temperature != nullptr)
            m_tools[i].temperature->SetLabelText(temperature_text(tool.nozzle));
        if (m_tools[i].fan != nullptr)
            m_tools[i].fan->SetLabelText(tool.fan.available ? wxString::Format("%d%%", tool.fan.percent) : wxString::FromUTF8("N/A"));
    }

    if (m_bed_temperature != nullptr)
        m_bed_temperature->SetLabelText(temperature_text(bed.temperature));

    Layout();
}

wxString PrinterStatusPanel::temperature_text(const TemperatureReading& reading)
{
    if (!reading.available)
        return wxString::FromUTF8("N/A");
    return wxString::Format("%.1f / %.1f", reading.current, reading.target);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
