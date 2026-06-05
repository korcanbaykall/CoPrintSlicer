#ifndef slic3r_GUI_DeviceDashboard_DeviceDashboardState_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceDashboardState_hpp_

#include <array>
#include <string>

#include <wx/colour.h>
#include <wx/string.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

constexpr int MaxDashboardTools = 4;

enum class ConnectionStatus {
    Unknown,
    Offline,
    Connecting,
    Online,
    Printing,
    Error
};

enum class PrintCommandState {
    Unknown,
    Idle,
    Printing,
    Paused,
    Complete,
    Error
};

struct PrinterIdentity {
    std::string id;
    std::string name;
    std::string ip;
    std::string type;
    std::string firmware_version;
};

struct ConnectionState {
    ConnectionStatus status{ConnectionStatus::Unknown};
    wxString message;
    bool can_send_commands{false};
};

struct CameraState {
    bool available{false};
    wxString stream_url;
    wxString snapshot_url;
};

struct PrintJobState {
    PrintCommandState state{PrintCommandState::Unknown};
    bool has_active_job{false};
    wxString file_name;
    int progress_percent{0};
    int current_layer{0};
    int total_layers{0};
    int remaining_seconds{-1};
    int elapsed_seconds{-1};
    wxString thumbnail_url;
};

struct TemperatureReading {
    bool available{false};
    double current{0.0};
    double target{0.0};
};

struct FanState {
    bool available{false};
    int percent{0};
};

struct ToolState {
    int index{0};
    wxString label;
    wxString material;
    wxColour color;
    TemperatureReading nozzle;
    FanState fan;
    bool available{false};
    bool active{false};
};

struct BedState {
    TemperatureReading temperature;
};

struct FilamentState {
    std::array<ToolState, MaxDashboardTools> tools;
    std::array<int, MaxDashboardTools> model_slot_to_tool{{0, 1, 2, 3}};
    std::array<wxColour, MaxDashboardTools> model_colors;
    std::array<wxString, MaxDashboardTools> model_materials;
};

struct MovementState {
    int selected_tool{0};
    double selected_distance_mm{1.0};
    int print_speed_percent{100};
    bool can_move{false};
};

struct DeviceDashboardState {
    PrinterIdentity printer;
    ConnectionState connection;
    CameraState camera;
    PrintJobState print_job;
    std::array<ToolState, MaxDashboardTools> tools;
    BedState bed;
    FilamentState filament;
    MovementState movement;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceDashboardState_hpp_
