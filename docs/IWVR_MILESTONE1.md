# IWVR Milestone 1: OpenXR Bootstrap & Tracking Integration

## Overview
This milestone establishes the foundational OpenXR integration for the Call of Duty: Infinite Warfare VR Mod (IWVR). The modification builds as a single unified Windows x64 executable (`iw7-mod.exe`), executing seamlessly on both native Windows and Linux (via Proton / DXVK / wineopenxr).

## Architecture

```
                  Windows x64 executable: iw7-mod.exe
                                  |
              +-------------------+-------------------+
              |                                       |
      (Native Windows)                          (Linux / Proton)
              |                                       |
       Direct3D 11 API                        DXVK (D3D11 -> Vulkan)
              |                                       |
    Khronos OpenXR Loader                   Khronos OpenXR Loader
              |                                       |
     ActiveRuntime Registry                  Proton ActiveRuntime Registry
              |                                       |
   Native Windows OpenXR Runtime                wineopenxr.dll bridge
  (SteamVR, Oculus, WMR, etc.)                        |
                                            Native Linux OpenXR Runtime
                                            (Monado, SteamVR for Linux)
```

### Key Architectural Tenets
1. **Unified Windows x64 Executable**: No native Linux iw7-mod port. The exact same binary runs on Windows and Linux via Proton.
2. **Official OpenXR Loader**: Uses the Khronos `OpenXR-SDK` official loader built as a static library component (`openxr_loader`) via Premake. No custom ABI hacks or direct dynamic link dependencies on unofficial shims.
3. **Transparent Dispatching**:
   - On Windows, the loader inspects `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` and loads the registered runtime.
   - Under Proton, Wine maps this registry entry to `C:\windows\system32\wineopenxr.dll`, which bridges calls from the Windows loader into the host Linux OpenXR runtime.

## OpenXR SDK Submodule
- **Repository**: `https://github.com/KhronosGroup/OpenXR-SDK.git`
- **Submodule Path**: `deps/OpenXR-SDK`
- **Commit Hash**: `f2448a8797c85814aa892efc1ab8707900fbcc78`
- **Tag**: `release-1.1.63`

## Component Implementation Details

### Activation & State Machine
The VR component is encapsulated in `src/client/component/vr/` (`vr.hpp`, `vr.cpp`) and activates only when the command-line flag `-vr` is supplied. When `-vr` is not present, the component remains in `initialization_state::disabled`, leaving flat-screen gameplay completely unaffected.

Initialization follows an explicit 5-state lifecycle:
1. `disabled`: `-vr` flag absent; no scheduler loops registered.
2. `waiting_for_d3d`: Registered into `scheduler::pipeline::renderer`; waits until `dx::device != nullptr`.
3. `initializing`: Direct3D 11 device acquired; executing OpenXR setup pipeline.
4. `initialized`: OpenXR instance, session, reference space, view configs, and blend modes fully operational.
5. `failed`: Initialization failed at any stage. Cleans up all acquired resources once, logs error, and leaves flat-screen IW functional without retrying on future frames.

### Lifecycle & OpenXR Flow
1. **Device Wait**: The component hooks into the engine's renderer scheduler (`scheduler::pipeline::renderer`). On each frame tick, it checks `dx::device`. It does not perform OpenXR initialization until `dx::device != nullptr`.
2. **Extension Verification**: Queries `xrEnumerateInstanceExtensionProperties` and mandates support for `XR_KHR_D3D11_ENABLE_EXTENSION_NAME`.
3. **Instance Creation**: Creates `XrInstance` with application name `IWVR` and engine name `IW7`.
4. **HMD System & Graphics Requirements**:
   - Queries `xrGetSystem` with `XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY`.
   - Obtains `xrGetD3D11GraphicsRequirementsKHR` via `xrGetInstanceProcAddr`.
   - Validates that the active D3D11 device meets `graphics_reqs.minFeatureLevel`.
5. **DXGI Adapter Inspection**:
   - Queries `IDXGIDevice` and `IDXGIAdapter` from `dx::device`.
   - Logs adapter description, vendor ID, device ID, and adapter LUID.
   - Compares the DXGI adapter LUID against the OpenXR required `graphics_reqs.adapterLuid` to diagnose multi-GPU / hybrid configurations.
6. **View Configuration Enumeration**:
   - Calls `xrEnumerateViewConfigurationViews` for `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`.
   - Requires at least two views (`view_count >= 2`) and logs recommended resolutions and swapchain sample counts.
   - Dynamically sizes internal `runtime_views` storage, initializing every view with `type = XR_TYPE_VIEW` and `next = nullptr`.
7. **Environment Blend Mode Enumeration**:
   - Calls `xrEnumerateEnvironmentBlendModes` for `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`.
   - Logs all runtime-supported blend modes.
   - Prefers `XR_ENVIRONMENT_BLEND_MODE_OPAQUE`. If unavailable, selects the first runtime-supported mode and logs a warning.
   - Passes the selected blend mode to `XrFrameEndInfo.environmentBlendMode`.
8. **Session & Space Creation**:
   - Creates `XrSession` with `XrGraphicsBindingD3D11KHR` binding `dx::device`.
   - Creates a local reference space (`XR_REFERENCE_SPACE_TYPE_LOCAL`).
9. **Event Polling & Session States**:
   - Continuously drains `xrPollEvent` with reinitialized `XrEventDataBuffer` (`next = nullptr`).
   - Handles `XR_SESSION_STATE_READY` by calling `xrBeginSession` for `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`, strictly guarded to never call `xrBeginSession` more than once per READY transition.
   - Handles `XR_SESSION_STATE_STOPPING` (`xrEndSession`), strictly guarded to only end when a session is running.
   - Handles `XR_SESSION_STATE_EXITING` and `XR_SESSION_STATE_LOSS_PENDING`. Other transitions are logged as informational without error.
10. **Frame Synchronization & View Locating**:
    - Guards frame lifecycle strictly behind `session_running`.
    - Executes `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews`, and `xrEndFrame`.
    - `xrEndFrame` is called unconditionally after a successful `xrBeginFrame`, even if `shouldRender == XR_FALSE`.
    - Submits `layerCount = 0` (no composition layers submitted in Milestone 1).
    - Locates views for Left and Right eyes using runtime-sized storage and rate-limits console logging (~1Hz) showing 3D position, orientation quaternion, and FOV angles.
11. **Clean Destruction**:
    - Destroys `XrSpace`, `XrSession`, and `XrInstance` on mod termination (`pre_destroy`) or early failure via `shutdown()`. Safe regardless of how far initialization progressed.
