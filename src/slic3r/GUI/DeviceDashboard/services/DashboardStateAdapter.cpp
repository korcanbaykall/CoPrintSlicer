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
        const int extruder_count = std::max(0, extruders->GetTotalExtderCount());
        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (i >= extruder_count)
                continue;

            ToolState& tool = state.tools[i];
            const double cur = static_cast<double>(extruders->GetNozzleTempCurrent(i));
            const double tgt = static_cast<double>(extruders->GetNozzleTempTarget(i));
            if (cur > 0.0 || tgt > 0.0) {
                tool.available = true;
                tool.nozzle.available = true;
                tool.nozzle.current = cur;
                tool.nozzle.target = tgt;
                state.filament.tools[i].nozzle = tool.nozzle;
                state.filament.tools[i].available = true;
            }

            const double nozzle_fan = static_cast<double>(extruders->GetNozzleFanSpeed(i));
            if (nozzle_fan >= 0.0) {
                tool.fan.available = true;
                tool.fan.percent = std::clamp(static_cast<int>(std::round(nozzle_fan * 100.0)), 0, 100);
                state.filament.tools[i].fan = tool.fan;
            }
        }
    }

    if (auto* bed = machine->GetBed()) {
        const double cur = static_cast<double>(bed->GetBedTemp());
        const double tgt = static_cast<double>(bed->GetBedTempTarget());
        if (cur > 0.0 || tgt > 0.0) {
            state.bed.temperature.available = true;
            state.bed.temperature.current = cur;
            state.bed.temperature.target = tgt;
        }
    }

    if (auto* fan = machine->GetFan()) {
        bool any_per_tool_fan = false;
        for (int i = 0; i < MaxDashboardTools; ++i)
            any_per_tool_fan = any_per_tool_fan || state.tools[i].fan.available;

        if (!any_per_tool_fan) {
            bool fan_available = false;
            int fan_percent = 0;

            const auto air_duct = fan->GetAirDuctData();
            for (const auto& part : air_duct.parts) {
                if (part.id == static_cast<int>(AIR_FUN::FAN_COOLING_0_AIRDOOR)) {
                    fan_available = true;
                    fan_percent = std::clamp(static_cast<int>(std::round(part.state / 10.0)), 0, 100);
                    break;
                }
            }

            if (!fan_available) {
                const int raw_speed = static_cast<int>(std::round(fan->GetCoolingFanSpeed() / 25.5f));
                if (raw_speed > 0) {
                    fan_available = true;
                    fan_percent = std::clamp(raw_speed, 0, 100);
                }
            }

            if (fan_available) {
                state.tools[0].fan.available = true;
                state.tools[0].fan.percent = fan_percent;
                state.filament.tools[0].fan = state.tools[0].fan;
            }
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
        state.tools[i].label = wxString::Format("Tool %d", i + 1);
        state.tools[i].material = wxString::FromUTF8("N/A");
        state.tools[i].color = colors[i];

        state.filament.tools[i] = state.tools[i];
        state.filament.model_colors[i] = colors[i];
        state.filament.model_materials[i] = wxString::FromUTF8("N/A");
        state.filament.model_weights[i] = wxString::FromUTF8("--");
        state.filament.assigned_colors[i] = colors[i];
        state.filament.model_slot_to_tool[i] = i;
    }
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
