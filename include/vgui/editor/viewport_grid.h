#pragma once
/*
 * viewport_grid.h - world grid geometry and snapping policy for the viewport
 * (V04).
 *
 * The kit generates world-space line segments for a ground grid and the
 * origin axes; the host renders them in its own 3D pass.  Auto density picks
 * a cell size so one screen cell stays roughly within
 * [min_cell_pixels, max_cell_pixels] as the camera zooms.
 *
 * snap_settings is a renderer-agnostic policy object.  V04 does not move any
 * object; V05/V07 gizmos and host operations read it.  Disabling snapping or
 * a single channel returns the raw value, so behaviour falls back to free
 * movement.
 *
 * Nothing in this header depends on a renderer or on the vgui core beyond the
 * public colour type.
 */
#include <vector>

#include <vgui/vgui.h>
#include "viewport_camera.h"

namespace vgui_editor_kit {

	struct viewport_grid_line
	{
		camera_vec3 a{};
		camera_vec3 b{};
		vgui_color color = 0;	/* 0xAARRGGBB */
		float width = 1.0f;		/* world-unit line width */
	};

	class viewport_grid
	{
	public:
		void set_visible(bool visible);
		bool visible() const noexcept;

		/* Auto picks the cell size from the camera; off uses cell_size(). */
		void set_auto_density(bool enabled);
		bool auto_density() const noexcept;
		void set_cell_size(float cell);
		float cell_size() const noexcept;

		void set_min_cell_pixels(float pixels);
		float min_cell_pixels() const noexcept;
		void set_max_cell_pixels(float pixels);
		float max_cell_pixels() const noexcept;

		int half_cell_count() const noexcept;
		void set_half_cell_count(int cells);
		int major_every() const noexcept;
		void set_major_every(int cells);

		void set_plane_height(float y);
		float plane_height() const noexcept;

		void set_axes_visible(bool visible);
		bool axes_visible() const noexcept;
		void set_axis_length_cells(float cells);

		void set_colors(vgui_color minor, vgui_color major,
			vgui_color axis_x, vgui_color axis_y, vgui_color axis_z);

		float effective_cell_size(float distance, int viewport_height,
			float fov_deg) const;

		/* Generates world-space grid + axis lines around the camera target.
		 * Returns false when invisible or when the parameters are unusable. */
		bool build(const camera_state& camera, int viewport_width,
			int viewport_height, std::vector<viewport_grid_line>* out) const;

	private:
		bool visible_ = true;
		bool auto_density_ = true;
		float cell_size_ = 1.0f;
		float min_cell_pixels_ = 24.0f;
		float max_cell_pixels_ = 96.0f;
		int half_cell_count_ = 32;
		int major_every_ = 10;
		float plane_height_ = 0.0f;
		bool axes_visible_ = true;
		float axis_length_cells_ = 5.0f;
		vgui_color minor_color_ = 0x33C8D2E0u;
		vgui_color major_color_ = 0x66C8D2E0u;
		vgui_color axis_x_color_ = 0xFFE05050u;
		vgui_color axis_y_color_ = 0xFF50E070u;
		vgui_color axis_z_color_ = 0xFF5080E0u;
	};

	struct snap_settings
	{
		bool enabled = true;
		bool translate = true;
		bool rotate = true;
		bool scale = true;
		float translate_step = 0.5f;
		float rotate_step_deg = 15.0f;
		float scale_step = 0.1f;

		float snap_translation(float value) const;
		camera_vec3 snap_translation(const camera_vec3& value) const;
		float snap_rotation_degrees(float degrees) const;
		float snap_scale(float value) const;
	};

} /* namespace vgui_editor_kit */
