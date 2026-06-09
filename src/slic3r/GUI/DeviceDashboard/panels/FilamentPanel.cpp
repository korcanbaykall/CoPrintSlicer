#include "FilamentPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"

#include <utility>

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

FilamentPanel::FilamentPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Filament"));

    auto* content = new wxPanel(m_frame->content_parent(), wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* columns = new wxBoxSizer(wxHORIZONTAL);

    auto* rows = new wxPanel(content, wxID_ANY);
    rows->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* row_sizer = new wxFlexGridSizer(MaxDashboardTools, 4, FromDIP(8), FromDIP(10));
    row_sizer->AddGrowableCol(1, 1);
    row_sizer->AddGrowableCol(3, 1);

    for (int i = 0; i < MaxDashboardTools; ++i) {
        m_rows[i].model_color = new wxPanel(rows, wxID_ANY);
        m_rows[i].model_color->SetMinSize(wxSize(FromDIP(40), FromDIP(28)));
        m_rows[i].model_color->SetBackgroundColour(*wxWHITE);
        m_rows[i].model_material = new wxStaticText(rows, wxID_ANY, wxString::FromUTF8("PLA"));
        m_rows[i].model_material->SetForegroundColour(DeviceUiStyle::text_primary());
        m_rows[i].tool_color = new wxPanel(rows, wxID_ANY);
        m_rows[i].tool_color->SetMinSize(wxSize(FromDIP(40), FromDIP(28)));
        m_rows[i].tool_color->SetBackgroundColour(DeviceUiStyle::accent());
        m_rows[i].tool_label = new wxStaticText(rows, wxID_ANY, wxString::Format("T%d", i + 1));
        m_rows[i].tool_label->SetForegroundColour(DeviceUiStyle::text_primary());

        row_sizer->Add(m_rows[i].model_color, 0, wxEXPAND);
        row_sizer->Add(m_rows[i].model_material, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->Add(m_rows[i].tool_color, 0, wxEXPAND);
        row_sizer->Add(m_rows[i].tool_label, 0, wxALIGN_CENTER_VERTICAL);
    }
    rows->SetSizer(row_sizer);
    columns->Add(rows, 1, wxEXPAND | wxRIGHT, FromDIP(18));

    auto* manage = new wxPanel(content, wxID_ANY);
    manage->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* manage_sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(manage, wxID_ANY, wxString::FromUTF8("Manage Filament"));
    title->SetForegroundColour(DeviceUiStyle::text_primary());
    m_selected_tool = new wxStaticText(manage, wxID_ANY, wxString::FromUTF8("Tool 1"));
    m_selected_tool->SetForegroundColour(DeviceUiStyle::text_primary());
    manage_sizer->Add(title, 0, wxBOTTOM, FromDIP(12));
    manage_sizer->Add(m_selected_tool, 0, wxBOTTOM, FromDIP(8));
    manage->SetSizer(manage_sizer);
    columns->Add(manage, 0, wxEXPAND);

    content->SetSizer(columns);
    m_frame->set_content(content);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void FilamentPanel::apply_state(const FilamentState& state)
{
    bool layout_changed = false;
    auto set_label_if_changed = [&layout_changed](wxStaticText* label, const wxString& text) {
        if (label == nullptr || label->GetLabelText() == text)
            return;
        label->SetLabelText(text);
        layout_changed = true;
    };

    for (int i = 0; i < MaxDashboardTools; ++i) {
        if (m_rows[i].model_color != nullptr)
            m_rows[i].model_color->SetBackgroundColour(state.model_colors[i]);
        set_label_if_changed(
            m_rows[i].model_material,
            state.model_materials[i].IsEmpty() ? wxString::FromUTF8("N/A") : state.model_materials[i]);

        const int tool_index = state.model_slot_to_tool[i];
        const bool valid_tool = tool_index >= 0 && tool_index < MaxDashboardTools;
        if (m_rows[i].tool_color != nullptr && valid_tool)
            m_rows[i].tool_color->SetBackgroundColour(state.assigned_colors[i]);
        set_label_if_changed(
            m_rows[i].tool_label,
            valid_tool ? wxString::Format("T%d", tool_index + 1) : wxString::FromUTF8("N/A"));
    }

    set_label_if_changed(m_selected_tool, wxString::Format("Tool %d", state.selected_tool + 1));

    if (layout_changed)
        Layout();
}

void FilamentPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
