#include "MovementPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"

#include <utility>

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

MovementPanel::MovementPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Movement"));

    auto* content = new wxPanel(m_frame->content_parent(), wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* grid = new wxFlexGridSizer(2, 3, FromDIP(10), FromDIP(18));
    grid->AddGrowableCol(0, 1);
    grid->AddGrowableCol(1, 1);
    grid->AddGrowableCol(2, 1);

    grid->Add(make_value_label(content, wxString::FromUTF8("Tool Selection")), 0, wxEXPAND);
    grid->Add(make_value_label(content, wxString::FromUTF8("Motion Distance")), 0, wxEXPAND);
    grid->Add(make_value_label(content, wxString::FromUTF8("Printing Speed")), 0, wxEXPAND);

    m_selected_tool = make_value_label(content, wxString::FromUTF8("T1"));
    m_distance = make_value_label(content, wxString::FromUTF8("1mm"));
    m_speed = make_value_label(content, wxString::FromUTF8("100%"));
    grid->Add(m_selected_tool, 0, wxEXPAND);
    grid->Add(m_distance, 0, wxEXPAND);
    grid->Add(m_speed, 0, wxEXPAND);

    content->SetSizer(grid);
    m_frame->set_content(content);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void MovementPanel::apply_state(const MovementState& state)
{
    if (m_selected_tool != nullptr)
        m_selected_tool->SetLabelText(wxString::Format("T%d", state.selected_tool + 1));
    if (m_distance != nullptr)
        m_distance->SetLabelText(wxString::Format("%.0fmm", state.selected_distance_mm));
    if (m_speed != nullptr)
        m_speed->SetLabelText(wxString::Format("%d%%", state.print_speed_percent));
    Layout();
}

void MovementPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

wxStaticText* MovementPanel::make_value_label(wxWindow* parent, const wxString& label)
{
    auto* text = new wxStaticText(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    text->SetForegroundColour(DeviceUiStyle::text_primary());
    return text;
}

void MovementPanel::dispatch(DeviceCommand command) const
{
    if (m_command_handler)
        m_command_handler(command);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
