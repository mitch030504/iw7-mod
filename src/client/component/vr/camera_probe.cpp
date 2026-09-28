#include <std_include.hpp>

#include "vr.hpp"
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include <utils/flags.hpp>
#include <utils/hook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vr
{
	namespace
	{
		utils::hook::detour draw_active_frame_hook;
		utils::hook::detour get_player_view_origin_hook;
		std::atomic<unsigned long long> last_draw_log{ 0 };
		std::atomic<unsigned long long> last_origin_log{ 0 };
		std::atomic<std::uintptr_t> cached_cg[2]{};
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

		// Keep SEH confined to this leaf diagnostic copy. A stale cg pointer is
		// expected at some client transitions and must only make a sample fail.
		__declspec(noinline) bool safe_copy_memory(void* dst, const void* src, size_t size)
		{
			__try
			{
				std::memcpy(dst, src, size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		static_assert(offsetof(game::cg_s, predictedPlayerState) == 0x08);
		static_assert(offsetof(game::cg_s, refdef) == 0x4B38);
		static_assert(offsetof(game::refdef_t, view) == 0x10);
		static_assert(offsetof(game::RefdefView, org) == 0x08);

		enum class refdef_source { unavailable, global_table, cached_player_state };

		const char* source_name(refdef_source source)
		{
			switch (source)
			{
			case refdef_source::global_table: return "table";
			case refdef_source::cached_player_state: return "cached-ps";
			default: return "unavailable";
			}
		}

		bool read_table_cg(int local_client_num, game::cg_s** cg)
		{
			if (!cg || local_client_num < 0 || local_client_num >= 2) return false;
			*cg = nullptr;
			const auto* table = reinterpret_cast<game::cg_s* const*>(game::cg.get());
			return safe_copy_memory(cg, table + local_client_num, sizeof(*cg));
		}

		bool sample_refdef_from_cg(std::uintptr_t cg_address, refdef_sample* sample)
		{
			if (!sample || !cg_address) return false;
			const auto view_address = cg_address + offsetof(game::cg_s, refdef) +
				offsetof(game::refdef_t, view);
			game::RefdefView view{};
			if (!safe_copy_memory(&view, reinterpret_cast<const void*>(view_address), sizeof(view)))
			{
				return false;
			}
			std::memcpy(sample->org, view.org, sizeof(sample->org));
			std::memcpy(sample->axis, view.axis, sizeof(sample->axis));
			sample->tan_half_fov_x = view.tanHalfFovX;
			sample->tan_half_fov_y = view.tanHalfFovY;
			return true;
		}

		bool sample_refdef(int local_client_num, refdef_sample* sample, refdef_source* source)
		{
			if (source) *source = refdef_source::unavailable;
			if (!sample || local_client_num < 0 || local_client_num >= 2) return false;
			game::cg_s* table_cg = nullptr;
			if (read_table_cg(local_client_num, &table_cg) && table_cg &&
				sample_refdef_from_cg(reinterpret_cast<std::uintptr_t>(table_cg), sample))
			{
				if (source) *source = refdef_source::global_table;
				return true;
			}
			const auto cached = cached_cg[local_client_num].load(std::memory_order_acquire);
			if (sample_refdef_from_cg(cached, sample))
			{
				if (source) *source = refdef_source::cached_player_state;
				return true;
			}
			return false;
		}

		void trace_refdef(const char* stage, const refdef_sample& sample, refdef_source source)
		{
			bootstrap_trace("CG_DrawActiveFrame %s refdefSource=%s org=(%.4f,%.4f,%.4f) axis[0]=(%.4f,%.4f,%.4f) axis[1]=(%.4f,%.4f,%.4f) axis[2]=(%.4f,%.4f,%.4f) tanHalfFovX=%.5f tanHalfFovY=%.5f",
				stage, source_name(source), sample.org[0], sample.org[1], sample.org[2],
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
			refdef_source before_source = refdef_source::unavailable;
			const bool before_available = trace && sample_refdef(local_client_num, &before, &before_source);
			inside_draw = true;
			origin_calls_inside_draw = 0;
			const int result = draw_active_frame_hook.invoke<int>(local_client_num, server_time,
				demo_type, cubemap_shot, cubemap_size, render_screen, draw_type);
			inside_draw = false;
			if (trace)
			{
				refdef_sample after{};
				refdef_source after_source = refdef_source::unavailable;
				const bool after_available = sample_refdef(local_client_num, &after, &after_source);
				if (before_available) trace_refdef("BEFORE", before, before_source);
				else bootstrap_trace("CG_DrawActiveFrame BEFORE refdef unavailable refdefSource=unavailable localClientNum=%d", local_client_num);
				if (after_available) trace_refdef("AFTER", after, after_source);
				else bootstrap_trace("CG_DrawActiveFrame AFTER refdef unavailable refdefSource=unavailable localClientNum=%d", local_client_num);
				bootstrap_trace("CG_DrawActiveFrame refdef changed=%s CG_GetPlayerViewOrigin calls inside=%u localClientNum=%d",
					before_available && after_available ?
						(std::memcmp(&before, &after, sizeof(before)) ? "yes" : "no") : "unavailable",
					origin_calls_inside_draw, local_client_num);
				tracking_snapshot tracking{};
				const bool valid = get_tracking_snapshot(&tracking);
				if (after_available) bootstrap_trace("CAMERA PROBE: game origin=(%.4f,%.4f,%.4f) axis[0]=(%.4f,%.4f,%.4f) axis[1]=(%.4f,%.4f,%.4f) axis[2]=(%.4f,%.4f,%.4f) raw HMD center position=(%.4f,%.4f,%.4f) quaternion=(%.5f,%.5f,%.5f,%.5f) generation=%llu viewStateFlags=0x%llX valid=%s centerOrientationValid=%s",
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
				else bootstrap_trace("CAMERA PROBE: refdef unavailable localClientNum=%d generation=%llu viewStateFlags=0x%llX valid=%s",
					local_client_num, static_cast<unsigned long long>(tracking.generation),
					static_cast<unsigned long long>(tracking.view_state_flags), valid ? "yes" : "no");
			}
			return result;
		}

		bool get_player_view_origin_stub(int local_client_num, const game::playerState_s* ps,
			game::vec3_t* out_origin)
		{
			const bool result = get_player_view_origin_hook.invoke<bool>(local_client_num, ps, out_origin);
			if (inside_draw) ++origin_calls_inside_draw;
			std::uintptr_t cg_address = 0;
			if (ps && local_client_num >= 0 && local_client_num < 2)
			{
				cg_address = reinterpret_cast<std::uintptr_t>(ps) -
					offsetof(game::cg_s, predictedPlayerState);
				cached_cg[local_client_num].store(cg_address, std::memory_order_release);
			}
			if (log_due(last_origin_log))
			{
				game::cg_s* table_cg = nullptr;
				const bool table_read = read_table_cg(local_client_num, &table_cg);
				bootstrap_trace("CG POINTER: client=%d psDerived=%p table=%p match=%s tableRead=%s",
					local_client_num, reinterpret_cast<const void*>(cg_address), table_cg,
					table_read && cg_address && reinterpret_cast<std::uintptr_t>(table_cg) == cg_address ? "yes" : "no",
					table_read ? "success" : "failure");
				refdef_sample origin_refdef{};
				if (sample_refdef_from_cg(cg_address, &origin_refdef))
				{
					bootstrap_trace("CG_GetPlayerViewOrigin REFDEF org=(%.4f,%.4f,%.4f) axis[0]=(%.4f,%.4f,%.4f) axis[1]=(%.4f,%.4f,%.4f) axis[2]=(%.4f,%.4f,%.4f) tanHalfFovX=%.5f tanHalfFovY=%.5f",
						origin_refdef.org[0], origin_refdef.org[1], origin_refdef.org[2],
						origin_refdef.axis[0][0], origin_refdef.axis[0][1], origin_refdef.axis[0][2],
						origin_refdef.axis[1][0], origin_refdef.axis[1][1], origin_refdef.axis[1][2],
						origin_refdef.axis[2][0], origin_refdef.axis[2][1], origin_refdef.axis[2][2],
						origin_refdef.tan_half_fov_x, origin_refdef.tan_half_fov_y);
				}
				else bootstrap_trace("CG_GetPlayerViewOrigin REFDEF unavailable localClientNum=%d", local_client_num);
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
