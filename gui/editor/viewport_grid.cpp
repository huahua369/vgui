#include <vgui/editor/viewport_grid.h>

#include <algorithm>
#include <cmath>

namespace vgui_editor_kit {

	namespace {

		constexpr float kPi = 3.14159265358979323846f;

		/* Auto density ladder.  Values are world units per cell. */
		constexpr float kCellLadder[] = {
			0.1f, 0.2f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f,
			10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f
		};
		constexpr int kCellCount = (int)(sizeof(kCellLadder) / sizeof(kCellLadder[0]));
		constexpr int kDefaultLadderIndex = 4;	/* cell = 1.0 */

		float finite_or(float value, float fallback)
		{
			return std::isfinite(value) ? value : fallback;
		}

	} /* namespace */

	void viewport_grid::set_visible(bool visible)
	{
		visible_ = visible;
	}

	bool viewport_grid::visible() const noexcept
	{
		return visible_;
	}

	void viewport_grid::set_auto_density(bool enabled)
	{
		auto_density_ = enabled;
	}

	bool viewport_grid::auto_density() const noexcept
	{
		return auto_density_;
	}

	void viewport_grid::set_cell_size(float cell)
	{
		if (std::isfinite(cell) && cell > 0.0f)
			cell_size_ = cell;
	}

	float viewport_grid::cell_size() const noexcept
	{
		return cell_size_;
	}

	void viewport_grid::set_min_cell_pixels(float pixels)
	{
		const float value = finite_or(pixels, 24.0f);
		if (value <= 0.0f)
			return;
		min_cell_pixels_ = value;
		if (max_cell_pixels_ <= min_cell_pixels_)
			max_cell_pixels_ = min_cell_pixels_ * 2.0f;
	}

	float viewport_grid::min_cell_pixels() const noexcept
	{
		return min_cell_pixels_;
	}

	void viewport_grid::set_max_cell_pixels(float pixels)
	{
		const float value = finite_or(pixels, 96.0f);
		if (value <= 0.0f)
			return;
		max_cell_pixels_ = value;
		if (min_cell_pixels_ >= max_cell_pixels_)
			min_cell_pixels_ = max_cell_pixels_ * 0.5f;
	}

	float viewport_grid::max_cell_pixels() const noexcept
	{
		return max_cell_pixels_;
	}

	void viewport_grid::set_half_cell_count(int cells)
	{
		half_cell_count_ = std::clamp(cells, 1, 512);
	}

	int viewport_grid::half_cell_count() const noexcept
	{
		return half_cell_count_;
	}

	void viewport_grid::set_major_every(int cells)
	{
		major_every_ = std::max(1, cells);
	}

	int viewport_grid::major_every() const noexcept
	{
		return major_every_;
	}

	void viewport_grid::set_plane_height(float y)
	{
		plane_height_ = finite_or(y, 0.0f);
	}

	float viewport_grid::plane_height() const noexcept
	{
		return plane_height_;
	}

	void viewport_grid::set_axes_visible(bool visible)
	{
		axes_visible_ = visible;
	}

	bool viewport_grid::axes_visible() const noexcept
	{
		return axes_visible_;
	}

	void viewport_grid::set_axis_length_cells(float cells)
	{
		if (std::isfinite(cells) && cells > 0.0f)
			axis_length_cells_ = cells;
	}

	void viewport_grid::set_colors(vgui_color minor, vgui_color major,
		vgui_color axis_x, vgui_color axis_y, vgui_color axis_z)
	{
		minor_color_ = minor;
		major_color_ = major;
		axis_x_color_ = axis_x;
		axis_y_color_ = axis_y;
		axis_z_color_ = axis_z;
	}

	float viewport_grid::effective_cell_size(float distance, int viewport_height,
		float fov_deg) const
	{
		if (!auto_density_)
			return std::max(0.001f, cell_size_);
		if (!(distance > 0.0f) || viewport_height <= 0 ||
			!(fov_deg > 0.0f) || fov_deg >= 180.0f)
			return std::max(0.001f, cell_size_);

		const float half_fov = fov_deg * 0.5f * kPi / 180.0f;
		const float tangent = std::tan(half_fov);
		if (!(tangent > 1e-5f))
			return std::max(0.001f, cell_size_);

		/* Screen pixels per world unit at the target distance. */
		const float scale = (float)viewport_height / (2.0f * distance * tangent);
		if (!(scale > 0.0f))
			return std::max(0.001f, cell_size_);

		const float min_pixels = std::max(1.0f, std::min(min_cell_pixels_,
			max_cell_pixels_));
		const float max_pixels = std::max(min_pixels + 1.0f, max_cell_pixels_);

		int index = kDefaultLadderIndex;
		while (index > 0 && kCellLadder[index] * scale > max_pixels)
			--index;
		while (index < kCellCount - 1 && kCellLadder[index] * scale < min_pixels)
			++index;
		return kCellLadder[index];
	}

	bool viewport_grid::build(const camera_state& camera, int viewport_width,
		int viewport_height, std::vector<viewport_grid_line>* out) const
	{
		if (out)
			out->clear();
		if (!visible_ || !out || viewport_width <= 0 || viewport_height <= 0)
			return false;

		const float cell = effective_cell_size(camera.distance, viewport_height,
			camera.fov_deg);
		if (!(cell > 0.0f) || !std::isfinite(cell))
			return false;

		const int half = std::clamp(half_cell_count_, 1, 512);
		const int cx = (int)std::lround(camera.target.x / cell);
		const int cz = (int)std::lround(camera.target.z / cell);
		const float y = plane_height_;
		const float minor_width = std::max(0.0005f, cell * 0.01f);
		const float major_width = std::max(0.0005f, cell * 0.02f);
		const float axis_width = std::max(0.0005f, cell * 0.03f);
		const int major = std::max(1, major_every_);

		const float x0 = (float)(cx - half) * cell;
		const float x1 = (float)(cx + half) * cell;
		const float z0 = (float)(cz - half) * cell;
		const float z1 = (float)(cz + half) * cell;

		out->reserve((std::size_t)(2 * (2 * half + 1) + 3));

		for (int i = cx - half; i <= cx + half; ++i)
		{
			const bool is_major = (i % major) == 0;
			viewport_grid_line line;
			line.a = { (float)i * cell, y, z0 };
			line.b = { (float)i * cell, y, z1 };
			line.color = is_major ? major_color_ : minor_color_;
			line.width = is_major ? major_width : minor_width;
			out->push_back(line);
		}
		for (int j = cz - half; j <= cz + half; ++j)
		{
			const bool is_major = (j % major) == 0;
			viewport_grid_line line;
			line.a = { x0, y, (float)j * cell };
			line.b = { x1, y, (float)j * cell };
			line.color = is_major ? major_color_ : minor_color_;
			line.width = is_major ? major_width : minor_width;
			out->push_back(line);
		}

		if (axes_visible_)
		{
			const float length = cell * std::max(1.0f, axis_length_cells_);
			viewport_grid_line x_axis;
			x_axis.a = { -length, y, 0.0f };
			x_axis.b = { length, y, 0.0f };
			x_axis.color = axis_x_color_;
			x_axis.width = axis_width;
			out->push_back(x_axis);

			viewport_grid_line z_axis;
			z_axis.a = { 0.0f, y, -length };
			z_axis.b = { 0.0f, y, length };
			z_axis.color = axis_z_color_;
			z_axis.width = axis_width;
			out->push_back(z_axis);

			viewport_grid_line y_axis;
			y_axis.a = { 0.0f, y, 0.0f };
			y_axis.b = { 0.0f, y + length, 0.0f };
			y_axis.color = axis_y_color_;
			y_axis.width = axis_width;
			out->push_back(y_axis);
		}
		return !out->empty();
	}

	float snap_settings::snap_translation(float value) const
	{
		if (!enabled || !translate || !(translate_step > 0.0f) ||
			!std::isfinite(value))
			return value;
		return std::round(value / translate_step) * translate_step;
	}

	camera_vec3 snap_settings::snap_translation(const camera_vec3& value) const
	{
		camera_vec3 result = value;
		result.x = snap_translation(value.x);
		result.y = snap_translation(value.y);
		result.z = snap_translation(value.z);
		return result;
	}

	float snap_settings::snap_rotation_degrees(float degrees) const
	{
		if (!enabled || !rotate || !(rotate_step_deg > 0.0f) ||
			!std::isfinite(degrees))
			return degrees;
		return std::round(degrees / rotate_step_deg) * rotate_step_deg;
	}

	float snap_settings::snap_scale(float value) const
	{
		if (!enabled || !scale || !(scale_step > 0.0f) ||
			!std::isfinite(value))
			return value;
		return std::round(value / scale_step) * scale_step;
	}

} /* namespace vgui_editor_kit */
