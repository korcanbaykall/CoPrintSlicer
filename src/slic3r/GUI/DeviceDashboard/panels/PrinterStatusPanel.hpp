#ifndef slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <array>

#include <wx/panel.h>

class wxStaticText;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class PrinterStatusPanel : public wxPanel
{
public:
    explicit PrinterStatusPanel(wxWindow* parent);

    void apply_state(const std::array<ToolState, MaxDashboardTools>& tools, const BedState& bed);

private:
    struct ToolView {
        wxStaticText* title{nullptr};
        wxStaticText* temperature{nullptr};
        wxStaticText* fan{nullptr};
    };

    static wxString temperature_text(const TemperatureReading& reading);

    DeviceCardFrame* m_frame{nullptr};
    std::array<ToolView, MaxDashboardTools> m_tools;
    wxStaticText* m_bed_temperature{nullptr};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_
