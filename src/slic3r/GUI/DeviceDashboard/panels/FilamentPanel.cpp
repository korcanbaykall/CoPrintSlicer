#include "FilamentPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"

#include <utility>

#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

void set_panel_colour(wxPanel* panel, const wxColour& colour)
{
    if (panel == nullptr || panel->GetBackgroundColour() == colour)
        return;
    panel->SetBackgroundColour(colour);
    panel->Refresh();
}

void style_action_button(Button* button)
{
    if (button == nullptr)
        return;
    button->SetMinSize(wxSize(-1, button->FromDIP(40)));
    button->SetCornerRadius(button->FromDIP(8));
    button->SetBorderWidth(0);
    button->SetBackgroundColorNormal(wxColour(65, 68, 75));
    button->SetTextColorNormal(wxColour(220, 220, 220));
}

} // namespace

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
    auto* row_sizer = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* model_header = new wxStaticText(rows, wxID_ANY, wxString::FromUTF8("Model Colors"));
    model_header->SetForegroundColour(DeviceUiStyle::text_muted());
    auto* tools_header = new wxStaticText(rows, wxID_ANY, wxString::FromUTF8("Assigned Tools"));
    tools_header->SetForegroundColour(DeviceUiStyle::text_muted());
    header->Add(model_header, 1, wxLEFT, FromDIP(8));
    header->Add(tools_header, 1, wxLEFT, FromDIP(26));
    row_sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto* divider = new wxPanel(rows, wxID_ANY);
    divider->SetMinSize(wxSize(-1, FromDIP(1)));
    divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    divider->SetBackgroundColour(wxColour(44, 129, 255));
    row_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    for (int i = 0; i < MaxDashboardTools; ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* model_box = new StaticBox(rows, wxID_ANY);
        model_box->SetMinSize(wxSize(FromDIP(240), FromDIP(44)));
        model_box->SetCornerRadius(FromDIP(8));
        model_box->SetBorderWidth(0);
        model_box->SetBackgroundColorNormal(DeviceUiStyle::control_background());
        model_box->SetBackgroundColour(DeviceUiStyle::control_background());
        auto* model_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_rows[i].model_color = new wxPanel(model_box, wxID_ANY);
        m_rows[i].model_color->SetMinSize(wxSize(FromDIP(58), FromDIP(36)));
        m_rows[i].model_color->SetBackgroundColour(*wxWHITE);
        model_sizer->Add(m_rows[i].model_color, 0, wxEXPAND | wxALL, FromDIP(4));
        m_rows[i].model_material = new wxStaticText(model_box, wxID_ANY, wxString::FromUTF8("PLA"));
        m_rows[i].model_material->SetForegroundColour(DeviceUiStyle::text_primary());
        {
            wxFont f = m_rows[i].model_material->GetFont();
            f.SetWeight(wxFONTWEIGHT_BOLD);
            m_rows[i].model_material->SetFont(f);
        }
        model_sizer->AddSpacer(FromDIP(12));
        model_sizer->Add(m_rows[i].model_material, 0, wxALIGN_CENTER_VERTICAL);
        model_sizer->AddStretchSpacer(1);
        m_rows[i].model_weight = new wxStaticText(model_box, wxID_ANY, wxString::FromUTF8("--"));
        m_rows[i].model_weight->SetForegroundColour(DeviceUiStyle::text_primary());
        model_sizer->Add(m_rows[i].model_weight, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        model_box->SetSizer(model_sizer);
        row->Add(model_box, 1, wxEXPAND);

        auto* arrow = new wxStaticText(rows, wxID_ANY, wxString::FromUTF8(">"));
        arrow->SetForegroundColour(DeviceUiStyle::text_primary());
        row->Add(arrow, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(14));

        auto* tool_box = new StaticBox(rows, wxID_ANY);
        m_rows[i].tool_button = tool_box;
        tool_box->SetMinSize(wxSize(FromDIP(136), FromDIP(44)));
        tool_box->SetCornerRadius(FromDIP(8));
        tool_box->SetBorderWidth(0);
        tool_box->SetBackgroundColorNormal(DeviceUiStyle::control_background());
        tool_box->SetBackgroundColour(DeviceUiStyle::control_background());
        tool_box->SetCursor(wxCursor(wxCURSOR_HAND));
        auto* tool_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_rows[i].tool_color = new wxPanel(tool_box, wxID_ANY);
        m_rows[i].tool_color->SetMinSize(wxSize(FromDIP(38), FromDIP(36)));
        m_rows[i].tool_color->SetBackgroundColour(DeviceUiStyle::accent());
        tool_sizer->Add(m_rows[i].tool_color, 0, wxEXPAND | wxALL, FromDIP(4));
        m_rows[i].tool_label = new wxStaticText(tool_box, wxID_ANY, wxString::Format("T%d", i + 1));
        m_rows[i].tool_label->SetForegroundColour(DeviceUiStyle::text_primary());
        {
            wxFont f = m_rows[i].tool_label->GetFont();
            f.SetWeight(wxFONTWEIGHT_BOLD);
            m_rows[i].tool_label->SetFont(f);
        }
        tool_sizer->AddSpacer(FromDIP(12));
        tool_sizer->Add(m_rows[i].tool_label, 0, wxALIGN_CENTER_VERTICAL);
        tool_sizer->AddStretchSpacer(1);
        auto* swap = new wxStaticText(tool_box, wxID_ANY, wxString::FromUTF8("<>"));
        swap->SetForegroundColour(DeviceUiStyle::text_primary());
        tool_sizer->Add(swap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        tool_box->SetSizer(tool_sizer);

        const auto open_tool_menu = [this, i, tool_box](wxMouseEvent&) {
            wxMenu menu;
            for (int tool = 0; tool < MaxDashboardTools; ++tool)
                menu.Append(1000 + tool, wxString::Format("T%d", tool + 1));
            menu.Bind(wxEVT_MENU, [this, i](wxCommandEvent& evt) {
                DeviceCommand command;
                command.kind = DeviceCommandKind::AssignModelSlotToTool;
                command.model_slot = i;
                command.tool_index = evt.GetId() - 1000;
                dispatch(command);
            });
            tool_box->PopupMenu(&menu);
        };
        tool_box->Bind(wxEVT_LEFT_DOWN, open_tool_menu);
        m_rows[i].tool_color->Bind(wxEVT_LEFT_DOWN, open_tool_menu);
        m_rows[i].tool_label->Bind(wxEVT_LEFT_DOWN, open_tool_menu);
        swap->Bind(wxEVT_LEFT_DOWN, open_tool_menu);

        row->Add(tool_box, 0, wxEXPAND);
        row_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    rows->SetSizer(row_sizer);
    columns->Add(rows, 1, wxEXPAND | wxRIGHT, FromDIP(18));

    auto* manage = new StaticBox(content, wxID_ANY);
    manage->SetBackgroundColour(DeviceUiStyle::card_background());
    manage->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    manage->SetBorderWidth(0);
    auto* manage_sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(manage, wxID_ANY, wxString::FromUTF8("Manage Filament"));
    title->SetForegroundColour(DeviceUiStyle::text_primary());
    {
        wxFont f = title->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(f);
    }
    auto* sep = new wxPanel(manage, wxID_ANY);
    sep->SetMinSize(wxSize(-1, FromDIP(1)));
    sep->SetMaxSize(wxSize(-1, FromDIP(1)));
    sep->SetBackgroundColour(DeviceUiStyle::card_border());

    auto* selected_box = new StaticBox(manage, wxID_ANY);
    selected_box->SetMinSize(wxSize(FromDIP(188), FromDIP(44)));
    selected_box->SetCornerRadius(FromDIP(8));
    selected_box->SetBorderWidth(1);
    selected_box->SetBorderColorNormal(wxColour(70, 73, 80));
    selected_box->SetBackgroundColorNormal(DeviceUiStyle::control_background());
    selected_box->SetBackgroundColour(DeviceUiStyle::control_background());
    selected_box->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* selected_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_selected_tool_dot = new wxPanel(selected_box, wxID_ANY);
    m_selected_tool_dot->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    m_selected_tool_dot->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    m_selected_tool_dot->SetBackgroundColour(DeviceUiStyle::accent());
    selected_sizer->Add(m_selected_tool_dot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    selected_sizer->AddSpacer(FromDIP(8));
    m_selected_tool = new wxStaticText(selected_box, wxID_ANY, wxString::FromUTF8("Tool 1"));
    m_selected_tool->SetForegroundColour(DeviceUiStyle::text_primary());
    {
        wxFont f = m_selected_tool->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        m_selected_tool->SetFont(f);
    }
    selected_sizer->Add(m_selected_tool, 1, wxALIGN_CENTER_VERTICAL);
    auto* arrow_down = new wxStaticText(selected_box, wxID_ANY, wxString::FromUTF8("v"));
    arrow_down->SetForegroundColour(DeviceUiStyle::text_muted());
    selected_sizer->Add(arrow_down, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    selected_box->SetSizer(selected_sizer);

    const auto open_selected_menu = [this, selected_box](wxMouseEvent&) {
        wxMenu menu;
        for (int tool = 0; tool < MaxDashboardTools; ++tool)
            menu.Append(2000 + tool, wxString::Format("Tool %d", tool + 1));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent& evt) {
            const int tool_index = evt.GetId() - 2000;
            DeviceCommand command;
            command.kind = DeviceCommandKind::SelectFilamentTool;
            command.tool_index = tool_index;
            dispatch(command);
            set_selected_tool(tool_index);
        });
        selected_box->PopupMenu(&menu);
    };
    selected_box->Bind(wxEVT_LEFT_DOWN, open_selected_menu);
    m_selected_tool_dot->Bind(wxEVT_LEFT_DOWN, open_selected_menu);
    m_selected_tool->Bind(wxEVT_LEFT_DOWN, open_selected_menu);
    arrow_down->Bind(wxEVT_LEFT_DOWN, open_selected_menu);

    m_load_button = new Button(manage, wxString::FromUTF8("Load"));
    style_action_button(m_load_button);
    m_load_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::LoadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    m_unload_button = new Button(manage, wxString::FromUTF8("Unload"));
    style_action_button(m_unload_button);
    m_unload_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::UnloadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    manage_sizer->Add(title, 0, wxLEFT | wxTOP, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(8));
    manage_sizer->Add(sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(12));
    manage_sizer->Add(selected_box, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(10));
    manage_sizer->Add(m_load_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(8));
    manage_sizer->Add(m_unload_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddStretchSpacer(1);
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
            set_panel_colour(m_rows[i].model_color, state.model_colors[i]);
        set_label_if_changed(
            m_rows[i].model_material,
            state.model_materials[i].IsEmpty() ? wxString::FromUTF8("N/A") : state.model_materials[i]);
        set_label_if_changed(
            m_rows[i].model_weight,
            state.model_weights[i].IsEmpty() ? wxString::FromUTF8("--") : state.model_weights[i]);

        const int tool_index = state.model_slot_to_tool[i];
        const bool valid_tool = tool_index >= 0 && tool_index < MaxDashboardTools;
        if (m_rows[i].tool_color != nullptr && valid_tool)
            set_panel_colour(m_rows[i].tool_color, state.assigned_colors[i]);
        set_label_if_changed(
            m_rows[i].tool_label,
            valid_tool ? wxString::Format("T%d", tool_index + 1) : wxString::FromUTF8("N/A"));
    }

    set_selected_tool(state.selected_tool);
    if (m_load_button != nullptr)
        m_load_button->Enable(state.can_load_unload);
    if (m_unload_button != nullptr)
        m_unload_button->Enable(state.can_load_unload);

    if (layout_changed)
        Layout();
}

void FilamentPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

void FilamentPanel::dispatch(DeviceCommand command) const
{
    if (m_command_handler)
        m_command_handler(command);
}

void FilamentPanel::set_selected_tool(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    m_selected_tool_index = tool_index;
    if (m_selected_tool != nullptr) {
        const wxString label = wxString::Format("Tool %d", tool_index + 1);
        if (m_selected_tool->GetLabelText() != label)
            m_selected_tool->SetLabelText(label);
    }
    if (m_selected_tool_dot != nullptr && m_rows[tool_index].tool_color != nullptr)
        set_panel_colour(m_selected_tool_dot, m_rows[tool_index].tool_color->GetBackgroundColour());
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
