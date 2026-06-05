#ifndef slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/panel.h>

class wxStaticText;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class CameraPanel : public wxPanel
{
public:
    using RefreshHandler = std::function<void()>;

    explicit CameraPanel(wxWindow* parent);

    void apply_state(const CameraState& state);
    void set_refresh_handler(RefreshHandler handler);

private:
    DeviceCardFrame* m_frame{nullptr};
    wxPanel* m_viewport{nullptr};
    wxStaticText* m_empty_state{nullptr};
    RefreshHandler m_refresh_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
