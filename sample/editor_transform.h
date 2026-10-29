#pragma once
/*
 * editor_transform.h - transform math shared by the game-editor sample and its
 * headless tests.
 *
 * The sample stores a full TRS transform: world position, quaternion
 * orientation and per-axis scale.  The kit's gizmo reports deltas in world
 * space (translation/rotation) and per-frame-axis multipliers (scale); this
 * header applies those deltas around the gizmo pivot.  Keeping the math here
 * lets the kit test verify that gizmo results and numeric property rows agree.
 */
#include <algorithm>
#include <cmath>
#include <string>

/* Keep the test translation unit's glm configuration identical to gui/pch.h
 * so quaternion layout and helper availability match the library. */
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#ifndef GLM_FORCE_XYZW_ONLY
#define GLM_FORCE_XYZW_ONLY
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vgui/editor/vector_editor.h>
#include <vgui/editor/viewport_gizmo.h>

namespace editor_transform {

	using vgui_editor_kit::camera_vec3;
	using vgui_editor_kit::gizmo_transform;

	struct transform
	{
		camera_vec3 position{};
		glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		camera_vec3 scale{ 1.0f, 1.0f, 1.0f };
	};

	inline float clamp_scale(float value)
	{
		return value > 0.01f ? value : 0.01f;
	}

	inline float clean_float(float value)
	{
		if (!std::isfinite(value))
			return 0.0f;
		if (std::fabs(value) < 1e-6f)
			return 0.0f;
		return std::round(value * 10000.0f) / 10000.0f;
	}

	inline glm::mat4 matrix(const transform& value)
	{
		glm::quat rotation = value.rotation;
		if (glm::dot(rotation, rotation) < 1e-8f)
			rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		rotation = glm::normalize(rotation);
		return glm::translate(glm::mat4(1.0f),
			glm::vec3(value.position.x, value.position.y, value.position.z)) *
			glm::mat4_cast(rotation) *
			glm::scale(glm::mat4(1.0f),
				glm::vec3(clamp_scale(value.scale.x),
					clamp_scale(value.scale.y),
					clamp_scale(value.scale.z)));
	}

	inline glm::vec3 axis(const gizmo_transform& delta, int index)
	{
		const camera_vec3& value = delta.axis[index];
		return glm::vec3(value.x, value.y, value.z);
	}

	inline glm::vec3 to_glm(const camera_vec3& value)
	{
		return glm::vec3(value.x, value.y, value.z);
	}

	inline camera_vec3 from_glm(const glm::vec3& value)
	{
		return { value.x, value.y, value.z };
	}

	/* Numeric property rows display Euler degrees (XYZ order), matching the
	 * quaternion constructor used when the user types a value. */
	inline camera_vec3 euler_degrees(const glm::quat& rotation)
	{
		glm::quat value = rotation;
		if (glm::dot(value, value) < 1e-8f)
			value = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		value = glm::normalize(value);
		const glm::vec3 euler = glm::eulerAngles(value) *
			(180.0f / 3.14159265358979323846f);
		return { clean_float(euler.x), clean_float(euler.y),
			clean_float(euler.z) };
	}

	inline glm::quat from_euler_degrees(const camera_vec3& degrees)
	{
		constexpr float kPi = 3.14159265358979323846f;
		const glm::vec3 radians(degrees.x * kPi / 180.0f,
			degrees.y * kPi / 180.0f, degrees.z * kPi / 180.0f);
		glm::quat result(radians);
		if (glm::dot(result, result) < 1e-8f)
			return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		return glm::normalize(result);
	}

	inline std::string vec3_text(const camera_vec3& value)
	{
		/* Round through double before formatting: float TRS values such as
		 * 1.4f otherwise print as 1.39999997615814 next to the exact value a
		 * user typed into the property row. */
		auto clean = [](float component) {
			if (!std::isfinite(component))
				return 0.0;
			if (std::fabs(component) < 1e-6f)
				return 0.0;
			return std::round((double)component * 10000.0) / 10000.0;
		};
		vgui_editor_kit::vector_editor::vector_value values{};
		values[0] = clean(value.x);
		values[1] = clean(value.y);
		values[2] = clean(value.z);
		return vgui_editor_kit::vector_editor::format_value(values, 3);
	}

	inline std::string euler_text(const glm::quat& rotation)
	{
		return vec3_text(euler_degrees(rotation));
	}

	inline bool parse_vec3(const std::string& text, camera_vec3* out)
	{
		vgui_editor_kit::vector_editor::vector_value values{};
		if (!out || !vgui_editor_kit::vector_editor::parse_value(text, 3,
			values))
			return false;
		camera_vec3 parsed{ (float)values[0], (float)values[1],
			(float)values[2] };
		if (!std::isfinite(parsed.x) || !std::isfinite(parsed.y) ||
			!std::isfinite(parsed.z))
			return false;
		*out = parsed;
		return true;
	}

	/* Applies one gizmo delta to a transform.  Translation and rotation are
	 * world-space; rotation also orbits the position around the gizmo pivot.
	 * Scale multiplies positions relative to the pivot and maps each gizmo axis
	 * to the closest local axis of the object (exact for local space and for
	 * axis-aligned world space; a non-uniform world scale of a rotated object
	 * would otherwise need shear, which this sample does not represent). */
	inline void apply(const gizmo_transform& delta, transform* value)
	{
		if (!value)
			return;

		value->position.x += delta.translation.x;
		value->position.y += delta.translation.y;
		value->position.z += delta.translation.z;

		const float angle = std::sqrt(delta.rotation.x * delta.rotation.x +
			delta.rotation.y * delta.rotation.y +
			delta.rotation.z * delta.rotation.z);
		if (angle > 1e-6f)
		{
			const glm::vec3 rotation_axis(delta.rotation.x / angle,
				delta.rotation.y / angle, delta.rotation.z / angle);
			const glm::quat rotation = glm::angleAxis(angle, rotation_axis);
			const glm::vec3 pivot = to_glm(delta.origin);
			const glm::vec3 position = to_glm(value->position);
			value->position = from_glm(pivot + rotation * (position - pivot));
			value->rotation = glm::normalize(rotation * value->rotation);
		}

		const float factor[3] = { delta.scale.x, delta.scale.y,
			delta.scale.z };
		const bool has_scale = std::fabs(factor[0] - 1.0f) > 1e-6f ||
			std::fabs(factor[1] - 1.0f) > 1e-6f ||
			std::fabs(factor[2] - 1.0f) > 1e-6f;
		if (!has_scale)
			return;

		const bool uniform = std::fabs(factor[0] - factor[1]) < 1e-6f &&
			std::fabs(factor[0] - factor[2]) < 1e-6f;
		const glm::vec3 pivot = to_glm(delta.origin);
		glm::vec3 position = to_glm(value->position);
		const glm::vec3 offset = position - pivot;
		if (uniform)
		{
			position = pivot + offset * factor[0];
			value->scale.x = clamp_scale(value->scale.x * factor[0]);
			value->scale.y = clamp_scale(value->scale.y * factor[0]);
			value->scale.z = clamp_scale(value->scale.z * factor[0]);
			value->position = from_glm(position);
			return;
		}

		for (int i = 0; i < 3; ++i)
		{
			if (std::fabs(factor[i] - 1.0f) <= 1e-6f)
				continue;
			const glm::vec3 scale_axis = axis(delta, i);
			position += scale_axis * glm::dot(offset, scale_axis) *
				(factor[i] - 1.0f);
		}
		value->position = from_glm(position);

		const glm::mat3 local_axes = glm::mat3_cast(
			glm::normalize(value->rotation));
		for (int i = 0; i < 3; ++i)
		{
			if (std::fabs(factor[i] - 1.0f) <= 1e-6f)
				continue;
			const glm::vec3 scale_axis = axis(delta, i);
			int best_axis = 0;
			float best_alignment = -1.0f;
			for (int local = 0; local < 3; ++local)
			{
				const float alignment = std::fabs(
					glm::dot(scale_axis, local_axes[local]));
				if (alignment > best_alignment)
				{
					best_alignment = alignment;
					best_axis = local;
				}
			}
			if (best_axis == 0)
				value->scale.x = clamp_scale(value->scale.x * factor[i]);
			else if (best_axis == 1)
				value->scale.y = clamp_scale(value->scale.y * factor[i]);
			else
				value->scale.z = clamp_scale(value->scale.z * factor[i]);
		}
	}

	inline bool changed(const gizmo_transform& delta)
	{
		const float translation = std::fabs(delta.translation.x) +
			std::fabs(delta.translation.y) + std::fabs(delta.translation.z);
		const float rotation = std::fabs(delta.rotation.x) +
			std::fabs(delta.rotation.y) + std::fabs(delta.rotation.z);
		const float scale = std::fabs(delta.scale.x - 1.0f) +
			std::fabs(delta.scale.y - 1.0f) +
			std::fabs(delta.scale.z - 1.0f);
		return translation > 1e-4f || rotation > 1e-5f || scale > 1e-5f;
	}

} /* namespace editor_transform */
