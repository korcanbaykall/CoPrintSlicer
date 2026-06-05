#include "DeviceDashboardPage.hpp"

#include "DeviceUiStyle.hpp"
#include "panels/CameraPanel.hpp"
#include "panels/FilamentPanel.hpp"
#include "panels/MovementPanel.hpp"
#include "panels/PrinterStatusPanel.hpp"
#include "panels/PrintStatusPanel.hpp"

#include <utility>

#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

DeviceDashboardPage::DeviceDashboardPage(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxHORIZONTAL);
    auto* left_column = new wxBoxSizer(wxVERTICAL);
    auto* right_column = new wxBoxSizer(wxVERTICAL);

    m_camera_panel = new CameraPanel(this);
    m_print_status_panel = new PrintStatusPanel(this);
    m_movement_panel = new MovementPanel(this);
    m_printer_status_panel = new PrinterStatusPanel(this);
    m_filament_panel = new FilamentPanel(this);

    left_column->Add(m_camera_panel, 1, wxEXPAND | wxBOTTOM, FromDIP(10));
    left_column->Add(m_print_status_panel, 0, wxEXPAND);

    right_column->Add(m_movement_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    right_column->Add(m_printer_status_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    right_column->Add(m_filament_panel, 1, wxEXPAND);

    root->Add(left_column, 1, wxEXPAND | wxRIGHT, FromDIP(10));
    root->Add(right_column, 1, wxEXPAND);
    SetSizer(root);

    auto forward_command = [this](const DeviceCommand& command) {
        if (m_command_handler)
            m_command_handler(command);
    };
    m_movement_panel->set_command_handler(forward_command);
    m_filament_panel->set_command_handler(forward_command);
}

void DeviceDashboardPage::apply_state(const DeviceDashboardState& state)
{
    if (m_camera_panel != nullptr)
        m_camera_panel->apply_state(state.camera);
    if (m_print_status_panel != nullptr)
        m_print_status_panel->apply_state(state.print_job);
    if (m_movement_panel != nullptr)
        m_movement_panel->apply_state(state.movement);
    if (m_printer_status_panel != nullptr)
        m_printer_status_panel->apply_state(state.tools, state.bed);
    if (m_filament_panel != nullptr)
        m_filament_panel->apply_state(state.filament);

    Layout();
}

void DeviceDashboardPage::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
