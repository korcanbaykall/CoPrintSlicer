#ifndef slic3r_GUI_DeviceDashboard_services_DashboardStateAdapter_hpp_
#define slic3r_GUI_DeviceDashboard_services_DashboardStateAdapter_hpp_

#include "../DeviceDashboardState.hpp"

namespace Slic3r {

class MachineObject;

namespace GUI {
namespace DeviceDashboard {

class DashboardStateAdapter
{
public:
    static DeviceDashboardState from_machine(MachineObject* machine);

private:
    static void apply_default_tools(DeviceDashboardState& state);
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_services_DashboardStateAdapter_hpp_
