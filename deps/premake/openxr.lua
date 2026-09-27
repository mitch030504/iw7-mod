openxr = {
	source = path.join(dependencies.basePath, "OpenXR-SDK"),
}

function openxr.import()
	links { "openxr_loader", "advapi32" }
	openxr.includes()
	defines {
		"XR_USE_PLATFORM_WIN32",
		"XR_USE_GRAPHICS_API_D3D11"
	}
end

function openxr.includes()
	includedirs {
		path.join(openxr.source, "include")
	}
end

function openxr.project()
	project "openxr_loader"
		language "C++"
		cppdialect "C++17"
		kind "StaticLib"

		includedirs {
			path.join(openxr.source, "include"),
			path.join(openxr.source, "src"),
			path.join(openxr.source, "src/common"),
			path.join(openxr.source, "src/loader"),
			path.join(openxr.source, "src/external/jsoncpp/include")
		}

		defines {
			"XR_OS_WINDOWS",
			"XR_USE_PLATFORM_WIN32",
			"XR_USE_GRAPHICS_API_D3D11",
			"NOMINMAX",
			"DISABLE_STD_FILESYSTEM"
		}

		links {
			"advapi32"
		}

		files {
			path.join(openxr.source, "src/loader/android_utilities.cpp"),
			path.join(openxr.source, "src/loader/api_layer_interface.cpp"),
			path.join(openxr.source, "src/loader/loader_core.cpp"),
			path.join(openxr.source, "src/loader/loader_init_data.cpp"),
			path.join(openxr.source, "src/loader/loader_instance.cpp"),
			path.join(openxr.source, "src/loader/loader_logger.cpp"),
			path.join(openxr.source, "src/loader/loader_logger_recorders.cpp"),
			path.join(openxr.source, "src/loader/loader_properties.cpp"),
			path.join(openxr.source, "src/loader/manifest_file.cpp"),
			path.join(openxr.source, "src/loader/runtime_interface.cpp"),
			path.join(openxr.source, "src/loader/xr_generated_loader.cpp"),
			path.join(openxr.source, "src/xr_generated_dispatch_table_core.c"),
			path.join(openxr.source, "src/common/filesystem_utils.cpp"),
			path.join(openxr.source, "src/common/object_info.cpp"),
			path.join(openxr.source, "src/external/jsoncpp/src/lib_json/json_reader.cpp"),
			path.join(openxr.source, "src/external/jsoncpp/src/lib_json/json_value.cpp"),
			path.join(openxr.source, "src/external/jsoncpp/src/lib_json/json_writer.cpp")
		}

		warnings "Off"
end

table.insert(dependencies, openxr)
