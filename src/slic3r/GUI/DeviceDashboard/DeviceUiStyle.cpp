#include "DeviceUiStyle.hpp"

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

wxColour DeviceUiStyle::page_background() { return wxColour(28, 30, 34); }
wxColour DeviceUiStyle::card_background() { return wxColour(22, 24, 29); }
wxColour DeviceUiStyle::card_border() { return wxColour(55, 58, 64); }
wxColour DeviceUiStyle::control_background() { return wxColour(43, 46, 52); }
wxColour DeviceUiStyle::text_primary() { return wxColour(235, 235, 235); }
wxColour DeviceUiStyle::text_muted() { return wxColour(150, 156, 166); }
wxColour DeviceUiStyle::accent() { return wxColour(44, 182, 125); }
wxColour DeviceUiStyle::danger() { return wxColour(255, 125, 114); }

int DeviceUiStyle::card_radius() { return 8; }
int DeviceUiStyle::card_border_width() { return 1; }

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
