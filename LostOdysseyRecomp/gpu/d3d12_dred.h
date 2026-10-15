#pragma once

// D3D12 Device Removed Extended Data: after a device loss, the log gets the
// faulting GPU address with the objects that lived there and the command lists
// the GPU had not finished. Breadcrumbs cost 2-5 % GPU time, so it follows the
// Debug log setting at launch; LO_D3D12_DRED=1 or =0 overrides it.
#ifdef _WIN32
struct ID3D12Device;
#endif

namespace gpu::dred
{
#ifdef _WIN32
    // Before the D3D12 device is created; later calls have no effect on it.
    void ConfigureBeforeDeviceCreation(bool debugLog);
    bool Enabled();
    // Once per process, and only for a removed device.
    void LogRemovedDevice(ID3D12Device* device);
#else
    inline bool Enabled() { return false; }
#endif
}
