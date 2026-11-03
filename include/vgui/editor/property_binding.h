#pragma once
/*
 * property_binding.h - PropertyGrid rows backed by a struct field.
 *
 * A PropertyGrid row normally needs two hand-written lambdas (get_value /
 * set_value) plus a format/parse pair.  field_property() builds the descriptor
 * from a member pointer instead, so a settings/component struct can be edited
 * directly:
 *
 *   using namespace vgui_editor_kit;
 *   grid.add_property(field_property("volume", "Volume", property_type::number,
 *                                    &settings, &Settings::volume, ui));
 *
 * Semantics:
 *   - get_value formats the field (bool -> "true"/"false", floats without a
 *     trailing zero noise, std::string as-is);
 *   - set_value parses the canonical text form (see property_desc docs), writes
 *     the field and marks the model dirty so widgets bound to that model resync
 *     on the next update;
 *   - PropertyGrid still records its own undo command when a command stack is
 *     attached, so this composes with binding_undo without double recording.
 *
 * Supported field types: bool, int (and other integrals), float, double,
 * std::string.  Anything else fails to compile on purpose.
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

#include <vgui/vgui.h>

#include "property_grid.h"

namespace vgui_editor_kit
{
	namespace detail
	{
		inline std::string field_to_text(bool value) { return value ? "true" : "false"; }
		inline std::string field_to_text(const std::string& value) { return value; }

		template <typename T>
		inline std::string field_to_text(T value)
		{
			static_assert(std::is_arithmetic<T>::value,
				"field_property: unsupported field type (use bool/int/float/double/std::string)");
			if constexpr (std::is_same<T, float>::value || std::is_same<T, double>::value)
			{
				char buf[64] = {};
				std::snprintf(buf, sizeof(buf), "%.6g", (double)value);
				return std::string(buf);
			}
			else
			{
				return std::to_string((long long)value);
			}
		}

		template <typename T>
		inline bool field_from_text(const std::string& text, T& out)
		{
			static_assert(std::is_arithmetic<T>::value || std::is_same<T, std::string>::value,
				"field_property: unsupported field type (use bool/int/float/double/std::string)");
			if constexpr (std::is_same<T, std::string>::value)
			{
				out = text;
				return true;
			}
			else if constexpr (std::is_same<T, bool>::value)
			{
				out = (text == "true" || text == "1");
				return true;
			}
			else
			{
				try
				{
					if constexpr (std::is_integral<T>::value)
						out = (T)std::stoll(text);
					else
						out = (T)std::stod(text);
				}
				catch (...)
				{
					return false;
				}
				return true;
			}
		}
	} /* namespace detail */

	template <typename M, typename T>
	property_desc field_property(std::string id, std::string label,
		property_type type, M* model, T M::*member, vgui_context* ctx = nullptr)
	{
		property_desc desc;
		desc.id = std::move(id);
		desc.label = std::move(label);
		desc.type = type;
		desc.value = detail::field_to_text(model->*member);
		desc.get_value = [model, member]() {
			return detail::field_to_text(model->*member);
		};
		desc.set_value = [model, member, ctx](const std::string& text) {
			T value{};
			if (!detail::field_from_text(text, value)) return;
			model->*member = value;
			if (ctx) vgui_model_mark_dirty(ctx, model);
		};
		return desc;
	}

	/* Convenience wrappers: type is inferred from the member. */
	template <typename M>
	property_desc field_property(std::string id, std::string label,
		M* model, bool M::*member, vgui_context* ctx = nullptr)
	{
		return field_property(std::move(id), std::move(label), property_type::boolean,
			model, member, ctx);
	}

	template <typename M>
	property_desc field_property(std::string id, std::string label,
		M* model, std::string M::*member, vgui_context* ctx = nullptr)
	{
		return field_property(std::move(id), std::move(label), property_type::text,
			model, member, ctx);
	}

	/* One arithmetic overload for both integral and floating point: two separate
	   overloads that only differ by a default template argument (enable_if in the
	   template parameter list) would redeclare the same template. */
	template <typename M, typename T,
		typename = std::enable_if_t<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value>>
	property_desc field_property(std::string id, std::string label,
		M* model, T M::*member, vgui_context* ctx = nullptr)
	{
		return field_property(std::move(id), std::move(label),
			std::is_floating_point<T>::value ? property_type::number : property_type::integer,
			model, member, ctx);
	}
} /* namespace vgui_editor_kit */
