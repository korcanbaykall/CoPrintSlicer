#include "FilamentPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/PopupWindow.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <utility>

#include <wx/graphics.h>
#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/image.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

constexpr int k_model_box_min_width = 280;
constexpr int k_tool_box_width = 180;
constexpr int k_arrow_slot_width = 72;
constexpr int k_row_height = 44;

class LeftRoundedColorBlock : public wxWindow
{
public:
    LeftRoundedColorBlock(wxWindow* parent, const wxColour& fill, int radius)
        : wxWindow(parent, wxID_ANY)
        , m_fill(fill)
        , m_radius(radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(parent ? parent->GetBackgroundColour() : *wxBLACK);
        Bind(wxEVT_PAINT, &LeftRoundedColorBlock::on_paint, this);
    }

    void set_fill(const wxColour& fill)
    {
        if (m_fill == fill)
            return;
        m_fill = fill;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxRect rect = GetClientRect();
        if (rect.width <= 0 || rect.height <= 0)
            return;

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        static constexpr double kPi = 3.14159265358979323846;
        const double x = rect.x;
        const double y = rect.y;
        const double w = rect.width;
        const double h = rect.height;
        const double r = std::min<double>(m_radius, std::min(w * 0.5, h * 0.5));

        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->SetBrush(wxBrush(m_fill));
        gc->SetPen(*wxTRANSPARENT_PEN);

        wxGraphicsPath path = gc->CreatePath();
        path.MoveToPoint(x + r, y);
        path.AddLineToPoint(x + w, y);
        path.AddLineToPoint(x + w, y + h);
        path.AddLineToPoint(x + r, y + h);
        path.AddArc(x + r, y + h - r, r, kPi / 2.0, kPi, false);
        path.AddLineToPoint(x, y + r);
        path.AddArc(x + r, y + r, r, kPi, 1.5 * kPi, false);
        path.CloseSubpath();
        gc->FillPath(path);
    }

    wxColour m_fill;
    int      m_radius;
};

void set_panel_colour(wxWindow* panel, const wxColour& colour)
{
    if (panel == nullptr || panel->GetBackgroundColour() == colour)
        return;

    if (auto* block = dynamic_cast<LeftRoundedColorBlock*>(panel)) {
        block->set_fill(colour);
        panel->SetBackgroundColour(colour);
        return;
    }

    if (auto* box = dynamic_cast<StaticBox*>(panel))
        box->SetBackgroundColorNormal(colour);

    panel->SetBackgroundColour(colour);
    panel->Refresh();
}

void style_action_button(Button* button)
{
    if (button == nullptr)
        return;
    button->SetMinSize(wxSize(-1, button->FromDIP(40)));
    button->SetCornerRadius(button->FromDIP(8));
    button->SetBorderWidth(0);
    button->SetBackgroundColorNormal(wxColour(65, 68, 75));
    button->SetTextColorNormal(wxColour(220, 220, 220));
}

wxBitmap make_white_bitmap_from_png(wxWindow* parent, const char* relative_path, const char* fallback_name, int px)
{
    const wxString path = Slic3r::GUI::from_u8(Slic3r::var(relative_path));
    wxImage image;
    if (!image.LoadFile(path, wxBITMAP_TYPE_PNG) || !image.IsOk())
        return create_scaled_bitmap(fallback_name, parent, px);

    const int size = parent->FromDIP(px);
    image.Rescale(size, size, wxIMAGE_QUALITY_BILINEAR);

    if (image.HasAlpha()) {
        unsigned char* alpha = image.GetAlpha();
        unsigned char* data = image.GetData();
        const int pixels = image.GetWidth() * image.GetHeight();
        for (int i = 0; i < pixels; ++i) {
            if (alpha[i] == 0)
                continue;
            data[i * 3 + 0] = 255;
            data[i * 3 + 1] = 255;
            data[i * 3 + 2] = 255;
        }
    } else {
        image.InitAlpha();
        unsigned char* alpha = image.GetAlpha();
        unsigned char* data = image.GetData();
        const int pixels = image.GetWidth() * image.GetHeight();
        for (int i = 0; i < pixels; ++i) {
            alpha[i] = 255;
            data[i * 3 + 0] = 255;
            data[i * 3 + 1] = 255;
            data[i * 3 + 2] = 255;
        }
    }

    return wxBitmap(image);
}

wxBitmap make_white_forward_icon(wxWindow* parent, int px)
{
    return make_white_bitmap_from_png(parent, "images/forwardicon.png", "filament_forward", px);
}

wxBitmap make_expand_arrow_icon(wxWindow* parent, int px)
{
    return make_white_bitmap_from_png(parent, "images/expandarrow.png", "replace_arrow_down", px);
}

class ToolSelectPopup final : public PopupWindow
{
public:
    using SelectHandler = std::function<void(int)>;

    ToolSelectPopup(wxWindow* parent, SelectHandler on_select)
        : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
        , m_on_select(std::move(on_select))
    {
#ifdef __WXMSW__
        BindUnfocusEvent();
#endif
        SetBackgroundColour(DeviceUiStyle::page_background());

        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* panel = new wxPanel(this, wxID_ANY);
        panel->SetBackgroundColour(DeviceUiStyle::page_background());
        auto* panel_sizer = new wxBoxSizer(wxVERTICAL);

        for (int i = 0; i < MaxDashboardTools; ++i) {
            auto* row = new StaticBox(panel, wxID_ANY);
            row->SetMinSize(wxSize(FromDIP(188), FromDIP(44)));
            row->SetCornerRadius(FromDIP(8));
            row->SetBorderWidth(1);
            row->SetBorderColorNormal(wxColour(70, 73, 80));
            row->SetBackgroundColorNormal(DeviceUiStyle::control_background());
            row->SetBackgroundColour(DeviceUiStyle::control_background());
            row->SetCursor(wxCursor(wxCURSOR_HAND));

            auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto* color = new LeftRoundedColorBlock(row, DeviceUiStyle::accent(), FromDIP(8));
            color->SetMinSize(wxSize(FromDIP(32), FromDIP(44)));
            color->SetBackgroundColour(DeviceUiStyle::accent());
            auto* label = new wxStaticText(row, wxID_ANY, wxString::Format("Tool %d", i + 1));
            label->SetForegroundColour(DeviceUiStyle::text_primary());
            {
                wxFont f = label->GetFont();
                f.SetWeight(wxFONTWEIGHT_BOLD);
                label->SetFont(f);
            }

            row_sizer->Add(color, 0, wxEXPAND);
            row_sizer->AddSpacer(FromDIP(14));
            row_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            row_sizer->AddStretchSpacer(1);
            row->SetSizer(row_sizer);

            const auto on_pick = [this, i](wxMouseEvent&) {
                if (m_on_select)
                    m_on_select(i);
                Dismiss();
            };
            row->Bind(wxEVT_LEFT_DOWN, on_pick);
            color->Bind(wxEVT_LEFT_DOWN, on_pick);
            label->Bind(wxEVT_LEFT_DOWN, on_pick);

            m_rows[i].row = row;
            m_rows[i].color = color;
            m_rows[i].label = label;

            panel_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, i + 1 < MaxDashboardTools ? FromDIP(6) : 0);
        }

        panel->SetSizer(panel_sizer);
        root->Add(panel, 1, wxALL, FromDIP(6));
        SetSizerAndFit(root);
    }

    void sync(const std::array<wxColour, MaxDashboardTools>& colors, int selected_index)
    {
        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (m_rows[i].color != nullptr)
                set_panel_colour(m_rows[i].color, colors[i]);
            if (m_rows[i].row != nullptr) {
                const wxColour border = i == selected_index ? DeviceUiStyle::accent() : wxColour(70, 73, 80);
                m_rows[i].row->SetBorderColorNormal(border);
                m_rows[i].row->Refresh();
            }
        }
        Layout();
        Fit();
    }

private:
    struct PopupRow {
        StaticBox* row{nullptr};
        wxWindow* color{nullptr};
        wxStaticText* label{nullptr};
    };

    std::array<PopupRow, MaxDashboardTools> m_rows;
    SelectHandler m_on_select;
};

} // namespace

FilamentPanel::FilamentPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Filament"));

    auto* content = new wxPanel(m_frame->content_parent(), wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* columns = new wxBoxSizer(wxHORIZONTAL);

    auto* rows = new wxPanel(content, wxID_ANY);
    rows->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* row_sizer = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* model_header = new wxStaticText(rows, wxID_ANY, wxString::FromUTF8("Model Colors"));
    model_header->SetForegroundColour(DeviceUiStyle::text_muted());
    auto* tools_header = new wxStaticText(rows, wxID_ANY, wxString::FromUTF8("Assigned Tools"));
    tools_header->SetForegroundColour(DeviceUiStyle::text_muted());
    auto* model_header_slot = new wxBoxSizer(wxHORIZONTAL);
    model_header_slot->Add(model_header, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    auto* arrow_header_slot = new wxBoxSizer(wxHORIZONTAL);
    auto* tools_header_slot = new wxBoxSizer(wxHORIZONTAL);
    tools_header_slot->Add(tools_header, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
    header->Add(model_header_slot, 1, wxEXPAND);
    header->Add(arrow_header_slot, 0, wxEXPAND);
    header->Add(tools_header_slot, 0, wxEXPAND);
    header->SetItemMinSize(model_header_slot, FromDIP(k_model_box_min_width), -1);
    header->SetItemMinSize(arrow_header_slot, FromDIP(k_arrow_slot_width), -1);
    header->SetItemMinSize(tools_header_slot, FromDIP(k_tool_box_width), -1);
    row_sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto* divider = new wxPanel(rows, wxID_ANY);
    divider->SetMinSize(wxSize(-1, FromDIP(1)));
    divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    divider->SetBackgroundColour(wxColour(44, 129, 255));
    row_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    for (int i = 0; i < MaxDashboardTools; ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* model_box = new StaticBox(rows, wxID_ANY);
        model_box->SetMinSize(wxSize(FromDIP(k_model_box_min_width), FromDIP(k_row_height)));
        model_box->SetBackgroundColour(DeviceUiStyle::control_background());
        model_box->SetCornerRadius(FromDIP(10));
        model_box->SetBorderWidth(0);
        model_box->SetBackgroundColorNormal(DeviceUiStyle::control_background());
        auto* model_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto* model_color_box = new LeftRoundedColorBlock(model_box, *wxWHITE, FromDIP(8));
        model_color_box->SetMinSize(wxSize(FromDIP(56), FromDIP(k_row_height)));
        model_color_box->SetBackgroundColour(*wxWHITE);
        m_rows[i].model_color = model_color_box;
        model_sizer->Add(model_color_box, 0, wxEXPAND);
        model_sizer->AddSpacer(FromDIP(14));
        m_rows[i].model_material = new wxStaticText(model_box, wxID_ANY, wxString::FromUTF8("PLA"));
        m_rows[i].model_material->SetForegroundColour(DeviceUiStyle::text_primary());
        {
            wxFont f = m_rows[i].model_material->GetFont();
            f.SetWeight(wxFONTWEIGHT_BOLD);
            m_rows[i].model_material->SetFont(f);
        }
        model_sizer->Add(m_rows[i].model_material, 0, wxALIGN_CENTER_VERTICAL);
        model_sizer->AddStretchSpacer(1);
        m_rows[i].model_weight = new wxStaticText(model_box, wxID_ANY, wxString::FromUTF8("--"));
        m_rows[i].model_weight->SetForegroundColour(DeviceUiStyle::text_muted());
        model_sizer->Add(m_rows[i].model_weight, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(34));
        model_box->SetSizer(model_sizer);
        row->Add(model_box, 1, wxEXPAND | wxRIGHT, FromDIP(0));

        auto* arrow_slot = new wxBoxSizer(wxHORIZONTAL);
        arrow_slot->AddStretchSpacer(1);
        auto* arrow = new wxStaticBitmap(rows, wxID_ANY,
            make_white_forward_icon(rows, 36));
        arrow_slot->Add(arrow, 0, wxALIGN_CENTER_VERTICAL);
        arrow_slot->AddStretchSpacer(1);
        row->Add(arrow_slot, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
        row->SetItemMinSize(arrow_slot, FromDIP(k_arrow_slot_width), FromDIP(k_row_height));

        auto* tool_box = new StaticBox(rows, wxID_ANY);
        m_rows[i].tool_button = tool_box;
        tool_box->SetMinSize(wxSize(FromDIP(k_tool_box_width), FromDIP(k_row_height)));
        tool_box->SetBackgroundColour(DeviceUiStyle::control_background());
        tool_box->SetCornerRadius(FromDIP(10));
        tool_box->SetBorderWidth(0);
        tool_box->SetBackgroundColorNormal(DeviceUiStyle::control_background());
        tool_box->SetCursor(wxCursor(wxCURSOR_HAND));
        auto* tool_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto* tool_color_box = new LeftRoundedColorBlock(tool_box, DeviceUiStyle::accent(), FromDIP(8));
        tool_color_box->SetMinSize(wxSize(FromDIP(32), FromDIP(k_row_height)));
        tool_color_box->SetBackgroundColour(DeviceUiStyle::accent());
        m_rows[i].tool_color = tool_color_box;
        tool_sizer->Add(tool_color_box, 0, wxEXPAND);
        tool_sizer->AddSpacer(FromDIP(14));
        m_rows[i].tool_label = new wxStaticText(tool_box, wxID_ANY, wxString::Format("T%d", i + 1));
        m_rows[i].tool_label->SetForegroundColour(DeviceUiStyle::text_primary());
        {
            wxFont f = m_rows[i].tool_label->GetFont();
            f.SetWeight(wxFONTWEIGHT_BOLD);
            m_rows[i].tool_label->SetFont(f);
        }
        tool_sizer->Add(m_rows[i].tool_label, 0, wxALIGN_CENTER_VERTICAL);
        tool_sizer->AddStretchSpacer(1);
        auto* swap = new wxStaticBitmap(tool_box, wxID_ANY,
            create_scaled_bitmap("assigned_tools_update", tool_box, 24));
        swap->SetCursor(wxCursor(wxCURSOR_HAND));
        tool_sizer->Add(swap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        tool_box->SetSizer(tool_sizer);

        const auto open_tool_menu = [this, i, tool_box](wxMouseEvent&) {
            wxMenu menu;
            for (int tool = 0; tool < MaxDashboardTools; ++tool)
                menu.Append(1000 + tool, wxString::Format("T%d", tool + 1));
            menu.Bind(wxEVT_MENU, [this, i](wxCommandEvent& evt) {
                DeviceCommand command;
                command.kind = DeviceCommandKind::AssignModelSlotToTool;
                command.model_slot = i;
                command.tool_index = evt.GetId() - 1000;
                dispatch(command);
            });
            tool_box->PopupMenu(&menu);
        };
        tool_box->Bind(wxEVT_LEFT_DOWN, open_tool_menu);
        m_rows[i].tool_color->Bind(wxEVT_LEFT_DOWN, open_tool_menu);
        m_rows[i].tool_label->Bind(wxEVT_LEFT_DOWN, open_tool_menu);
        swap->Bind(wxEVT_LEFT_DOWN, open_tool_menu);

        row->Add(tool_box, 0, wxEXPAND);
        row_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
    }
    rows->SetSizer(row_sizer);
    columns->Add(rows, 1, wxEXPAND | wxRIGHT, FromDIP(18));

    auto* manage = new StaticBox(content, wxID_ANY);
    manage->SetBackgroundColour(DeviceUiStyle::card_background());
    manage->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    manage->SetBorderWidth(0);
    auto* manage_sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(manage, wxID_ANY, wxString::FromUTF8("Manage Filament"));
    title->SetForegroundColour(DeviceUiStyle::text_primary());
    {
        wxFont f = title->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(f);
    }
    auto* sep = new wxPanel(manage, wxID_ANY);
    sep->SetMinSize(wxSize(-1, FromDIP(1)));
    sep->SetMaxSize(wxSize(-1, FromDIP(1)));
    sep->SetBackgroundColour(DeviceUiStyle::card_border());

    m_selected_tool_box = new StaticBox(manage, wxID_ANY);
    m_selected_tool_box->SetMinSize(wxSize(FromDIP(188), FromDIP(44)));
    m_selected_tool_box->SetCornerRadius(FromDIP(8));
    m_selected_tool_box->SetBorderWidth(1);
    m_selected_tool_box->SetBorderColorNormal(wxColour(70, 73, 80));
    m_selected_tool_box->SetBackgroundColorNormal(DeviceUiStyle::control_background());
    m_selected_tool_box->SetBackgroundColour(DeviceUiStyle::control_background());
    m_selected_tool_box->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* selected_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_selected_tool_dot = new wxPanel(m_selected_tool_box, wxID_ANY);
    m_selected_tool_dot->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    m_selected_tool_dot->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    m_selected_tool_dot->SetBackgroundColour(DeviceUiStyle::accent());
    selected_sizer->Add(m_selected_tool_dot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    selected_sizer->AddSpacer(FromDIP(8));
    m_selected_tool = new wxStaticText(m_selected_tool_box, wxID_ANY, wxString::FromUTF8("Tool 1"));
    m_selected_tool->SetForegroundColour(DeviceUiStyle::text_primary());
    {
        wxFont f = m_selected_tool->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        m_selected_tool->SetFont(f);
    }
    selected_sizer->Add(m_selected_tool, 1, wxALIGN_CENTER_VERTICAL);
    auto* arrow_down = new wxStaticBitmap(m_selected_tool_box, wxID_ANY, make_expand_arrow_icon(m_selected_tool_box, 18));
    arrow_down->SetCursor(wxCursor(wxCURSOR_HAND));
    selected_sizer->Add(arrow_down, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    m_selected_tool_box->SetSizer(selected_sizer);

    m_tool_select_popup = new ToolSelectPopup(this, [this](int tool_index) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::SelectFilamentTool;
        command.tool_index = tool_index;
        dispatch(command);
        set_selected_tool(tool_index);
    });

    const auto open_selected_menu = [this](wxMouseEvent& event) {
        event.Skip(false);
        if (m_tool_select_popup == nullptr || m_selected_tool_box == nullptr)
            return;

        std::array<wxColour, MaxDashboardTools> colors{};
        for (int tool = 0; tool < MaxDashboardTools; ++tool) {
            colors[tool] = m_rows[tool].tool_color != nullptr
                ? m_rows[tool].tool_color->GetBackgroundColour()
                : DeviceUiStyle::accent();
        }
        static_cast<ToolSelectPopup*>(m_tool_select_popup)->sync(colors, m_selected_tool_index);

        const wxPoint screen_pos = m_selected_tool_box->ClientToScreen(wxPoint(0, 0));
        m_tool_select_popup->Position(screen_pos, wxSize(0, m_selected_tool_box->GetSize().y + FromDIP(4)));
        m_tool_select_popup->Popup(m_selected_tool_box);
    };
    m_selected_tool_box->Bind(wxEVT_LEFT_UP, open_selected_menu);
    m_selected_tool_dot->Bind(wxEVT_LEFT_UP, open_selected_menu);
    m_selected_tool->Bind(wxEVT_LEFT_UP, open_selected_menu);
    arrow_down->Bind(wxEVT_LEFT_UP, open_selected_menu);

    m_load_button = new Button(manage, wxString::FromUTF8("Load"));
    style_action_button(m_load_button);
    m_load_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::LoadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    m_unload_button = new Button(manage, wxString::FromUTF8("Unload"));
    style_action_button(m_unload_button);
    m_unload_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::UnloadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    manage_sizer->Add(title, 0, wxLEFT | wxTOP, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(8));
    manage_sizer->Add(sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(12));
    manage_sizer->Add(m_selected_tool_box, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(10));
    manage_sizer->Add(m_load_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(8));
    manage_sizer->Add(m_unload_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddStretchSpacer(1);
    manage->SetSizer(manage_sizer);
    columns->Add(manage, 0, wxEXPAND);

    content->SetSizer(columns);
    m_frame->set_content(content);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void FilamentPanel::apply_state(const FilamentState& state)
{
    bool layout_changed = false;
    auto set_label_if_changed = [&layout_changed](wxStaticText* label, const wxString& text) {
        if (label == nullptr || label->GetLabelText() == text)
            return;
        label->SetLabelText(text);
        layout_changed = true;
    };

    for (int i = 0; i < MaxDashboardTools; ++i) {
        if (m_rows[i].model_color != nullptr)
            set_panel_colour(m_rows[i].model_color, state.model_colors[i]);
        set_label_if_changed(
            m_rows[i].model_material,
            state.model_materials[i].IsEmpty() ? wxString::FromUTF8("N/A") : state.model_materials[i]);
        set_label_if_changed(
            m_rows[i].model_weight,
            state.model_weights[i].IsEmpty() ? wxString::FromUTF8("--") : state.model_weights[i]);

        const int tool_index = state.model_slot_to_tool[i];
        const bool valid_tool = tool_index >= 0 && tool_index < MaxDashboardTools;
        if (m_rows[i].tool_color != nullptr && valid_tool)
            set_panel_colour(m_rows[i].tool_color, state.assigned_colors[i]);
        set_label_if_changed(
            m_rows[i].tool_label,
            valid_tool ? wxString::Format("T%d", tool_index + 1) : wxString::FromUTF8("N/A"));
    }

    set_selected_tool(state.selected_tool);
    if (m_load_button != nullptr)
        m_load_button->Enable(state.can_load_unload);
    if (m_unload_button != nullptr)
        m_unload_button->Enable(state.can_load_unload);

    if (layout_changed)
        Layout();
}

void FilamentPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

void FilamentPanel::dispatch(DeviceCommand command) const
{
    if (m_command_handler)
        m_command_handler(command);
}

void FilamentPanel::set_selected_tool(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    m_selected_tool_index = tool_index;
    if (m_selected_tool != nullptr) {
        const wxString label = wxString::Format("Tool %d", tool_index + 1);
        if (m_selected_tool->GetLabelText() != label)
            m_selected_tool->SetLabelText(label);
    }
    if (m_selected_tool_dot != nullptr && m_rows[tool_index].tool_color != nullptr)
        set_panel_colour(m_selected_tool_dot, m_rows[tool_index].tool_color->GetBackgroundColour());
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
