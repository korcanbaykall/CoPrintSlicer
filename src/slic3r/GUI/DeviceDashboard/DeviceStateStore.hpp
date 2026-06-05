#ifndef slic3r_GUI_DeviceDashboard_DeviceStateStore_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceStateStore_hpp_

#include "DeviceDashboardState.hpp"

#include <functional>
#include <map>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceStateStore
{
public:
    using ListenerId = int;
    using Listener = std::function<void(const DeviceDashboardState&)>;

    const DeviceDashboardState& state() const { return m_state; }

    void set_state(DeviceDashboardState state);
    void update(const std::function<void(DeviceDashboardState&)>& updater);

    ListenerId add_listener(Listener listener);
    void remove_listener(ListenerId id);

private:
    void notify_listeners() const;

    DeviceDashboardState m_state;
    std::map<ListenerId, Listener> m_listeners;
    ListenerId m_next_listener_id{1};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceStateStore_hpp_
