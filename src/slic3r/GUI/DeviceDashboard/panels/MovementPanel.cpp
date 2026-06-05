#include "MovementPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../Widgets/Button.hpp"
#include "../../Widgets/StaticBox.hpp"
#include "../../wxExtensions.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

constexpr double DistanceOptions[] = {1.0, 5.0, 10.0, 20.0};

enum class JoystickAction {
    None,
    XMinus,
    XPlus,
    YMinus,
    YPlus
};

class AxisJoystickPanel : public wxPanel
{
public:
    using ActionHandler = std::function<void(JoystickAction)>;

    AxisJoystickPanel(wxWindow* parent, int square, int center_size, int center_gap, int button_gap)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(square, square))
        , m_square(square)
        , m_center_size(center_size)
        , m_center_pos((square - center_size) / 2)
        , m_center_gap(center_gap)
        , m_button_gap(button_gap)
    {
        SetMinSize(wxSize(square, square));
        SetMaxSize(wxSize(square, square));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(DeviceUiStyle::card_background());
        Bind(wxEVT_PAINT, &AxisJoystickPanel::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &AxisJoystickPanel::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &AxisJoystickPanel::on_left_up, this);
        Bind(wxEVT_LEAVE_WINDOW, &AxisJoystickPanel::on_mouse_leave, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &AxisJoystickPanel::on_mouse_capture_lost, this);
    }

    int center_pos() const { return m_center_pos; }
    int center_size() const { return m_center_size; }
    void set_action_handler(ActionHandler handler) { m_action_handler = std::move(handler); }

private:
    struct Piece {
        std::vector<wxPoint2DDouble> points;
        wxString label;
        wxPoint2DDouble label_center;
        JoystickAction action{JoystickAction::None};
    };

    wxPoint2DDouble p(double x, double y) const
    {
        const double scale = static_cast<double>(m_square) / 300.0;
        return {x * scale, y * scale};
    }

    std::vector<Piece> pieces() const
    {
        const double center_left = static_cast<double>(m_center_pos);
        const double center_top = static_cast<double>(m_center_pos);
        const double center_right = static_cast<double>(m_center_pos + m_center_size);
        const double center_bottom = static_cast<double>(m_center_pos + m_center_size);
        const double cgap = static_cast<double>(m_center_gap);
        const double diagonal_gap = static_cast<double>(m_button_gap) / std::sqrt(2.0);
        const double left_inner = center_left - cgap;
        const double top_inner = center_top - cgap;
        const double right_inner = center_right + cgap;
        const double bottom_inner = center_bottom + cgap;

        return {
            {{{p(54, 22), p(246, 22), p(260, 36), {right_inner - diagonal_gap, top_inner}, {left_inner + diagonal_gap, top_inner}, p(40, 36)}}, wxString::FromUTF8("Y+"), p(150, 74), JoystickAction::YPlus},
            {{{p(22, 54), p(36, 40), {left_inner, top_inner + diagonal_gap}, {left_inner, bottom_inner - diagonal_gap}, p(36, 260), p(22, 246)}}, wxString::FromUTF8("X-"), p(68, 150), JoystickAction::XMinus},
            {{{p(278, 54), p(264, 40), {right_inner, top_inner + diagonal_gap}, {right_inner, bottom_inner - diagonal_gap}, p(264, 260), p(278, 246)}}, wxString::FromUTF8("X+"), p(232, 150), JoystickAction::XPlus},
            {{{p(54, 278), p(246, 278), p(260, 264), {right_inner - diagonal_gap, bottom_inner}, {left_inner + diagonal_gap, bottom_inner}, p(40, 264)}}, wxString::FromUTF8("Y-"), p(150, 226), JoystickAction::YMinus},
        };
    }

    wxGraphicsPath rounded_path(wxGraphicsContext* gc, const std::vector<wxPoint2DDouble>& points, double radius) const
    {
        wxGraphicsPath path = gc->CreatePath();
        const int n = static_cast<int>(points.size());
        if (n == 0)
            return path;

        auto len = [](const wxPoint2DDouble& a, const wxPoint2DDouble& b) {
            const double dx = b.m_x - a.m_x;
            const double dy = b.m_y - a.m_y;
            return std::sqrt(dx * dx + dy * dy);
        };

        std::vector<wxPoint2DDouble> before(n), after(n);
        for (int i = 0; i < n; ++i) {
            const auto& prev = points[(i - 1 + n) % n];
            const auto& curr = points[i];
            const auto& next = points[(i + 1) % n];
            const double lin = std::max(1.0, len(prev, curr));
            const double lout = std::max(1.0, len(curr, next));
            const double ri = std::min(radius, lin * 0.45);
            const double ro = std::min(radius, lout * 0.45);

            before[i] = {curr.m_x - (curr.m_x - prev.m_x) / lin * ri, curr.m_y - (curr.m_y - prev.m_y) / lin * ri};
            after[i] = {curr.m_x + (next.m_x - curr.m_x) / lout * ro, curr.m_y + (next.m_y - curr.m_y) / lout * ro};
        }

        path.MoveToPoint(before[0]);
        for (int i = 0; i < n; ++i) {
            path.AddQuadCurveToPoint(points[i].m_x, points[i].m_y, after[i].m_x, after[i].m_y);
            path.AddLineToPoint(before[(i + 1) % n]);
        }
        path.CloseSubpath();
        return path;
    }

    static bool contains_point(const std::vector<wxPoint2DDouble>& poly, const wxPoint& point)
    {
        bool inside = false;
        const double x = point.x;
        const double y = point.y;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const double xi = poly[i].m_x;
            const double yi = poly[i].m_y;
            const double xj = poly[j].m_x;
            const double yj = poly[j].m_y;
            const bool intersect = ((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi);
            if (intersect)
                inside = !inside;
        }
        return inside;
    }

    JoystickAction hit_test(const wxPoint& point) const
    {
        for (const auto& piece : pieces())
            if (contains_point(piece.points, point))
                return piece.action;
        return JoystickAction::None;
    }

    void on_left_down(wxMouseEvent& event)
    {
        m_pressed_action = hit_test(event.GetPosition());
        if (m_pressed_action != JoystickAction::None) {
            if (!HasCapture())
                CaptureMouse();
            Refresh();
            return;
        }
        event.Skip();
    }

    void on_left_up(wxMouseEvent& event)
    {
        if (HasCapture())
            ReleaseMouse();

        const JoystickAction pressed = m_pressed_action;
        const JoystickAction released = hit_test(event.GetPosition());
        m_pressed_action = JoystickAction::None;
        Refresh();

        if (m_action_handler && pressed != JoystickAction::None && pressed == released)
            m_action_handler(pressed);
        else
            event.Skip();
    }

    void on_mouse_leave(wxMouseEvent& event)
    {
        if (!HasCapture() && m_pressed_action != JoystickAction::None) {
            m_pressed_action = JoystickAction::None;
            Refresh();
        }
        event.Skip();
    }

    void on_mouse_capture_lost(wxMouseCaptureLostEvent&)
    {
        m_pressed_action = JoystickAction::None;
        Refresh();
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        wxFont label_font = GetFont();
        label_font.SetPointSize(std::max(12, label_font.GetPointSize() + 4));
        label_font.SetWeight(wxFONTWEIGHT_BOLD);

        for (const auto& piece : pieces()) {
            const bool pressed = piece.action == m_pressed_action;
            gc->SetBrush(wxBrush(pressed ? wxColour(190, 190, 190) : wxColour(214, 214, 214)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawPath(rounded_path(gc.get(), piece.points, 20.0));
            gc->SetFont(label_font, wxColour(18, 25, 35));
            double text_w = 0.0;
            double text_h = 0.0;
            gc->GetTextExtent(piece.label, &text_w, &text_h);
            gc->DrawText(piece.label, piece.label_center.m_x - text_w * 0.5, piece.label_center.m_y - text_h * 0.5);
        }
    }

    int m_square{0};
    int m_center_size{0};
    int m_center_pos{0};
    int m_center_gap{0};
    int m_button_gap{0};
    JoystickAction m_pressed_action{JoystickAction::None};
    ActionHandler m_action_handler;
};

void set_button_active(Button* button, bool active, bool filled_active = false)
{
    if (button == nullptr)
        return;
    button->SetBorderColorNormal(active ? DeviceUiStyle::accent() : wxColour(55, 58, 64));
    button->SetTextColorNormal(active ? DeviceUiStyle::accent() : DeviceUiStyle::text_muted());
    button->SetBackgroundColorNormal(filled_active && active ? wxColour(61, 64, 68) : wxColour(43, 46, 52));
    button->Refresh();
}

} // namespace

MovementPanel::MovementPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Movement"));

    auto* content = new wxPanel(m_frame->content_parent(), wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::card_background());

    auto* body = new wxBoxSizer(wxVERTICAL);
    auto* headers = new wxBoxSizer(wxHORIZONTAL);
    headers->Add(make_header_label(content, wxString::FromUTF8("Tool\nSelection")), 0, wxALIGN_CENTER_VERTICAL);
    headers->AddSpacer(FromDIP(20));
    headers->Add(make_header_label(content, wxString::FromUTF8("Move\nX,Y axis")), 0, wxALIGN_CENTER_VERTICAL);
    headers->AddSpacer(FromDIP(20));
    headers->Add(make_header_label(content, wxString::FromUTF8("Move\nZ axis")), 0, wxALIGN_CENTER_VERTICAL);
    body->Add(headers, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(6));

    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    auto* tool_col = new wxBoxSizer(wxVERTICAL);
    for (int i = 0; i < MaxDashboardTools; ++i) {
        m_tool_buttons[i] = make_tool_button(content, wxString::Format("T%d", i + 1), i == 0);
        m_tool_buttons[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            DeviceCommand command;
            command.kind = DeviceCommandKind::SelectTool;
            command.tool_index = i;
            dispatch(command);
            set_active_tool_button(i);
        });
        tool_col->Add(m_tool_buttons[i], 0, i < MaxDashboardTools - 1 ? wxBOTTOM : 0, FromDIP(10));
    }
    controls->Add(tool_col, 0, wxTOP, FromDIP(18));

    const int xy_square = FromDIP(300);
    const int center_size = FromDIP(85);
    auto* xy_area = new AxisJoystickPanel(content, xy_square, center_size, FromDIP(4), FromDIP(3));
    xy_area->SetCursor(wxCursor(wxCURSOR_HAND));
    xy_area->set_action_handler([this](JoystickAction action) {
        switch (action) {
        case JoystickAction::XMinus: dispatch_axis(Axis::X, -1.0); break;
        case JoystickAction::XPlus:  dispatch_axis(Axis::X,  1.0); break;
        case JoystickAction::YMinus: dispatch_axis(Axis::Y, -1.0); break;
        case JoystickAction::YPlus:  dispatch_axis(Axis::Y,  1.0); break;
        case JoystickAction::None: break;
        }
    });

    auto* center_btn = new Button(xy_area, wxString(), "home", 0, 34);
    center_btn->SetSize(wxRect(wxPoint(xy_area->center_pos(), xy_area->center_pos()), wxSize(xy_area->center_size(), xy_area->center_size())));
    center_btn->SetMinSize(wxSize(xy_area->center_size(), xy_area->center_size()));
    center_btn->SetMaxSize(wxSize(xy_area->center_size(), xy_area->center_size()));
    center_btn->SetCornerRadius(FromDIP(18));
    center_btn->SetBorderWidth(0);
    center_btn->SetBackgroundColorNormal(*wxWHITE);
    center_btn->SetBackgroundColour(DeviceUiStyle::card_background());
    center_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    center_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::Home;
        dispatch(command);
    });

    controls->AddSpacer(FromDIP(20));
    controls->Add(xy_area, 0);

    auto* z_col = new wxBoxSizer(wxVERTICAL);

    // Z+ butonu — rectangle_10 SVG şekli üzerine +Z etiketi
    auto* z_plus_host = new wxPanel(content, wxID_ANY);
    z_plus_host->SetMinSize(wxSize(FromDIP(90), FromDIP(75)));
    z_plus_host->SetMaxSize(wxSize(FromDIP(90), FromDIP(75)));
    z_plus_host->SetBackgroundColour(DeviceUiStyle::card_background());
    z_plus_host->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* z_plus_bmp = new wxStaticBitmap(z_plus_host, wxID_ANY,
        create_scaled_bitmap("rectangle_10", z_plus_host, 75));
    auto* z_plus_lbl = new wxStaticText(z_plus_host, wxID_ANY, wxString::FromUTF8("+Z"));
    z_plus_lbl->SetForegroundColour(wxColour(45, 48, 55));
    {
        wxFont f = z_plus_lbl->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        f.SetPointSize(f.GetPointSize() + 2);
        z_plus_lbl->SetFont(f);
    }
    auto center_in_parent = [](wxWindow* parent, wxWindow* child) {
        if (!parent || !child) return;
        const wxSize ps = parent->GetClientSize();
        const wxSize cs = child->GetBestSize();
        child->Move(std::max(0, (ps.x - cs.x) / 2), std::max(0, (ps.y - cs.y) / 2));
    };
    auto layout_z_plus = [z_plus_host, z_plus_bmp, z_plus_lbl, center_in_parent]() {
        if (z_plus_bmp) {
            const wxSize hs = z_plus_host->GetClientSize();
            const wxSize bs = z_plus_bmp->GetBestSize();
            z_plus_bmp->Move(std::max(0, (hs.x - bs.x) / 2), std::max(0, (hs.y - bs.y) / 2));
        }
        center_in_parent(z_plus_host, z_plus_lbl);
    };
    z_plus_host->Bind(wxEVT_SIZE, [layout_z_plus](wxSizeEvent& e) { e.Skip(); layout_z_plus(); });
    CallAfter(layout_z_plus);
    z_plus_host->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { dispatch_axis(Axis::Z, -1.0); });
    z_plus_lbl->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { dispatch_axis(Axis::Z, -1.0); });

    // Z home butonu
    auto* z_home = new Button(content, wxString(), "home", 0, 40);
    z_home->SetMinSize(wxSize(FromDIP(80), FromDIP(75)));
    z_home->SetMaxSize(wxSize(FromDIP(80), FromDIP(75)));
    z_home->SetCornerRadius(FromDIP(15));
    z_home->SetBorderWidth(0);
    z_home->SetBackgroundColorNormal(*wxWHITE);
    z_home->SetCursor(wxCursor(wxCURSOR_HAND));
    z_home->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::Home;
        dispatch(command);
    });

    // Z- butonu — rectangle_12 SVG şekli üzerine -Z etiketi
    auto* z_minus_host = new wxPanel(content, wxID_ANY);
    z_minus_host->SetMinSize(wxSize(FromDIP(90), FromDIP(75)));
    z_minus_host->SetMaxSize(wxSize(FromDIP(90), FromDIP(75)));
    z_minus_host->SetBackgroundColour(DeviceUiStyle::card_background());
    z_minus_host->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* z_minus_bmp = new wxStaticBitmap(z_minus_host, wxID_ANY,
        create_scaled_bitmap("rectangle_12", z_minus_host, 75));
    auto* z_minus_lbl = new wxStaticText(z_minus_host, wxID_ANY, wxString::FromUTF8("-Z"));
    z_minus_lbl->SetForegroundColour(wxColour(45, 48, 55));
    {
        wxFont f = z_minus_lbl->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        f.SetPointSize(f.GetPointSize() + 2);
        z_minus_lbl->SetFont(f);
    }
    auto layout_z_minus = [z_minus_host, z_minus_bmp, z_minus_lbl, center_in_parent]() {
        if (z_minus_bmp) {
            const wxSize hs = z_minus_host->GetClientSize();
            const wxSize bs = z_minus_bmp->GetBestSize();
            z_minus_bmp->Move(std::max(0, (hs.x - bs.x) / 2), std::max(0, (hs.y - bs.y) / 2));
        }
        center_in_parent(z_minus_host, z_minus_lbl);
    };
    z_minus_host->Bind(wxEVT_SIZE, [layout_z_minus](wxSizeEvent& e) { e.Skip(); layout_z_minus(); });
    CallAfter(layout_z_minus);
    z_minus_host->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { dispatch_axis(Axis::Z, 1.0); });
    z_minus_lbl->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { dispatch_axis(Axis::Z, 1.0); });

    z_col->Add(z_plus_host, 0, wxLEFT, FromDIP(2));
    z_col->AddSpacer(FromDIP(5));
    z_col->Add(z_home, 0, wxLEFT | wxBOTTOM, FromDIP(2));
    z_col->Add(z_minus_host, 0, wxLEFT, FromDIP(2));
    controls->AddSpacer(FromDIP(20));
    controls->Add(z_col, 0, wxALIGN_TOP | wxTOP, FromDIP(22));

    auto* distance_box = new StaticBox(content, wxID_ANY);
    distance_box->SetCornerRadius(FromDIP(8));
    distance_box->SetBorderWidth(1);
    distance_box->SetBorderColorNormal(wxColour(55, 58, 64));
    distance_box->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    distance_box->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* distance_sizer = new wxBoxSizer(wxVERTICAL);
    distance_sizer->Add(make_header_label(distance_box, wxString::FromUTF8("Motion\nDistance")), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    distance_sizer->AddSpacer(FromDIP(8));
    for (int i = 0; i < 4; ++i) {
        m_distance_buttons[i] = make_option_button(distance_box, wxString::Format("%.0fmm", DistanceOptions[i]), i == 0);
        const double distance = DistanceOptions[i];
        m_distance_buttons[i]->Bind(wxEVT_BUTTON, [this, distance](wxCommandEvent&) {
            DeviceCommand command;
            command.kind = DeviceCommandKind::SetMotionDistance;
            command.value = distance;
            dispatch(command);
            set_active_distance_button(distance);
        });
        distance_sizer->Add(m_distance_buttons[i], 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
        distance_sizer->AddSpacer(i == 3 ? FromDIP(10) : FromDIP(6));
    }
    distance_box->SetSizer(distance_sizer);
    controls->AddSpacer(FromDIP(20));
    controls->Add(distance_box, 0, wxALIGN_TOP | wxTOP, FromDIP(18));

    auto* speed_box = new StaticBox(content, wxID_ANY);
    speed_box->SetCornerRadius(FromDIP(8));
    speed_box->SetBorderWidth(1);
    speed_box->SetBorderColorNormal(wxColour(55, 58, 64));
    speed_box->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    speed_box->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* speed_sizer = new wxBoxSizer(wxVERTICAL);
    speed_sizer->Add(make_header_label(speed_box, wxString::FromUTF8("Printing\nSpeed")), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    speed_sizer->AddSpacer(FromDIP(8));
    const std::array<std::pair<wxString, int>, 4> speeds{{
        {wxString::FromUTF8("Slow"), 50},
        {wxString::FromUTF8("Normal"), 100},
        {wxString::FromUTF8("Fast"), 125},
        {wxString::FromUTF8("Ultra"), 166}
    }};
    for (int i = 0; i < 4; ++i) {
        m_speed_buttons[i] = make_option_button(speed_box, speeds[i].first, i == 1);
        const int percent = speeds[i].second;
        const SpeedPreset preset = static_cast<SpeedPreset>(i);
        m_speed_buttons[i]->Bind(wxEVT_BUTTON, [this, percent, preset](wxCommandEvent&) {
            DeviceCommand command;
            command.kind = DeviceCommandKind::SetPrintSpeed;
            command.value = percent;
            dispatch(command);
            set_active_speed_button(preset);
        });
        speed_sizer->Add(m_speed_buttons[i], 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
        speed_sizer->AddSpacer(i == 3 ? FromDIP(10) : FromDIP(6));
    }
    speed_box->SetSizer(speed_sizer);
    controls->AddSpacer(FromDIP(40));
    controls->Add(speed_box, 0, wxALIGN_TOP | wxTOP, FromDIP(18));

    body->Add(controls, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(12));
    content->SetSizer(body);

    m_frame->set_content(content);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void MovementPanel::apply_state(const MovementState& state)
{
    set_active_tool_button(state.selected_tool);
    set_active_distance_button(state.selected_distance_mm);
}

void MovementPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

Button* MovementPanel::make_tool_button(wxWindow* parent, const wxString& label, bool active)
{
    auto* button = new Button(parent, label);
    const wxSize size(FromDIP(92), FromDIP(58));
    button->SetMinSize(size);
    button->SetMaxSize(size);
    button->SetSize(size);
    button->SetCornerRadius(FromDIP(10));
    button->SetBorderWidth(1);
    set_button_active(button, active, true);
    return button;
}

Button* MovementPanel::make_option_button(wxWindow* parent, const wxString& label, bool active)
{
    auto* button = new Button(parent, label);
    button->SetMinSize(wxSize(FromDIP(73), FromDIP(45)));
    button->SetCornerRadius(FromDIP(8));
    button->SetBorderWidth(1);
    set_button_active(button, active);
    return button;
}

wxStaticText* MovementPanel::make_header_label(wxWindow* parent, const wxString& label)
{
    auto* text = new wxStaticText(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    text->SetForegroundColour(DeviceUiStyle::text_muted());
    wxFont font = text->GetFont();
    if (font.GetPointSize() > 1)
        font.SetPointSize(font.GetPointSize() - 1);
    text->SetFont(font);
    return text;
}

void MovementPanel::dispatch_axis(Axis axis, double direction) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::MoveAxis;
    command.axis = axis;
    command.value = direction * m_selected_distance_mm;
    dispatch(command);
}

void MovementPanel::dispatch(DeviceCommand command) const
{
    if (m_command_handler)
        m_command_handler(command);
}

void MovementPanel::set_active_tool_button(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    if (m_selected_tool == tool_index)
        return;
    m_selected_tool = tool_index;
    for (int i = 0; i < MaxDashboardTools; ++i)
        set_button_active(m_tool_buttons[i], i == m_selected_tool, true);
}

void MovementPanel::set_active_distance_button(double distance_mm)
{
    m_selected_distance_mm = distance_mm > 0.0 ? distance_mm : 1.0;
    for (int i = 0; i < 4; ++i)
        set_button_active(m_distance_buttons[i], std::abs(DistanceOptions[i] - m_selected_distance_mm) < 0.01);
}

void MovementPanel::set_active_speed_button(SpeedPreset preset)
{
    if (m_speed_preset == preset)
        return;
    m_speed_preset = preset;
    for (int i = 0; i < 4; ++i)
        set_button_active(m_speed_buttons[i], static_cast<int>(m_speed_preset) == i);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
