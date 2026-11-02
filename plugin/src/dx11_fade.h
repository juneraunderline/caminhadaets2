#pragma once

namespace dx11_fade {

// Installs a DXGI Present hook for the current process. Returns false if DX11
// is unavailable or the swap-chain hook cannot be installed.
bool init(void);
void shutdown(void);

// Thread-safe transition controls. Alpha is updated by the game thread and
// consumed by the Present hook on the render thread.
void fade_out(void);
void fade_in(void);
bool is_black(void);

} // namespace dx11_fade
