#ifndef slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_

#include "../DeviceCommandService.hpp"
#include "../DeviceDashboardState.hpp"

#include <functional>
#include <array>

#include <wx/panel.h>

class Button;
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
    enum class SpeedPreset {
        Slow,
        Normal,
        Fast,
        Ultra
    };

    Button* make_tool_button(wxWindow* parent, const wxString& label, bool active = false);
    Button* make_option_button(wxWindow* parent, const wxString& label, bool active = false);
    wxStaticText* make_header_label(wxWindow* parent, const wxString& label);
    void dispatch_axis(Axis axis, double direction) const;
    void dispatch(DeviceCommand command) const;
    void set_active_tool_button(int tool_index);
    void set_active_distance_button(double distance_mm);
    void set_active_speed_button(SpeedPreset preset);

    DeviceCardFrame* m_frame{nullptr};
    std::array<Button*, MaxDashboardTools> m_tool_buttons{nullptr, nullptr, nullptr, nullptr};
    std::array<Button*, 4> m_distance_buttons{nullptr, nullptr, nullptr, nullptr};
    std::array<Button*, 4> m_speed_buttons{nullptr, nullptr, nullptr, nullptr};
    double m_selected_distance_mm{1.0};
    int m_selected_tool{0};
    SpeedPreset m_speed_preset{SpeedPreset::Normal};
    CommandHandler m_command_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_
