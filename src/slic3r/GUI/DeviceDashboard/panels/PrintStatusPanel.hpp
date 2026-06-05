#ifndef slic3r_GUI_DeviceDashboard_panels_PrintStatusPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_PrintStatusPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/panel.h>

class wxGauge;
class wxStaticBitmap;
class wxStaticText;
class Button;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class PrintStatusPanel : public wxPanel
{
public:
    using ActionHandler = std::function<void()>;

    explicit PrintStatusPanel(wxWindow* parent);

    void apply_state(const PrintJobState& state);
    void set_pause_handler(ActionHandler handler);
    void set_stop_handler(ActionHandler handler);

private:
    static wxString time_text(int seconds);

    DeviceCardFrame* m_frame{nullptr};
    wxPanel* m_thumbnail_host{nullptr};
    wxStaticBitmap* m_thumbnail{nullptr};
    wxStaticText* m_file_name{nullptr};
    wxStaticText* m_elapsed_time{nullptr};
    wxStaticText* m_layer_info{nullptr};
    wxStaticText* m_remaining_time{nullptr};
    wxStaticText* m_progress_percent{nullptr};
    wxGauge* m_progress{nullptr};
    Button* m_pause_button{nullptr};
    Button* m_stop_button{nullptr};
    ActionHandler m_pause_handler;
    ActionHandler m_stop_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_PrintStatusPanel_hpp_
