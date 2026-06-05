#include "CameraPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"

#include <utility>

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

CameraPanel::CameraPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Camera"));

    m_viewport = new wxPanel(m_frame->content_parent(), wxID_ANY);
    m_viewport->SetBackgroundColour(*wxBLACK);
    m_viewport->SetMinSize(wxSize(-1, FromDIP(240)));
    auto* viewport_sizer = new wxBoxSizer(wxVERTICAL);

    m_empty_state = new wxStaticText(m_viewport, wxID_ANY, wxString::FromUTF8("Camera unavailable"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    m_empty_state->SetForegroundColour(DeviceUiStyle::text_muted());
    viewport_sizer->AddStretchSpacer(1);
    viewport_sizer->Add(m_empty_state, 0, wxALIGN_CENTER_HORIZONTAL);
    viewport_sizer->AddStretchSpacer(1);
    m_viewport->SetSizer(viewport_sizer);

    m_frame->set_content(m_viewport);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void CameraPanel::apply_state(const CameraState& state)
{
    if (m_empty_state != nullptr)
        m_empty_state->Show(!state.available || state.stream_url.IsEmpty());

    if (m_viewport != nullptr)
        m_viewport->Layout();
}

void CameraPanel::set_refresh_handler(RefreshHandler handler)
{
    m_refresh_handler = std::move(handler);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
