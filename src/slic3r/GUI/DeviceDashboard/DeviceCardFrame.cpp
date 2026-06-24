#include "DeviceCardFrame.hpp"

#include "DeviceUiStyle.hpp"

#include <wx/font.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

DeviceCardFrame::DeviceCardFrame(wxWindow* parent, const wxString& title)
    : StaticBox(parent, wxID_ANY)
{
    SetCornerRadius(FromDIP(DeviceUiStyle::card_radius()));
    SetBorderWidth(DeviceUiStyle::card_border_width());
    SetBorderColorNormal(DeviceUiStyle::card_border());
    SetBackgroundColorNormal(DeviceUiStyle::card_background());
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxVERTICAL);
    m_header_row = new wxBoxSizer(wxHORIZONTAL);

    m_title = new wxStaticText(this, wxID_ANY, title);
    m_title->SetForegroundColour(DeviceUiStyle::text_primary());
    {
        wxFont font = m_title->GetFont();
        font.SetWeight(wxFONTWEIGHT_BOLD);
        m_title->SetFont(font);
    }
    m_header_row->Add(m_title, 0, wxALIGN_CENTER_VERTICAL);
    m_header_row->AddStretchSpacer(1);
    header->Add(m_header_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto* divider = new wxPanel(this, wxID_ANY);
    divider->SetMinSize(wxSize(-1, FromDIP(1)));
    divider->SetBackgroundColour(DeviceUiStyle::card_border());
    header->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    root->Add(header, 0, wxEXPAND);

    m_content_parent = new wxPanel(this, wxID_ANY);
    m_content_parent->SetBackgroundColour(DeviceUiStyle::card_background());
    m_content_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_parent->SetSizer(m_content_sizer);
    root->Add(m_content_parent, 1, wxEXPAND | wxALL, FromDIP(12));

    SetSizer(root);
}

wxWindow* DeviceCardFrame::content_parent() const
{
    return m_content_parent;
}

void DeviceCardFrame::set_title(const wxString& title)
{
    if (m_title != nullptr)
        m_title->SetLabelText(title);
}

void DeviceCardFrame::set_content(wxWindow* content)
{
    if (content == nullptr || m_content_sizer == nullptr)
        return;

    m_content_sizer->Clear(false);
    m_content_sizer->Add(content, 1, wxEXPAND);
    m_content_parent->Layout();
}

void DeviceCardFrame::set_header_action(wxWindow* action)
{
    if (m_header_row == nullptr)
        return;

    if (m_header_action != nullptr) {
        m_header_row->Detach(m_header_action);
        m_header_action->Hide();
    }

    m_header_action = action;
    if (m_header_action != nullptr) {
        m_header_row->Add(m_header_action, 0, wxALIGN_CENTER_VERTICAL);
        m_header_action->Show();
    }

    Layout();
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
