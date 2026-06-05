#ifndef slic3r_GUI_PrinterWebView_hpp_
#define slic3r_GUI_PrinterWebView_hpp_

#include <map>
#include <array>
#include <vector>
#include <string>

#include <wx/panel.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/image.h>
#include <wx/webrequest.h>
#include <wx/webview.h>
#include "Widgets/WebView.hpp"
#include "DeviceDashboard/DeviceCommandService.hpp"
#include "DeviceDashboard/DeviceStateStore.hpp"
class wxStaticBitmap;
class wxStaticText;
class wxPopupTransientWindow;
class wxGauge;
class StaticBox;
class Button;
namespace Slic3r {
struct BBLocalMachine;
class MachineObject;

namespace GUI {

namespace DeviceDashboard {
class MovementPanel;
class PrintStatusPanel;
class PrinterStatusPanel;
} // namespace DeviceDashboard

class CloudTaskManagerPage;
enum class PrinterWebViewTab {
    Status,
    Storage,
    Update,
    Assistant
};

class PrinterWebView : public wxPanel
{
public:
    PrinterWebView(wxWindow *parent);
    ~PrinterWebView() override;

    void load_url(wxString &url, wxString apikey);
    bool Show(bool show) override;
    void reload();
    void update_mode();
    void toggle_printers_popup();
    void dismiss_printers_popup();
    void toggle_extruder_popup();
    void dismiss_extruder_popup();
    void toggle_fan_popup();
    void dismiss_fan_popup();
    void prompt_fan_value();
    void prompt_ip_connect();
    void refresh_fan_value_display();
    void reset_placeholder_selections();
    void toggle_speed_popup();
    void dismiss_speed_popup();
    void rebuild_printers_popup();
    void rebuild_sidebar_printer_list();
    void show_sidebar_root_view();
    void show_sidebar_printers_view();
    void show_sidebar_add_printer_view();
    void rebuild_extruder_popup();
    void rebuild_fan_popup();
    void rebuild_speed_popup();
    void toggle_filament_tool_popup();
    void dismiss_filament_tool_popup();
    void rebuild_filament_tool_popup();
    void select_tab(PrinterWebViewTab tab);
    void update_sidebar_selection();
    wxPanel *create_placeholder_page(wxWindow *parent, const wxString &title, const wxString &description);
    wxPanel *create_update_page(wxWindow *parent);
    void set_sidebar_user_avatar(const wxBitmap &avatar_bitmap);
    void begin_moonraker_lan_scan();
    void set_fallback_preview_thumbnail();
    void on_thumbnail_webrequest_state(wxWebRequestEvent &evt);
    void update_preview_thumbnail(const MachineObject *obj);
    void refresh_layer_info_from_selected_machine();
    void refresh_update_page_from_selected_machine();
    void UpdateState();
    void OnClose(wxCloseEvent &evt);
    void SendAPIKey();
    void OnError(wxWebViewEvent &evt);
    void OnLoaded(wxWebViewEvent &evt);

    /** Used by Add Printer flow (dialog + LAN discovery). Returns false on failure. */
    bool finish_add_moonraker_printer(const BBLocalMachine &machine, bool use_ssl);

    void sync_model_colors_from_plater();

private:
    wxString sidebar_display_name_for(const MachineObject *machine) const;
    void apply_filament_tool_selection(int tool_index);
    void refresh_filament_preview_from_selected_machine();
    void apply_filament_preview_fallback();
    void apply_filament_preview_rows(const std::array<wxColour, 4> &model_colors,
                                     const std::array<wxString, 4> &materials,
                                     const std::array<wxString, 4> &weights,
                                     const std::array<int, 4> &assigned_tools,
                                     const std::array<wxColour, 4> &assigned_colors);
    void set_filament_assigned_tool(int model_slot_index, int ui_tool, bool send_mapping_command);
    void send_tool_map_command(int model_slot_index, int ui_tool);
    void prompt_and_save_filament_selection_then_load();
    void save_filament_selection_to_moonraker(int ui_tool, const wxString &material, const wxString &color_hex);
    void clear_filament_selection_from_moonraker(int ui_tool);
    void refresh_moonraker_status_from_selected_machine();
    void apply_printer_status_tool_selection(int tool_index);
    void prompt_ps_target_temperature(bool is_bed, int extruder_index);
    void show_toolhead_temperature_dialog(int active_extruder_index);
    void show_filament_load_wizard();
    void show_add_printer_dialog();
    void show_printer_card_actions_menu(wxWindow *anchor, MachineObject *machine);
    bool confirm_forget_printer();
    void forget_local_printer(MachineObject *machine);
    void ensure_camera_webview_created();
    void ensure_storage_page_created();
    void handle_dashboard_command(const DeviceDashboard::DeviceCommand &command);
    struct SidebarItem {
        PrinterWebViewTab tab;
        wxPanel *panel{ nullptr };
        wxPanel *active_strip{ nullptr };
        wxStaticText *label{ nullptr };
        wxStaticText *chevron{ nullptr };
    };

    wxString m_apikey;
    bool m_apikey_sent{ false };
    wxPanel *m_assistant_page{ nullptr };
    wxWebView *m_browser{ nullptr };
    wxStaticText *m_extruder_display_label{ nullptr };
    wxPopupTransientWindow *m_extruder_popup{ nullptr };
    wxWindow *m_extruder_popup_button{ nullptr };
    wxPanel *m_extruder_popup_panel{ nullptr };
    wxStaticText *m_fan_display_label{ nullptr };
    wxPopupTransientWindow *m_fan_popup{ nullptr };
    wxWindow *m_fan_popup_button{ nullptr };
    wxPanel *m_fan_popup_panel{ nullptr };
    wxStaticText *m_fan_value_label{ nullptr };
    wxStaticText *m_bed_temp_value{ nullptr };
    wxStaticText *m_extruder_temp_value{ nullptr };
    wxPanel *m_connected_printer_panel{ nullptr };
    wxStaticText *m_connected_printer_status_label{ nullptr };
    wxStaticText *m_connected_printer_logout_label{ nullptr };
    bool m_has_active_printer_connection{ false };
    std::map<wxString, wxString> m_fan_values;
    wxTimer *m_layer_refresh_timer{ nullptr };
    wxWindow *m_preview_printers_button{ nullptr };
    wxPanel *m_sidebar_header_panel{ nullptr };
    wxStaticText *m_sidebar_header_back{ nullptr };
    wxStaticText *m_sidebar_header_title{ nullptr };
    wxStaticText *m_sidebar_header_add{ nullptr };
    wxPanel *m_sidebar_user_avatar_panel{ nullptr };
    wxBitmap m_sidebar_user_avatar_bitmap;
    wxPanel *m_auto_connect_scroll_track{ nullptr };
    wxPanel *m_auto_connect_scroll_thumb{ nullptr };
    wxScrolledWindow *m_auto_connect_list_window{ nullptr };
    std::vector<BBLocalMachine> m_discovered_moonraker_printers;
    bool m_lan_scan_in_progress{ false };
    bool m_lan_rescan_requested{ false };
    wxPanel *m_sidebar_printer_list_panel{ nullptr };
    wxBoxSizer *m_sidebar_printer_list_sizer{ nullptr };
    wxString m_sidebar_printer_list_signature;
    wxPanel *m_sidebar_add_printer_panel{ nullptr };
    wxPanel *m_sidebar_root_panel{ nullptr };
    wxBoxSizer *m_sidebar_root_sizer{ nullptr };
    int m_sidebar_add_tab_index{ 1 };
    wxStaticBitmap *m_preview_thumbnail{ nullptr };
    wxWebView *m_camera_webview{ nullptr };
    wxPanel *m_camera_webview_host{ nullptr };
    bool m_camera_webview_initialized{ false };
    wxStaticText *m_printer_name_value{ nullptr };
    wxStaticText *m_printer_model_value{ nullptr };
    wxStaticText *m_printer_serial_value{ nullptr };
    wxStaticText *m_printer_firmware_value{ nullptr };
    wxStaticBitmap *m_printer_photo_bitmap{ nullptr };
    std::string m_camera_machine_id;
    wxString m_camera_stream_url;
    wxString m_preview_thumbnail_url;
    wxPopupTransientWindow *m_printers_popup{ nullptr };
    wxPanel *m_printers_popup_panel{ nullptr };
    wxString m_selected_extruder{ "T1" };
    int m_selected_extruder_index{ 0 };
    wxString m_selected_fan{ "T1" };
    wxString m_selected_fan_value{ "__" };
    wxString m_selected_speed{ "--" };
    PrinterWebViewTab m_selected_tab{ PrinterWebViewTab::Status };
    std::vector<SidebarItem> m_sidebar_items;
    wxStaticText *m_speed_display_label{ nullptr };
    wxPopupTransientWindow *m_speed_popup{ nullptr };
    wxWindow *m_speed_popup_button{ nullptr };
    wxPanel *m_speed_popup_panel{ nullptr };
    wxPopupTransientWindow *m_filament_tool_popup{ nullptr };
    wxWindow *m_filament_tool_popup_button{ nullptr };
    wxPanel *m_filament_tool_popup_panel{ nullptr };
    wxPanel *m_storage_placeholder{ nullptr };
    StaticBox *m_filament_tool_color_dot{ nullptr };
    wxStaticText *m_filament_tool_name_lbl{ nullptr };
    wxStaticText *m_manage_filament_title{ nullptr };
    StaticBox *m_filament_tool_selector{ nullptr };
    Button *m_filament_load_btn{ nullptr };
    Button *m_filament_unload_btn{ nullptr };
    int m_selected_filament_tool{ 0 };
    std::array<wxPanel *, 4> m_filament_model_color_panels{ nullptr, nullptr, nullptr, nullptr };
    std::array<wxStaticText *, 4> m_filament_material_labels{ nullptr, nullptr, nullptr, nullptr };
    std::array<wxStaticText *, 4> m_filament_weight_labels{ nullptr, nullptr, nullptr, nullptr };
    std::array<wxPanel *, 4> m_filament_assigned_color_panels{ nullptr, nullptr, nullptr, nullptr };
    std::array<wxStaticText *, 4> m_filament_assigned_tool_labels{ nullptr, nullptr, nullptr, nullptr };
    std::array<int, 4> m_filament_assigned_tool_mapping{ 1, 2, 3, 4 };
    std::array<wxColour, 4> m_filament_loaded_tool_colors;
    std::array<wxString, 4> m_filament_loaded_tool_materials;
    // Colors synced from the Plater at upload time — used as fallback when
    // no printer metadata is available (e.g. printer is idle after upload).
    std::array<wxColour, 4> m_plater_synced_colors;
    std::array<wxString, 4> m_plater_synced_materials;
    bool m_has_plater_synced_colors{ false };
    std::array<double, 4> m_moonraker_nozzle_current{ 0.0, 0.0, 0.0, 0.0 };
    std::array<double, 4> m_moonraker_nozzle_target{ 0.0, 0.0, 0.0, 0.0 };
    double m_moonraker_bed_current{ 0.0 };
    double m_moonraker_bed_target{ 0.0 };
    int m_moonraker_fan_percent{ 0 };
    bool m_has_moonraker_status{ false };
    bool m_moonraker_status_fetch_in_progress{ false };
    std::string m_moonraker_status_machine_id;
    wxString m_filament_preview_fetch_key;
    bool m_filament_preview_fetch_in_progress{ false };
    wxPanel *m_status_page{ nullptr };
    CloudTaskManagerPage *m_storage_page{ nullptr };
    wxImage m_thumbnail_image;
    wxWebRequest m_thumbnail_web_request;
    wxStaticText *m_update_header_title{ nullptr };
    wxStaticText *m_update_model_value{ nullptr };
    wxPanel *m_update_page{ nullptr };
    wxStaticText *m_update_percent_value{ nullptr };
    wxStaticBitmap *m_update_printer_bitmap{ nullptr };
    wxGauge *m_update_progress_gauge{ nullptr };
    wxStaticText *m_update_release_note_link{ nullptr };
    wxStaticText *m_update_serial_value{ nullptr };
    wxStaticText *m_update_status_value{ nullptr };
    wxStaticText *m_update_version_value{ nullptr };
    DeviceDashboard::DeviceStateStore m_dashboard_state_store;
    DeviceDashboard::MovementPanel*          m_dashboard_movement_panel{nullptr};
    DeviceDashboard::PrintStatusPanel*        m_dashboard_print_status_panel{nullptr};
    DeviceDashboard::PrinterStatusPanel*      m_dashboard_printer_status_panel{nullptr};
    double m_axis_move_step{ 1.0 };
    int m_zoomFactor{ 100 };
};

} // namespace GUI
} // namespace Slic3r

#endif
