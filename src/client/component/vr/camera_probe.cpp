#include <std_include.hpp>

#include "vr.hpp"
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include <utils/flags.hpp>
#include <utils/hook.hpp>

#include <atomic>
#include <cmath>
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
		std::atomic<unsigned long long> last_camera_log{ 0 };
		std::atomic<unsigned long long> last_camera_error_log{ 0 };
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
		static_assert(offsetof(game::RefdefView, axis) == 0x14);

		struct matrix3 { double v[3][3]{}; };
		struct quaternion { double x, y, z, w; };
		thread_local std::uintptr_t last_camera_cg = 0;
		thread_local quaternion baseline_hmd{};
		thread_local bool recenter_valid = false;

		bool normalized_quaternion(const float* source, quaternion& output)
		{
			if (!source) return false;
			for (int i = 0; i < 4; ++i) if (!std::isfinite(source[i])) return false;
			const double magnitude = double(source[0]) * source[0] + double(source[1]) * source[1] +
				double(source[2]) * source[2] + double(source[3]) * source[3];
			if (!std::isfinite(magnitude) || magnitude < 1.0e-8) return false;
			const double scale = 1.0 / std::sqrt(magnitude);
			output = { source[0] * scale, source[1] * scale, source[2] * scale, source[3] * scale };
			return true;
		}

		matrix3 rotation_matrix(const quaternion& q)
		{
			matrix3 r{};
			r.v[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z);
			r.v[0][1] = 2 * (q.x * q.y - q.z * q.w);
			r.v[0][2] = 2 * (q.x * q.z + q.y * q.w);
			r.v[1][0] = 2 * (q.x * q.y + q.z * q.w);
			r.v[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z);
			r.v[1][2] = 2 * (q.y * q.z - q.x * q.w);
			r.v[2][0] = 2 * (q.x * q.z - q.y * q.w);
			r.v[2][1] = 2 * (q.y * q.z + q.x * q.w);
			r.v[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
			return r;
		}

		matrix3 transpose(const matrix3& a)
		{
			matrix3 result{};
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 3; ++col) result.v[row][col] = a.v[col][row];
			return result;
		}

		matrix3 multiply(const matrix3& a, const matrix3& b)
		{
			matrix3 result{};
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 3; ++col)
					for (int k = 0; k < 3; ++k) result.v[row][col] += a.v[row][k] * b.v[k][col];
			return result;
		}

		double determinant(const matrix3& a)
		{
			return a.v[0][0] * (a.v[1][1] * a.v[2][2] - a.v[1][2] * a.v[2][1]) -
				a.v[0][1] * (a.v[1][0] * a.v[2][2] - a.v[1][2] * a.v[2][0]) +
				a.v[0][2] * (a.v[1][0] * a.v[2][1] - a.v[1][1] * a.v[2][0]);
		}

		bool valid_basis(const matrix3& a, double& det)
		{
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 3; ++col)
					if (!std::isfinite(a.v[row][col])) return false;
			for (int col = 0; col < 3; ++col)
			{
				double length_squared = 0;
				for (int row = 0; row < 3; ++row) length_squared += a.v[row][col] * a.v[row][col];
				if (std::abs(length_squared - 1) > 0.02) return false;
				for (int other = col + 1; other < 3; ++other)
				{
					double dot = 0;
					for (int row = 0; row < 3; ++row) dot += a.v[row][col] * a.v[row][other];
					if (std::abs(dot) > 0.02) return false;
				}
			}
			det = determinant(a);
			return std::isfinite(det) && std::abs(det - 1) <= 0.03;
		}

		int camera_matrix_writer_stub(void* arg1, void* arg2, unsigned int arg3, float* axis, float* origin)
		{
			const int result = utils::hook::invoke<int>(0x140145180, arg1, arg2, arg3, axis, origin);
			const auto error = [](const char* reason) {
				if (log_due(last_camera_error_log)) bootstrap_trace("IWVR camera invalid: %s", reason);
			};
			if (!result || !axis || !origin) { error("writer result or destination"); return result; }
			constexpr auto axis_offset = offsetof(game::cg_s, refdef) + offsetof(game::refdef_t, view) + offsetof(game::RefdefView, axis);
			constexpr auto origin_offset = offsetof(game::cg_s, refdef) + offsetof(game::refdef_t, view) + offsetof(game::RefdefView, org);
			const auto axis_address = reinterpret_cast<std::uintptr_t>(axis);
			const auto origin_address = reinterpret_cast<std::uintptr_t>(origin);
			if (axis_address < axis_offset || origin_address < origin_offset ||
				axis_address - axis_offset != origin_address - origin_offset)
			{ error("cg destination mismatch"); return result; }
			const auto cg_address = axis_address - axis_offset;
			tracking_snapshot tracking{};
			get_tracking_snapshot(&tracking); // Its return also requires position; Phase 2B does not.
			if (!tracking.center_orientation_valid ||
				!(tracking.view_state_flags & 0x1))
			{ error("HMD orientation unavailable"); return result; }
			quaternion current{};
			if (!normalized_quaternion(tracking.center_orientation, current))
			{ error("HMD quaternion invalid"); return result; }
			bool recentered = false;
			if (!recenter_valid || last_camera_cg != cg_address)
			{
				baseline_hmd = current;
				last_camera_cg = cg_address;
				recenter_valid = true;
				recentered = true;
				bootstrap_trace("IWVR camera recentered cg=%p generation=%llu", reinterpret_cast<void*>(cg_address),
					static_cast<unsigned long long>(tracking.generation));
			}
			const matrix3 h0 = rotation_matrix(baseline_hmd);
			const matrix3 h = rotation_matrix(current);
			const matrix3 d_xr = multiply(transpose(h0), h);
			const matrix3 m{ { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } } };
			const matrix3 d_iw = multiply(multiply(m, d_xr), transpose(m));
			matrix3 base{};
			for (int col = 0; col < 3; ++col)
				for (int row = 0; row < 3; ++row) base.v[row][col] = axis[col * 3 + row];
			const matrix3 injected = multiply(base, d_iw);
			double det = 0;
			if (!valid_basis(injected, det))
			{ error("output basis or determinant"); return result; }
			for (int col = 0; col < 3; ++col)
				for (int row = 0; row < 3; ++row) axis[col * 3 + row] = static_cast<float>(injected.v[row][col]);
			if (log_due(last_camera_log))
			{
				bootstrap_trace("IWVR ORIENTATION: cg=%p generation=%llu viewFlags=0x%llX hmd=(%.5f,%.5f,%.5f,%.5f) baseForward=(%.4f,%.4f,%.4f) baseLeft=(%.4f,%.4f,%.4f) baseUp=(%.4f,%.4f,%.4f) injectedForward=(%.4f,%.4f,%.4f) injectedLeft=(%.4f,%.4f,%.4f) injectedUp=(%.4f,%.4f,%.4f) D_iw=((%.4f,%.4f,%.4f),(%.4f,%.4f,%.4f),(%.4f,%.4f,%.4f)) determinant=%.6f recentered=%s origin=(%.4f,%.4f,%.4f)",
					reinterpret_cast<void*>(cg_address), static_cast<unsigned long long>(tracking.generation),
					static_cast<unsigned long long>(tracking.view_state_flags), current.x, current.y, current.z, current.w,
					base.v[0][0], base.v[1][0], base.v[2][0], base.v[0][1], base.v[1][1], base.v[2][1], base.v[0][2], base.v[1][2], base.v[2][2],
					injected.v[0][0], injected.v[1][0], injected.v[2][0], injected.v[0][1], injected.v[1][1], injected.v[2][1], injected.v[0][2], injected.v[1][2], injected.v[2][2],
					d_iw.v[0][0], d_iw.v[0][1], d_iw.v[0][2], d_iw.v[1][0], d_iw.v[1][1], d_iw.v[1][2], d_iw.v[2][0], d_iw.v[2][1], d_iw.v[2][2],
					det, recentered ? "yes" : "no", origin[0], origin[1], origin[2]);
			}
			return result;
		}

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
				if (utils::flags::has_flag("vrhead"))
				{
					utils::hook::call(0x14088F74D, camera_matrix_writer_stub);
					bootstrap_trace("IWVR camera injection installed callsite=0x14088F74D original=0x140145180");
				}
			}
		};
	}
}

REGISTER_COMPONENT(vr::camera_probe::component)
