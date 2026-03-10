#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/webview.h>
#include <wx/string.h>
#include <wx/webrequest.h>
#include <wx/image.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>


namespace Slic3r {
namespace GUI {

class MachineObject;

class PrinterWebView : public wxPanel {
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void reload();
    void update_mode();
    void set_printer_layer(int layer);
    void set_file_layer(int layer);
    void set_layer_info(int printer_layer, int file_layer);
    void set_estimated_remaining_seconds(int remaining_seconds);
    void set_active_file_name(const wxString &file_name);
    void update_preview_thumbnail(const MachineObject *obj);
    void set_fallback_preview_thumbnail();
    void on_thumbnail_webrequest_state(wxWebRequestEvent &evt);

    bool Show(bool show = true) override;

private:
    void SendAPIKey();
    void refresh_layer_info_from_selected_machine();

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;
    bool m_apikey_sent;

    wxString m_url_deferred;
    wxString m_preview_thumbnail_url;
    wxTimer *m_layer_refresh_timer { nullptr };
    wxWebRequest m_thumbnail_web_request;
    wxImage m_thumbnail_image;
    wxStaticBitmap *m_preview_thumbnail { nullptr };
    wxStaticText *m_active_file_name_value { nullptr };
    wxStaticText *m_estimated_finish_label { nullptr };
    wxStaticText *m_estimated_finish_value { nullptr };
    wxStaticText *m_layer_label { nullptr };
    wxStaticText *m_layer_printer_value { nullptr };
    wxStaticText *m_layer_file_value { nullptr };

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
