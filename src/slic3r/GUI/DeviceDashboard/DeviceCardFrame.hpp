#ifndef slic3r_GUI_DeviceDashboard_DeviceCardFrame_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceCardFrame_hpp_

#include "../Widgets/StaticBox.hpp"

#include <wx/string.h>

class wxBoxSizer;
class wxPanel;
class wxStaticText;
class wxWindow;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame : public StaticBox
{
public:
    explicit DeviceCardFrame(wxWindow* parent, const wxString& title = wxString());

    wxWindow* content_parent() const;
    void set_title(const wxString& title);
    void set_content(wxWindow* content);

private:
    wxStaticText* m_title{nullptr};
    wxPanel* m_content_parent{nullptr};
    wxBoxSizer* m_content_sizer{nullptr};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceCardFrame_hpp_
