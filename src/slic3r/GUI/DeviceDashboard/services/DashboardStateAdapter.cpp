#include "DashboardStateAdapter.hpp"

#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

DeviceDashboardState DashboardStateAdapter::from_machine(MachineObject* machine)
{
    DeviceDashboardState state;
    apply_default_tools(state);

    if (machine == nullptr) {
        state.connection.status = ConnectionStatus::Offline;
        state.connection.can_send_commands = false;
        return state;
    }

    state.printer.id = machine->get_dev_id();
    state.printer.name = machine->get_dev_name();
    state.printer.ip = machine->get_dev_ip();
    state.printer.type = machine->printer_type;
    state.printer.firmware_version = machine->get_ota_version();

    state.connection.status = machine->is_online() ? ConnectionStatus::Online : ConnectionStatus::Offline;
    state.connection.can_send_commands = machine->is_online();

    if (auto* extruders = machine->GetExtderSystem()) {
        for (int i = 0; i < MaxDashboardTools; ++i) {
            ToolState& tool = state.tools[i];
            tool.available = true;
            tool.nozzle.available = true;
            tool.nozzle.current = static_cast<double>(extruders->GetNozzleTempCurrent(i));
            tool.nozzle.target = static_cast<double>(extruders->GetNozzleTempTarget(i));
            state.filament.tools[i].nozzle = tool.nozzle;
            state.filament.tools[i].available = tool.available;
        }
    }

    if (auto* bed = machine->GetBed()) {
        state.bed.temperature.available = true;
        state.bed.temperature.current = static_cast<double>(bed->GetBedTemp());
        state.bed.temperature.target = static_cast<double>(bed->GetBedTempTarget());
    }

    if (auto* fan = machine->GetFan()) {
        const int fan_percent = static_cast<int>(std::round(fan->GetCoolingFanSpeed() / 25.5f));
        for (int i = 0; i < MaxDashboardTools; ++i) {
            state.tools[i].fan.available = true;
            state.tools[i].fan.percent = fan_percent;
            state.filament.tools[i].fan = state.tools[i].fan;
        }
    }

    if (!machine->subtask_name.empty())
        state.print_job.file_name = from_u8(machine->subtask_name);
    else if (!machine->m_gcode_file.empty())
        state.print_job.file_name = from_u8(machine->m_gcode_file);
    if (machine->slice_info != nullptr)
        state.print_job.thumbnail_url = from_u8(machine->slice_info->thumbnail_url);

    state.print_job.has_active_job = machine->is_in_printing();
    state.print_job.progress_percent = std::clamp(machine->mc_print_percent, 0, 100);
    state.print_job.current_layer = machine->curr_layer;
    state.print_job.total_layers = machine->total_layers;
    state.print_job.remaining_seconds = machine->mc_left_time;

    state.movement.can_move = state.connection.can_send_commands;
    return state;
}

void DashboardStateAdapter::apply_default_tools(DeviceDashboardState& state)
{
    const std::array<wxColour, MaxDashboardTools> colors{
        wxColour(255, 255, 255),
        wxColour(57, 145, 212),
        wxColour(217, 101, 43),
        wxColour(164, 207, 42)
    };

    for (int i = 0; i < MaxDashboardTools; ++i) {
        state.tools[i].index = i;
        state.tools[i].label = wxString::Format("T%d", i + 1);
        state.tools[i].material = wxString::FromUTF8("N/A");
        state.tools[i].color = colors[i];

        state.filament.tools[i] = state.tools[i];
        state.filament.model_colors[i] = colors[i];
        state.filament.model_materials[i] = wxString::FromUTF8("N/A");
        state.filament.model_slot_to_tool[i] = i;
    }
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
