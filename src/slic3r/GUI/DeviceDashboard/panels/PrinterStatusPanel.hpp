#ifndef slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <array>
#include <functional>

#include <wx/panel.h>

class wxStaticText;
class StaticBox;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class PrinterStatusPanel : public wxPanel
{
public:
    using ToolSelectHandler  = std::function<void(int tool_index)>;
    using NozzleTempHandler  = std::function<void(int tool_index)>;
    using BedTempHandler     = std::function<void()>;

    explicit PrinterStatusPanel(wxWindow* parent);

    void apply_state(const std::array<ToolState, MaxDashboardTools>& tools, const BedState& bed);
    void set_active_tool(int tool_index);

    void set_tool_select_handler(ToolSelectHandler handler);
    void set_nozzle_temp_handler(NozzleTempHandler handler);
    void set_bed_temp_handler(BedTempHandler handler);

private:
    struct ToolView {
        StaticBox*    card{nullptr};
        wxStaticText* title{nullptr};
        wxStaticText* temperature{nullptr};
        wxStaticText* fan{nullptr};
    };

    static wxString temperature_text(const TemperatureReading& reading);

    DeviceCardFrame* m_frame{nullptr};
    std::array<ToolView, MaxDashboardTools> m_tools;
    wxStaticText* m_bed_temperature{nullptr};
    int m_active_tool{0};

    ToolSelectHandler m_tool_select_handler;
    NozzleTempHandler m_nozzle_temp_handler;
    BedTempHandler    m_bed_temp_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_
