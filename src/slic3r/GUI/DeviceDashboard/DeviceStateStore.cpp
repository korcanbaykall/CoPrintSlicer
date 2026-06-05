#include "DeviceStateStore.hpp"

#include <utility>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

void DeviceStateStore::set_state(DeviceDashboardState state)
{
    m_state = std::move(state);
    notify_listeners();
}

void DeviceStateStore::update(const std::function<void(DeviceDashboardState&)>& updater)
{
    if (updater)
        updater(m_state);
    notify_listeners();
}

DeviceStateStore::ListenerId DeviceStateStore::add_listener(Listener listener)
{
    const ListenerId id = m_next_listener_id++;
    m_listeners.emplace(id, std::move(listener));
    return id;
}

void DeviceStateStore::remove_listener(ListenerId id)
{
    m_listeners.erase(id);
}

void DeviceStateStore::notify_listeners() const
{
    for (const auto& item : m_listeners) {
        if (item.second)
            item.second(m_state);
    }
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
