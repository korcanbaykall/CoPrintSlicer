#ifndef slic3r_GUI_DeviceDashboard_DeviceDashboardPage_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceDashboardPage_hpp_

#include "DeviceCommandService.hpp"
#include "DeviceDashboardState.hpp"

#include <functional>

#include <wx/panel.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class CameraPanel;
class FilamentPanel;
class MovementPanel;
class PrinterStatusPanel;
class PrintStatusPanel;

class DeviceDashboardPage : public wxPanel
{
public:
    using CommandHandler = std::function<void(const DeviceCommand&)>;

    explicit DeviceDashboardPage(wxWindow* parent);

    void apply_state(const DeviceDashboardState& state);
    void set_command_handler(CommandHandler handler);

private:
    CameraPanel* m_camera_panel{nullptr};
    PrintStatusPanel* m_print_status_panel{nullptr};
    MovementPanel* m_movement_panel{nullptr};
    PrinterStatusPanel* m_printer_status_panel{nullptr};
    FilamentPanel* m_filament_panel{nullptr};
    CommandHandler m_command_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceDashboardPage_hpp_
