#pragma once

#include <functional>
#include <string>

namespace sa {

// Follows Windows' default playback device. COM must be initialized (MTA) on the thread that uses it.
class DefaultDeviceWatch {
public:
    DefaultDeviceWatch() = default;
    ~DefaultDeviceWatch() { stop(); }
    DefaultDeviceWatch(const DefaultDeviceWatch&) = delete;
    DefaultDeviceWatch& operator=(const DefaultDeviceWatch&) = delete;

    // `changed` is called by Windows on a thread of its own whenever a playback device appears or disappears, or another
    // one becomes the default. It must only signal another thread.
    bool start(std::function<void()> changed);
    void stop();

    // Endpoint ID of the default playback device. Empty: there is none.
    std::wstring default_id() const;

private:
    void* enumerator_ = nullptr; // IMMDeviceEnumerator*
    void* client_ = nullptr;     // IMMNotificationClient*
};

} // namespace sa
