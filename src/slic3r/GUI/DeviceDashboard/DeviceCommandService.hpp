#ifndef slic3r_GUI_DeviceDashboard_DeviceCommandService_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceCommandService_hpp_

#include <functional>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

enum class Axis {
    X,
    Y,
    Z
};

enum class DeviceCommandKind {
    SelectTool,
    SelectFilamentTool,
    MoveAxis,
    Home,
    SetMotionDistance,
    SetPrintSpeed,
    PausePrint,
    ResumePrint,
    StopPrint,
    LoadFilament,
    UnloadFilament,
    AssignModelSlotToTool
};

struct DeviceCommand {
    DeviceCommandKind kind{DeviceCommandKind::SelectTool};
    Axis axis{Axis::X};
    int tool_index{0};
    int model_slot{0};
    double value{0.0};
};

class DeviceCommandService
{
public:
    using Dispatcher = std::function<void(const DeviceCommand&)>;

    explicit DeviceCommandService(Dispatcher dispatcher = {});

    void set_dispatcher(Dispatcher dispatcher);

    void select_tool(int tool_index) const;
    void move_axis(Axis axis, double distance_mm) const;
    void home() const;
    void set_motion_distance(double distance_mm) const;
    void set_print_speed_percent(int percent) const;
    void pause_print() const;
    void resume_print() const;
    void stop_print() const;
    void load_filament(int tool_index) const;
    void unload_filament(int tool_index) const;
    void assign_model_slot_to_tool(int model_slot, int tool_index) const;

private:
    void dispatch(DeviceCommand command) const;

    Dispatcher m_dispatcher;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceCommandService_hpp_
