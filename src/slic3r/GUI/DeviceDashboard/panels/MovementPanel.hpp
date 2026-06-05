#ifndef slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_

#include "../DeviceCommandService.hpp"
#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/panel.h>

class wxStaticText;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class MovementPanel : public wxPanel
{
public:
    using CommandHandler = std::function<void(const DeviceCommand&)>;

    explicit MovementPanel(wxWindow* parent);

    void apply_state(const MovementState& state);
    void set_command_handler(CommandHandler handler);

private:
    wxStaticText* make_value_label(wxWindow* parent, const wxString& label);
    void dispatch(DeviceCommand command) const;

    DeviceCardFrame* m_frame{nullptr};
    wxStaticText* m_selected_tool{nullptr};
    wxStaticText* m_distance{nullptr};
    wxStaticText* m_speed{nullptr};
    CommandHandler m_command_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_
