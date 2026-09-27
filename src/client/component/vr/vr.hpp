#pragma once

#include <cstdint>

namespace vr
{
	struct tracked_eye
	{
		float position[3]{};
		float orientation[4]{}; // x, y, z, w in OpenXR LOCAL space
		float fov[4]{}; // left, right, up, down in radians
	};

	struct tracking_snapshot
	{
		bool valid{};
		bool center_orientation_valid{};
		std::uint64_t generation{};
		std::int64_t predicted_display_time{};
		std::uint64_t view_state_flags{};
		tracked_eye eyes[2]{};
		float center_position[3]{};
		float center_orientation[4]{};
	};

	bool get_tracking_snapshot(tracking_snapshot* out);
	void bootstrap_trace(const char* format, ...) noexcept;
}
