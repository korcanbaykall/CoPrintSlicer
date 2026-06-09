#ifndef slic3r_GUI_DeviceDashboard_panels_FilamentPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_FilamentPanel_hpp_

#include "../DeviceCommandService.hpp"
#include "../DeviceDashboardState.hpp"

#include <array>
#include <functional>

#include <wx/panel.h>

class wxStaticText;
class Button;
class StaticBox;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class FilamentPanel : public wxPanel
{
public:
    using CommandHandler = std::function<void(const DeviceCommand&)>;

    explicit FilamentPanel(wxWindow* parent);

    void apply_state(const FilamentState& state);
    void set_command_handler(CommandHandler handler);

private:
    struct RowView {
        wxPanel* model_color{nullptr};
        wxStaticText* model_material{nullptr};
        wxStaticText* model_weight{nullptr};
        wxPanel* tool_color{nullptr};
        wxStaticText* tool_label{nullptr};
        StaticBox* tool_button{nullptr};
    };

    void dispatch(DeviceCommand command) const;
    void set_selected_tool(int tool_index);

    DeviceCardFrame* m_frame{nullptr};
    std::array<RowView, MaxDashboardTools> m_rows;
    wxPanel* m_selected_tool_dot{nullptr};
    wxStaticText* m_selected_tool{nullptr};
    Button* m_load_button{nullptr};
    Button* m_unload_button{nullptr};
    int m_selected_tool_index{0};
    CommandHandler m_command_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_FilamentPanel_hpp_
