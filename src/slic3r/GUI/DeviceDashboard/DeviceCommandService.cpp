#include "DeviceCommandService.hpp"

#include <utility>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

DeviceCommandService::DeviceCommandService(Dispatcher dispatcher)
    : m_dispatcher(std::move(dispatcher))
{
}

void DeviceCommandService::set_dispatcher(Dispatcher dispatcher)
{
    m_dispatcher = std::move(dispatcher);
}

void DeviceCommandService::select_tool(int tool_index) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::SelectTool;
    command.tool_index = tool_index;
    dispatch(command);
}

void DeviceCommandService::move_axis(Axis axis, double distance_mm) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::MoveAxis;
    command.axis = axis;
    command.value = distance_mm;
    dispatch(command);
}

void DeviceCommandService::home() const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::Home;
    dispatch(command);
}

void DeviceCommandService::set_motion_distance(double distance_mm) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::SetMotionDistance;
    command.value = distance_mm;
    dispatch(command);
}

void DeviceCommandService::set_print_speed_percent(int percent) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::SetPrintSpeed;
    command.value = percent;
    dispatch(command);
}

void DeviceCommandService::pause_print() const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::PausePrint;
    dispatch(command);
}

void DeviceCommandService::resume_print() const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::ResumePrint;
    dispatch(command);
}

void DeviceCommandService::stop_print() const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::StopPrint;
    dispatch(command);
}

void DeviceCommandService::load_filament(int tool_index) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::LoadFilament;
    command.tool_index = tool_index;
    dispatch(command);
}

void DeviceCommandService::unload_filament(int tool_index) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::UnloadFilament;
    command.tool_index = tool_index;
    dispatch(command);
}

void DeviceCommandService::assign_model_slot_to_tool(int model_slot, int tool_index) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::AssignModelSlotToTool;
    command.model_slot = model_slot;
    command.tool_index = tool_index;
    dispatch(command);
}

void DeviceCommandService::dispatch(DeviceCommand command) const
{
    if (m_dispatcher)
        m_dispatcher(command);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
