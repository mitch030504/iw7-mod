#include <std_include.hpp>
#include "vr.hpp"

#include "loader/component_loader.hpp"
#include "component/console/console.hpp"
#include "component/scheduler.hpp"
#include "component/directx.hpp"

#include <utils/flags.hpp>
#include <utils/string.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <vector>
#include <chrono>
#include <string>
#include <cstring>

#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace vr
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		enum class initialization_state
		{
			disabled,
			waiting_for_d3d,
			initializing,
			initialized,
			failed
		};

		initialization_state current_init_state = initialization_state::disabled;
		bool session_running = false;

		XrInstance instance = XR_NULL_HANDLE;
		XrSystemId system_id = XR_NULL_SYSTEM_ID;
		XrSession session = XR_NULL_HANDLE;
		XrSpace reference_space = XR_NULL_HANDLE;
		XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
		XrEnvironmentBlendMode selected_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

		PFN_xrGetD3D11GraphicsRequirementsKHR pfn_xrGetD3D11GraphicsRequirementsKHR = nullptr;

		std::vector<XrView> runtime_views;
		std::vector<XrViewConfigurationView> runtime_view_configs;

		std::string result_to_string(const XrResult result)
		{
			if (instance != XR_NULL_HANDLE)
			{
				char buffer[XR_MAX_RESULT_STRING_SIZE]{};
				if (XR_SUCCEEDED(xrResultToString(instance, result, buffer)))
				{
					return buffer;
				}
			}

			switch (result)
			{
			case XR_SUCCESS: return "XR_SUCCESS";
			case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
			case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
			case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
			case XR_SPACE_BOUNDS_UNAVAILABLE: return "XR_SPACE_BOUNDS_UNAVAILABLE";
			case XR_SESSION_NOT_FOCUSED: return "XR_SESSION_NOT_FOCUSED";
			case XR_FRAME_DISCARDED: return "XR_FRAME_DISCARDED";
			case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
			case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
			case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
			case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
			case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
			case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
			case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
			case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
			case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
			case XR_ERROR_SIZE_INSUFFICIENT: return "XR_ERROR_SIZE_INSUFFICIENT";
			case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
			case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
			case XR_ERROR_SESSION_RUNNING: return "XR_ERROR_SESSION_RUNNING";
			case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
			case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
			case XR_ERROR_SYSTEM_INVALID: return "XR_ERROR_SYSTEM_INVALID";
			case XR_ERROR_PATH_INVALID: return "XR_ERROR_PATH_INVALID";
			case XR_ERROR_PATH_COUNT_EXCEEDED: return "XR_ERROR_PATH_COUNT_EXCEEDED";
			case XR_ERROR_PATH_FORMAT_INVALID: return "XR_ERROR_PATH_FORMAT_INVALID";
			case XR_ERROR_PATH_UNSUPPORTED: return "XR_ERROR_PATH_UNSUPPORTED";
			case XR_ERROR_LAYER_INVALID: return "XR_ERROR_LAYER_INVALID";
			case XR_ERROR_LAYER_LIMIT_EXCEEDED: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
			case XR_ERROR_SWAPCHAIN_RECT_INVALID: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
			case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
			case XR_ERROR_ACTION_TYPE_MISMATCH: return "XR_ERROR_ACTION_TYPE_MISMATCH";
			case XR_ERROR_SESSION_NOT_READY: return "XR_ERROR_SESSION_NOT_READY";
			case XR_ERROR_SESSION_NOT_STOPPING: return "XR_ERROR_SESSION_NOT_STOPPING";
			case XR_ERROR_TIME_INVALID: return "XR_ERROR_TIME_INVALID";
			case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
			case XR_ERROR_FILE_ACCESS_ERROR: return "XR_ERROR_FILE_ACCESS_ERROR";
			case XR_ERROR_FILE_CONTENTS_INVALID: return "XR_ERROR_FILE_CONTENTS_INVALID";
			case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
			case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
			case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
			default: return "XR_UNKNOWN_RESULT (" + std::to_string(static_cast<int>(result)) + ")";
			}
		}

		std::string session_state_to_string(const XrSessionState state)
		{
			switch (state)
			{
			case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
			case XR_SESSION_STATE_IDLE: return "IDLE";
			case XR_SESSION_STATE_READY: return "READY";
			case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
			case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
			case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
			case XR_SESSION_STATE_STOPPING: return "STOPPING";
			case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
			case XR_SESSION_STATE_EXITING: return "EXITING";
			default: return "STATE_" + std::to_string(static_cast<int>(state));
			}
		}

		std::string blend_mode_to_string(const XrEnvironmentBlendMode mode)
		{
			switch (mode)
			{
			case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return "XR_ENVIRONMENT_BLEND_MODE_OPAQUE";
			case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return "XR_ENVIRONMENT_BLEND_MODE_ADDITIVE";
			case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return "XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND";
			default: return "XR_ENVIRONMENT_BLEND_MODE_UNKNOWN (" + std::to_string(static_cast<int>(mode)) + ")";
			}
		}

		void shutdown()
		{
			if (session_running && session != XR_NULL_HANDLE)
			{
				xrEndSession(session);
				session_running = false;
			}

			if (reference_space != XR_NULL_HANDLE)
			{
				xrDestroySpace(reference_space);
				reference_space = XR_NULL_HANDLE;
			}

			if (session != XR_NULL_HANDLE)
			{
				xrDestroySession(session);
				session = XR_NULL_HANDLE;
			}

			if (instance != XR_NULL_HANDLE)
			{
				xrDestroyInstance(instance);
				instance = XR_NULL_HANDLE;
			}

			system_id = XR_NULL_SYSTEM_ID;
			session_state = XR_SESSION_STATE_UNKNOWN;
			pfn_xrGetD3D11GraphicsRequirementsKHR = nullptr;
			runtime_views.clear();
			runtime_view_configs.clear();

			console::info("VR: OpenXR resources cleanly released.\n");
		}

		bool init_openxr()
		{
			// 1. Enumerate OpenXR instance extensions
			uint32_t extension_count = 0;
			XrResult res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrEnumerateInstanceExtensionProperties failed: %s\n", result_to_string(res).c_str());
				shutdown();
				return false;
			}

			std::vector<XrExtensionProperties> extensions(extension_count, { XR_TYPE_EXTENSION_PROPERTIES });
			res = xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data());
			if (XR_FAILED(res))
			{
				console::error("VR Error: Failed to retrieve extension properties: %s\n", result_to_string(res).c_str());
				shutdown();
				return false;
			}

			console::info("VR: Discovered %u OpenXR instance extensions:\n", extension_count);
			bool has_d3d11 = false;
			for (const auto& ext : extensions)
			{
				console::info("VR:   Extension: %s (v%u)\n", ext.extensionName, ext.extensionVersion);
				if (std::strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
				{
					has_d3d11 = true;
				}
			}

			// 2. Require XR_KHR_D3D11_ENABLE_EXTENSION_NAME
			if (!has_d3d11)
			{
				console::error("VR Error: Required extension '%s' is not supported by the OpenXR runtime.\n",
					XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
				shutdown();
				return false;
			}

			// 3. Create XrInstance with application name "IWVR"
			const char* const enabled_extensions[] = {
				XR_KHR_D3D11_ENABLE_EXTENSION_NAME
			};

			XrInstanceCreateInfo instance_ci{ XR_TYPE_INSTANCE_CREATE_INFO };
			utils::string::copy(instance_ci.applicationInfo.applicationName, "IWVR");
			instance_ci.applicationInfo.applicationVersion = 1;
			utils::string::copy(instance_ci.applicationInfo.engineName, "IW7");
			instance_ci.applicationInfo.engineVersion = 1;
			instance_ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
			instance_ci.enabledExtensionCount = 1;
			instance_ci.enabledExtensionNames = enabled_extensions;

			res = xrCreateInstance(&instance_ci, &instance);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrCreateInstance failed: %s\n", result_to_string(res).c_str());
				shutdown();
				return false;
			}

			XrInstanceProperties instance_props{ XR_TYPE_INSTANCE_PROPERTIES };
			if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &instance_props)))
			{
				console::info("VR: OpenXR Runtime: %s (%u.%u.%u)\n",
					instance_props.runtimeName,
					XR_VERSION_MAJOR(instance_props.runtimeVersion),
					XR_VERSION_MINOR(instance_props.runtimeVersion),
					XR_VERSION_PATCH(instance_props.runtimeVersion));
			}

			// 4. Get XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY
			XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
			system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			res = xrGetSystem(instance, &system_info, &system_id);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrGetSystem (HMD) failed: %s\n", result_to_string(res).c_str());
				shutdown();
				return false;
			}

			XrSystemProperties system_props{ XR_TYPE_SYSTEM_PROPERTIES };
			if (XR_SUCCEEDED(xrGetSystemProperties(instance, system_id, &system_props)))
			{
				console::info("VR: HMD System Name: '%s', Vendor ID: 0x%04X\n",
					system_props.systemName, system_props.vendorId);
			}

			// 5. Query xrGetD3D11GraphicsRequirementsKHR
			res = xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
				reinterpret_cast<PFN_xrVoidFunction*>(&pfn_xrGetD3D11GraphicsRequirementsKHR));
			if (XR_FAILED(res) || !pfn_xrGetD3D11GraphicsRequirementsKHR)
			{
				console::error("VR Error: Failed to resolve xrGetD3D11GraphicsRequirementsKHR function pointer.\n");
				shutdown();
				return false;
			}

			XrGraphicsRequirementsD3D11KHR graphics_reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
			res = pfn_xrGetD3D11GraphicsRequirementsKHR(instance, system_id, &graphics_reqs);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrGetD3D11GraphicsRequirementsKHR failed: %s\n", result_to_string(res).c_str());
				shutdown();
				return false;
			}

			// 6. Validate and log graphics requirements
			const D3D_FEATURE_LEVEL device_level = dx::device->GetFeatureLevel();
			console::info("VR: D3D11 Device Feature Level: 0x%04X, OpenXR Required Minimum Feature Level: 0x%04X\n",
				static_cast<unsigned int>(device_level),
				static_cast<unsigned int>(graphics_reqs.minFeatureLevel));

			if (device_level < graphics_reqs.minFeatureLevel)
			{
				console::error("VR Error: Active D3D11 device feature level (0x%04X) is below OpenXR minimum (0x%04X).\n",
					static_cast<unsigned int>(device_level),
					static_cast<unsigned int>(graphics_reqs.minFeatureLevel));
				shutdown();
				return false;
			}

			// 7. Inspect DXGI adapter backing dx::device and log details
			ComPtr<IDXGIDevice> dxgi_device;
			HRESULT hr = dx::device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
			if (FAILED(hr) || !dxgi_device)
			{
				console::error("VR Error: dx::device QueryInterface(IDXGIDevice) failed (hr = 0x%08X).\n", hr);
				shutdown();
				return false;
			}

			ComPtr<IDXGIAdapter> dxgi_adapter;
			hr = dxgi_device->GetAdapter(&dxgi_adapter);
			if (FAILED(hr) || !dxgi_adapter)
			{
				console::error("VR Error: IDXGIDevice::GetAdapter failed (hr = 0x%08X).\n", hr);
				shutdown();
				return false;
			}

			DXGI_ADAPTER_DESC adapter_desc{};
			hr = dxgi_adapter->GetDesc(&adapter_desc);
			if (FAILED(hr))
			{
				console::error("VR Error: IDXGIAdapter::GetDesc failed (hr = 0x%08X).\n", hr);
				shutdown();
				return false;
			}

			const std::string desc_str = utils::string::convert(adapter_desc.Description);
			console::info("VR: DXGI Adapter Description: %s\n", desc_str.c_str());
			console::info("VR: DXGI Adapter Vendor ID: 0x%04X, Device ID: 0x%04X\n",
				adapter_desc.VendorId, adapter_desc.DeviceId);
			console::info("VR: DXGI Adapter LUID: %08X:%08X\n",
				adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart);
			console::info("VR: OpenXR Required Adapter LUID: %08X:%08X\n",
				graphics_reqs.adapterLuid.HighPart, graphics_reqs.adapterLuid.LowPart);

			// Compare and log OpenXR-required adapter LUID
			const bool luid_match = (graphics_reqs.adapterLuid.LowPart == adapter_desc.AdapterLuid.LowPart &&
			                         graphics_reqs.adapterLuid.HighPart == adapter_desc.AdapterLuid.HighPart);
			if (luid_match)
			{
				console::info("VR: OpenXR and DXGI adapter LUIDs match.\n");
			}
			else
			{
				console::warn("VR Warning: OpenXR required adapter LUID does NOT match DXGI adapter LUID! Multi-GPU or hybrid graphics configuration detected.\n");
			}

			// 8. Enumerate view configuration views (PRIMARY_STEREO)
			uint32_t view_count = 0;
			res = xrEnumerateViewConfigurationViews(
				instance,
				system_id,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
				0,
				&view_count,
				nullptr);
			if (XR_FAILED(res) || view_count == 0)
			{
				console::error("VR Error: xrEnumerateViewConfigurationViews failed to get view count: %s\n",
					result_to_string(res).c_str());
				shutdown();
				return false;
			}

			if (view_count < 2)
			{
				console::error("VR Error: Runtime reported %u views for PRIMARY_STEREO; at least 2 views required.\n",
					view_count);
				shutdown();
				return false;
			}

			runtime_view_configs.assign(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
			res = xrEnumerateViewConfigurationViews(
				instance,
				system_id,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
				view_count,
				&view_count,
				runtime_view_configs.data());
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrEnumerateViewConfigurationViews failed to retrieve view configs: %s\n",
					result_to_string(res).c_str());
				shutdown();
				return false;
			}

			console::info("VR: PRIMARY_STEREO view configuration enumerated: %u views\n", view_count);
			for (uint32_t i = 0; i < view_count; ++i)
			{
				console::info("VR:   View [%u]: Recommended Resolution: %ux%u (Max: %ux%u), Recommended Samples: %u\n",
					i,
					runtime_view_configs[i].recommendedImageRectWidth,
					runtime_view_configs[i].recommendedImageRectHeight,
					runtime_view_configs[i].maxImageRectWidth,
					runtime_view_configs[i].maxImageRectHeight,
					runtime_view_configs[i].recommendedSwapchainSampleCount);
			}

			// Initialize runtime-sized view storage
			runtime_views.assign(view_count, { XR_TYPE_VIEW });
			for (auto& v : runtime_views)
			{
				v.type = XR_TYPE_VIEW;
				v.next = nullptr;
			}

			// 9. Enumerate supported environment blend modes
			uint32_t blend_mode_count = 0;
			res = xrEnumerateEnvironmentBlendModes(
				instance,
				system_id,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
				0,
				&blend_mode_count,
				nullptr);
			if (XR_FAILED(res) || blend_mode_count == 0)
			{
				console::error("VR Error: xrEnumerateEnvironmentBlendModes failed to query mode count: %s\n",
					result_to_string(res).c_str());
				shutdown();
				return false;
			}

			std::vector<XrEnvironmentBlendMode> blend_modes(blend_mode_count);
			res = xrEnumerateEnvironmentBlendModes(
				instance,
				system_id,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
				blend_mode_count,
				&blend_mode_count,
				blend_modes.data());
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrEnumerateEnvironmentBlendModes failed to retrieve modes: %s\n",
					result_to_string(res).c_str());
				shutdown();
				return false;
			}

			console::info("VR: Discovered %u supported environment blend modes:\n", blend_mode_count);
			bool opaque_supported = false;
			for (const auto mode : blend_modes)
			{
				console::info("VR:   Blend Mode: %s\n", blend_mode_to_string(mode).c_str());
				if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
				{
					opaque_supported = true;
				}
			}

			if (opaque_supported)
			{
				selected_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
				console::info("VR: Selected preferred environment blend mode: %s\n",
					blend_mode_to_string(selected_blend_mode).c_str());
			}
			else
			{
				selected_blend_mode = blend_modes[0];
				console::warn("VR Warning: XR_ENVIRONMENT_BLEND_MODE_OPAQUE unavailable; selecting first runtime-supported mode: %s\n",
					blend_mode_to_string(selected_blend_mode).c_str());
			}

			// 10. Create XrSession using XrGraphicsBindingD3D11KHR
			XrGraphicsBindingD3D11KHR graphics_binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
			graphics_binding.device = dx::device;

			XrSessionCreateInfo session_ci{ XR_TYPE_SESSION_CREATE_INFO };
			session_ci.next = &graphics_binding;
			session_ci.systemId = system_id;

			res = xrCreateSession(instance, &session_ci, &session);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrCreateSession failed: %s\n", result_to_string(res).c_str());
				shutdown();
				return false;
			}
			console::info("VR: OpenXR session successfully created.\n");

			// 11. Create XR_REFERENCE_SPACE_TYPE_LOCAL reference space
			XrReferenceSpaceCreateInfo space_ci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
			space_ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			space_ci.poseInReferenceSpace.orientation.w = 1.0f;
			space_ci.poseInReferenceSpace.position = { 0.0f, 0.0f, 0.0f };

			res = xrCreateReferenceSpace(session, &space_ci, &reference_space);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrCreateReferenceSpace (LOCAL) failed: %s\n", result_to_string(res).c_str());
				shutdown();
				return false;
			}
			console::info("VR: LOCAL reference space successfully created.\n");

			return true;
		}

		void handle_session_state_changed(const XrSessionState new_state)
		{
			console::info("VR: Session state changed: %s -> %s\n",
				session_state_to_string(session_state).c_str(),
				session_state_to_string(new_state).c_str());

			session_state = new_state;

			switch (new_state)
			{
			case XR_SESSION_STATE_READY:
			{
				if (!session_running)
				{
					XrSessionBeginInfo begin_info{ XR_TYPE_SESSION_BEGIN_INFO };
					begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					const XrResult res = xrBeginSession(session, &begin_info);
					if (XR_SUCCEEDED(res))
					{
						session_running = true;
						console::info("VR: xrBeginSession succeeded (PRIMARY_STEREO). Session running.\n");
					}
					else
					{
						console::error("VR Error: xrBeginSession failed: %s\n", result_to_string(res).c_str());
					}
				}
				else
				{
					console::warn("VR Warning: Session state READY received while session is already running; skipping xrBeginSession.\n");
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
			{
				if (session_running && session != XR_NULL_HANDLE)
				{
					const XrResult res = xrEndSession(session);
					session_running = false;
					console::info("VR: xrEndSession completed: %s\n", result_to_string(res).c_str());
				}
				else
				{
					console::warn("VR Warning: Session state STOPPING received while session is not running.\n");
				}
				break;
			}
			case XR_SESSION_STATE_EXITING:
			{
				session_running = false;
				console::info("VR: OpenXR runtime requested session exit.\n");
				break;
			}
			case XR_SESSION_STATE_LOSS_PENDING:
			{
				session_running = false;
				console::warn("VR Warning: OpenXR session loss pending.\n");
				break;
			}
			default:
				// Other state transitions (IDLE, SYNCHRONIZED, VISIBLE, FOCUSED) are logged above and not treated as errors
				break;
			}
		}

		void poll_events()
		{
			if (instance == XR_NULL_HANDLE)
			{
				return;
			}

			XrEventDataBuffer event_buffer{ XR_TYPE_EVENT_DATA_BUFFER };
			event_buffer.next = nullptr;

			while (true)
			{
				const XrResult res = xrPollEvent(instance, &event_buffer);
				if (res == XR_EVENT_UNAVAILABLE)
				{
					break;
				}

				if (XR_FAILED(res))
				{
					console::error("VR Error: xrPollEvent failed: %s\n", result_to_string(res).c_str());
					break;
				}

				if (event_buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					const auto* state_event = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event_buffer);
					if (state_event->session == session)
					{
						handle_session_state_changed(state_event->state);
					}
				}
				else if (event_buffer.type == XR_TYPE_EVENT_DATA_EVENTS_LOST)
				{
					const auto* lost_event = reinterpret_cast<const XrEventDataEventsLost*>(&event_buffer);
					console::warn("VR Warning: %u events lost.\n", lost_event->lostEventCount);
				}
				else if (event_buffer.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
				{
					console::warn("VR Warning: Instance loss pending.\n");
				}

				event_buffer = { XR_TYPE_EVENT_DATA_BUFFER };
				event_buffer.next = nullptr;
			}
		}

		void render_frame()
		{
			if (!session_running || session == XR_NULL_HANDLE || reference_space == XR_NULL_HANDLE)
			{
				return;
			}

			XrFrameWaitInfo wait_info{ XR_TYPE_FRAME_WAIT_INFO };
			XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
			XrResult res = xrWaitFrame(session, &wait_info, &frame_state);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrWaitFrame failed: %s\n", result_to_string(res).c_str());
				return;
			}

			XrFrameBeginInfo begin_info{ XR_TYPE_FRAME_BEGIN_INFO };
			res = xrBeginFrame(session, &begin_info);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrBeginFrame failed: %s\n", result_to_string(res).c_str());
				return;
			}

			if (frame_state.shouldRender)
			{
				XrViewLocateInfo locate_info{ XR_TYPE_VIEW_LOCATE_INFO };
				locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				locate_info.displayTime = frame_state.predictedDisplayTime;
				locate_info.space = reference_space;

				XrViewState view_state{ XR_TYPE_VIEW_STATE };
				uint32_t view_count_output = 0;

				for (auto& v : runtime_views)
				{
					v.type = XR_TYPE_VIEW;
					v.next = nullptr;
				}

				res = xrLocateViews(
					session,
					&locate_info,
					&view_state,
					static_cast<uint32_t>(runtime_views.size()),
					&view_count_output,
					runtime_views.data());

				if (XR_SUCCEEDED(res))
				{
					static auto last_pose_log = std::chrono::steady_clock::now();
					const auto now = std::chrono::steady_clock::now();
					if (now - last_pose_log >= std::chrono::seconds(1))
					{
						last_pose_log = now;
						constexpr float rad_to_deg = 57.29577951308232f;
						const char* const eye_names[2] = { "Left", "Right" };
						for (uint32_t i = 0; i < view_count_output && i < runtime_views.size() && i < 2; ++i)
						{
							const auto& p = runtime_views[i].pose.position;
							const auto& o = runtime_views[i].pose.orientation;
							const auto& f = runtime_views[i].fov;
							console::info("VR Pose [%s Eye]: Pos(%.3f, %.3f, %.3f) Ori(%.3f, %.3f, %.3f, %.3f) FOV(L:%.1f R:%.1f U:%.1f D:%.1f deg)\n",
								eye_names[i],
								p.x, p.y, p.z,
								o.x, o.y, o.z, o.w,
								f.angleLeft * rad_to_deg, f.angleRight * rad_to_deg,
								f.angleUp * rad_to_deg, f.angleDown * rad_to_deg);
						}
					}
				}
				else
				{
					console::warn("VR Warning: xrLocateViews failed: %s\n", result_to_string(res).c_str());
				}
			}

			// End frame with zero submitted layers for Milestone 1 initial tracking experiment.
			// Called unconditionally following a successful xrBeginFrame, even if shouldRender == XR_FALSE.
			XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
			end_info.displayTime = frame_state.predictedDisplayTime;
			end_info.environmentBlendMode = selected_blend_mode;
			end_info.layerCount = 0;
			end_info.layers = nullptr;

			res = xrEndFrame(session, &end_info);
			if (XR_FAILED(res))
			{
				console::error("VR Error: xrEndFrame failed: %s\n", result_to_string(res).c_str());
			}
		}

		void render_tick()
		{
			if (current_init_state == initialization_state::disabled ||
				current_init_state == initialization_state::failed)
			{
				return;
			}

			// Wait until dx::device != nullptr
			if (current_init_state == initialization_state::waiting_for_d3d)
			{
				if (!dx::device)
				{
					return;
				}

				current_init_state = initialization_state::initializing;
				console::info("VR: Direct3D11 device detected. Initializing OpenXR...\n");
				if (!init_openxr())
				{
					console::error("VR Error: OpenXR initialization failed. VR component disabled; flat-screen mode continues normally.\n");
					shutdown();
					current_init_state = initialization_state::failed;
					return;
				}

				current_init_state = initialization_state::initialized;
				console::info("VR: OpenXR bootstrap initialized successfully.\n");
			}

			if (current_init_state == initialization_state::initialized)
			{
				// 1. Poll OpenXR events
				poll_events();

				// 2. Perform frame lifecycle when session is running
				if (session_running)
				{
					render_frame();
				}
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (!utils::flags::has_flag("vr"))
			{
				return;
			}

			console::info("VR: -vr flag detected. Activating VR component.\n");
			current_init_state = initialization_state::waiting_for_d3d;

			scheduler::loop(render_tick, scheduler::pipeline::renderer);
		}

		void pre_destroy() override
		{
			if (current_init_state != initialization_state::disabled)
			{
				shutdown();
				current_init_state = initialization_state::disabled;
			}
		}
	};
}

REGISTER_COMPONENT(vr::component)
