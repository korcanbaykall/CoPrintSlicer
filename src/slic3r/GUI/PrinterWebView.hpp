#ifndef slic3r_GUI_PrinterWebView_hpp_
#define slic3r_GUI_PrinterWebView_hpp_

#include <map>
#include <vector>

#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/image.h>
#include <wx/webrequest.h>
#include <wx/webview.h>
#include "Widgets/ProgressBar.hpp"
#include "Widgets/WebView.hpp"
class wxStaticBitmap;
class wxStaticText;
class wxPopupTransientWindow;
class wxGauge;
namespace Slic3r {
class MachineObject;

namespace GUI {

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
    void set_printer_layer(int layer);
    void set_file_layer(int layer);
    void set_layer_info(int printer_layer, int file_layer);
    void set_estimated_remaining_seconds(int remaining_seconds);
    void set_active_file_name(const wxString &file_name);
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
    void rebuild_extruder_popup();
    void rebuild_fan_popup();
    void rebuild_speed_popup();
    void select_tab(PrinterWebViewTab tab);
    void update_sidebar_selection();
    wxPanel *create_placeholder_page(wxWindow *parent, const wxString &title, const wxString &description);
    wxPanel *create_update_page(wxWindow *parent);
    void set_fallback_preview_thumbnail();
    void on_thumbnail_webrequest_state(wxWebRequestEvent &evt);
    void update_preview_thumbnail(const MachineObject *obj);
    void refresh_print_controls_from_selected_machine();
    void refresh_layer_info_from_selected_machine();
    void refresh_update_page_from_selected_machine();
    void UpdateState();
    void OnClose(wxCloseEvent &evt);
    void SendAPIKey();
    void OnError(wxWebViewEvent &evt);
    void OnLoaded(wxWebViewEvent &evt);

private:
    struct SidebarItem {
        PrinterWebViewTab tab;
        wxPanel *panel{ nullptr };
        wxPanel *active_strip{ nullptr };
        wxStaticText *label{ nullptr };
        wxStaticText *chevron{ nullptr };
    };

    wxStaticText *m_active_file_name_value{ nullptr };
    wxString m_apikey;
    bool m_apikey_sent{ false };
    wxPanel *m_assistant_page{ nullptr };
    wxWebView *m_browser{ nullptr };
    wxStaticText *m_estimated_finish_label{ nullptr };
    wxStaticText *m_estimated_finish_value{ nullptr };
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
    wxStaticText *m_connected_printer_name_label{ nullptr };
    wxStaticText *m_connected_printer_logout_label{ nullptr };
    bool m_has_active_printer_connection{ false };
    std::map<wxString, wxString> m_fan_values;
    wxStaticText *m_layer_file_value{ nullptr };
    wxStaticText *m_layer_label{ nullptr };
    wxStaticText *m_layer_printer_value{ nullptr };
    wxStaticBitmap *m_pause_resume_icon{ nullptr };
    wxTimer *m_layer_refresh_timer{ nullptr };
    wxWindow *m_preview_printers_button{ nullptr };
    wxStaticBitmap *m_preview_thumbnail{ nullptr };
    wxWebView *m_camera_webview{ nullptr };
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
    wxString m_selected_extruder{ "Extruder" };
    wxString m_selected_fan{ "Fan" };
    wxString m_selected_fan_value{ "__" };
    wxString m_selected_speed{ "--" };
    PrinterWebViewTab m_selected_tab{ PrinterWebViewTab::Status };
    std::vector<SidebarItem> m_sidebar_items;
    wxStaticText *m_speed_display_label{ nullptr };
    wxPopupTransientWindow *m_speed_popup{ nullptr };
    wxWindow *m_speed_popup_button{ nullptr };
    wxPanel *m_speed_popup_panel{ nullptr };
    wxPanel *m_status_page{ nullptr };
    CloudTaskManagerPage *m_storage_page{ nullptr };
    wxImage m_thumbnail_image;
    wxWebRequest m_thumbnail_web_request;
    wxStaticText *m_update_header_title{ nullptr };
    wxStaticText *m_update_model_value{ nullptr };
    wxPanel *m_update_page{ nullptr };
    wxStaticText *m_update_percent_value{ nullptr };
    wxStaticBitmap *m_update_printer_bitmap{ nullptr };
    ProgressBar *m_print_progress_bar{ nullptr };
    wxGauge *m_update_progress_gauge{ nullptr };
    wxStaticText *m_update_release_note_link{ nullptr };
    wxStaticText *m_update_serial_value{ nullptr };
    wxStaticText *m_update_status_value{ nullptr };
    wxStaticText *m_update_version_value{ nullptr };
    double m_axis_move_step{ 1.0 };
    int m_zoomFactor{ 100 };
};

} // namespace GUI
} // namespace Slic3r

#endif
