#ifndef slic3r_GUI_DeviceDashboard_DeviceUiStyle_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceUiStyle_hpp_

#include <wx/colour.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

struct DeviceUiStyle {
    static wxColour page_background();
    static wxColour card_background();
    static wxColour card_border();
    static wxColour control_background();
    static wxColour text_primary();
    static wxColour text_muted();
    static wxColour accent();
    static wxColour danger();

    static int card_radius();
    static int card_border_width();
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceUiStyle_hpp_
