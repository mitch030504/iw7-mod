#include <std_include.hpp>

#include "vr.hpp"
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include <utils/flags.hpp>
#include <utils/hook.hpp>

#include <atomic>
#include <cstring>

namespace vr
{
	namespace
	{
		utils::hook::detour draw_active_frame_hook;
		utils::hook::detour get_player_view_origin_hook;
		std::atomic<unsigned long long> last_draw_log{ 0 };
		std::atomic<unsigned long long> last_origin_log{ 0 };
		thread_local bool inside_draw = false;
		thread_local unsigned int origin_calls_inside_draw = 0;

		bool log_due(std::atomic<unsigned long long>& previous)
		{
			const auto now = GetTickCount64();
			auto last = previous.load(std::memory_order_relaxed);
			return now - last >= 1000 &&
				previous.compare_exchange_strong(last, now, std::memory_order_relaxed);
		}

		struct refdef_sample
		{
			float org[3];
			float axis[3][3];
			float tan_half_fov_x;
			float tan_half_fov_y;
		};

		refdef_sample sample_refdef()
		{
			refdef_sample sample{};
			const auto& view = game::cg->refdef.view;
			std::memcpy(sample.org, view.org, sizeof(sample.org));
			std::memcpy(sample.axis, view.axis, sizeof(sample.axis));
			sample.tan_half_fov_x = view.tanHalfFovX;
			sample.tan_half_fov_y = view.tanHalfFovY;
			return sample;
		}

		void trace_refdef(const char* stage, const refdef_sample& sample)
		{
			bootstrap_trace("CG_DrawActiveFrame %s org=(%.4f,%.4f,%.4f) axis[0]=(%.4f,%.4f,%.4f) axis[1]=(%.4f,%.4f,%.4f) axis[2]=(%.4f,%.4f,%.4f) tanHalfFovX=%.5f tanHalfFovY=%.5f",
				stage, sample.org[0], sample.org[1], sample.org[2],
				sample.axis[0][0], sample.axis[0][1], sample.axis[0][2],
				sample.axis[1][0], sample.axis[1][1], sample.axis[1][2],
				sample.axis[2][0], sample.axis[2][1], sample.axis[2][2],
				sample.tan_half_fov_x, sample.tan_half_fov_y);
		}

		int draw_active_frame_stub(int local_client_num, int server_time, int demo_type,
			int cubemap_shot, int cubemap_size, int render_screen, unsigned int draw_type)
		{
			const bool trace = local_client_num == 0 && log_due(last_draw_log);
			refdef_sample before{};
			if (trace) before = sample_refdef();
			inside_draw = true;
			origin_calls_inside_draw = 0;
			const int result = draw_active_frame_hook.invoke<int>(local_client_num, server_time,
				demo_type, cubemap_shot, cubemap_size, render_screen, draw_type);
			inside_draw = false;
			if (trace)
			{
				const auto after = sample_refdef();
				trace_refdef("BEFORE", before);
				trace_refdef("AFTER", after);
				bootstrap_trace("CG_DrawActiveFrame refdef changed=%s CG_GetPlayerViewOrigin calls inside=%u localClientNum=%d",
					std::memcmp(&before, &after, sizeof(before)) ? "yes" : "no",
					origin_calls_inside_draw, local_client_num);
				tracking_snapshot tracking{};
				const bool valid = get_tracking_snapshot(&tracking);
				bootstrap_trace("CAMERA PROBE: game origin=(%.4f,%.4f,%.4f) axis[0]=(%.4f,%.4f,%.4f) axis[1]=(%.4f,%.4f,%.4f) axis[2]=(%.4f,%.4f,%.4f) raw HMD center position=(%.4f,%.4f,%.4f) quaternion=(%.5f,%.5f,%.5f,%.5f) generation=%llu viewStateFlags=0x%llX valid=%s centerOrientationValid=%s",
					after.org[0], after.org[1], after.org[2],
					after.axis[0][0], after.axis[0][1], after.axis[0][2],
					after.axis[1][0], after.axis[1][1], after.axis[1][2],
					after.axis[2][0], after.axis[2][1], after.axis[2][2],
					tracking.center_position[0], tracking.center_position[1], tracking.center_position[2],
					tracking.center_orientation[0], tracking.center_orientation[1],
					tracking.center_orientation[2], tracking.center_orientation[3],
					static_cast<unsigned long long>(tracking.generation),
					static_cast<unsigned long long>(tracking.view_state_flags),
					valid ? "yes" : "no", tracking.center_orientation_valid ? "yes" : "no");
			}
			return result;
		}

		bool get_player_view_origin_stub(int local_client_num, const game::playerState_s* ps,
			game::vec3_t* out_origin)
		{
			const bool result = get_player_view_origin_hook.invoke<bool>(local_client_num, ps, out_origin);
			if (inside_draw) ++origin_calls_inside_draw;
			if (log_due(last_origin_log))
			{
				const float* origin = out_origin ? *out_origin : nullptr;
				const float* ps_origin = ps ? ps->origin : nullptr;
				const float* angles = ps ? ps->viewangles : nullptr;
				bootstrap_trace("CG_GetPlayerViewOrigin localClientNum=%d returned=%s insideDraw=%s outOrigin=(%.4f,%.4f,%.4f) psOrigin=(%.4f,%.4f,%.4f) psViewangles=(%.4f,%.4f,%.4f) ps=%p out=%p",
					local_client_num, result ? "true" : "false", inside_draw ? "yes" : "no",
					origin ? origin[0] : 0.0f, origin ? origin[1] : 0.0f, origin ? origin[2] : 0.0f,
					ps_origin ? ps_origin[0] : 0.0f, ps_origin ? ps_origin[1] : 0.0f, ps_origin ? ps_origin[2] : 0.0f,
					angles ? angles[0] : 0.0f, angles ? angles[1] : 0.0f, angles ? angles[2] : 0.0f,
					ps, out_origin);
			}
			return result;
		}
	}

	namespace camera_probe
	{
		class component final : public component_interface
		{
		public:
			void post_unpack() override
			{
				if (!utils::flags::has_flag("vr") || game::environment::is_dedi()) return;
				draw_active_frame_hook.create(0x14026CB50, draw_active_frame_stub);
				get_player_view_origin_hook.create(0x1408EC810, get_player_view_origin_stub);
				bootstrap_trace("camera probes installed");
			}
		};
	}
}

REGISTER_COMPONENT(vr::camera_probe::component)
