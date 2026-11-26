#include "device_watch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <new>
#include <utility>

namespace sa {
namespace {

class NotificationClient final : public IMMNotificationClient {
public:
    explicit NotificationClient(std::function<void()> changed) : changed_(std::move(changed)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) changed_();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        changed_();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~NotificationClient() = default;
    std::function<void()> changed_;
    std::atomic<ULONG> refs_{1};
};

} // namespace

bool DefaultDeviceWatch::start(std::function<void()> changed) {
    stop();
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)))) {
        return false;
    }
    enumerator_ = enumerator;
    auto* client = new (std::nothrow) NotificationClient(std::move(changed));
    if (!client) return false;
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(client))) {
        client->Release();
        return false; // default_id() still works: the caller polls
    }
    client_ = client;
    return true;
}

void DefaultDeviceWatch::stop() {
    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumerator_);
    auto* client = static_cast<NotificationClient*>(client_);
    if (enumerator && client) enumerator->UnregisterEndpointNotificationCallback(client);
    if (client) client->Release();
    if (enumerator) enumerator->Release();
    enumerator_ = client_ = nullptr;
}

std::wstring DefaultDeviceWatch::default_id() const {
    std::wstring id;
    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumerator_);
    IMMDevice* device = nullptr;
    if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) return id;
    LPWSTR text = nullptr;
    if (SUCCEEDED(device->GetId(&text)) && text) {
        id = text;
        CoTaskMemFree(text);
    }
    device->Release();
    return id;
}

} // namespace sa
