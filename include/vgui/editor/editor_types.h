#pragma once
/*
 * editor_types.h - shared value types for vgui_editor_kit.
 *
 * This header deliberately contains no editor-specific document model.  The
 * host application owns scene/asset data; editor kit components exchange
 * stable IDs and small payload records through these types.
 */
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vgui/vgui.h>

namespace vgui_editor_kit {

	using object_id = std::uint64_t;
	inline constexpr object_id invalid_object = 0;

	/* Shared visual constants for the editor kit's canvas/node-graph views.
	 * Centralised so NodeGraph and Canvas stay consistent: every visual element
	 * (fonts, node borders, connection lines, ports) scales 1:1 with zoom,
	 * growing and shrinking with the view like part of the canvas, with only a
	 * small floor so things stay clickable at extreme zoom-out. Sizes are in
	 * logical units; the drawing helpers apply the context ui_scale on top. */
	inline constexpr float kEditorBaseFontSize = 15.0f;
	inline constexpr float kEditorMinFontSizePx = 1.0f;

	inline constexpr float kEditorMinBorderPx = 0.75f;

	inline constexpr float kEditorBaseLineWidthPx = 2.0f;
	inline constexpr float kEditorMinLineWidthPx = 1.0f;

	enum class drag_payload_kind
	{
		none = 0,
		text,
		files,
		asset,
		scene_object,
		custom
	};

	struct drag_payload
	{
		drag_payload_kind kind = drag_payload_kind::none;
		std::string text;
		std::vector<std::string> files;
		std::string id;
		const void* data = nullptr;

		bool empty() const noexcept
		{
			return kind == drag_payload_kind::none;
		}
	};

	class drop_target
	{
	public:
		virtual ~drop_target() = default;
		virtual bool can_accept(const drag_payload& payload) const = 0;
		/* Optional hover feedback while an accepted payload hovers the widget
		 * this target is attached to.  The default draws a horizontal
		 * insertion line across the widget; override to emit line/rect hints
		 * in absolute window coordinates, or leave hint->kind = NONE to
		 * signal that the current position rejects the drop. */
		virtual void hover(const drag_payload& payload, vgui_widget* target,
			float x, float y, vgui_drop_hint* hint);
		virtual bool drop(const drag_payload& payload, float x, float y) = 0;
	};

	struct row_info
	{
		vgui_item_id id = 0;
		std::string text;
		std::string detail;
		bool enabled = true;
	};

	/* Transitional interface for virtual list/tree/table data.  The first
	 * implementation may materialize rows, but callers should already treat this
	 * as a pull-based model so the core can switch to visible-row queries later. */
	class row_model
	{
	public:
		virtual ~row_model() = default;
		virtual std::size_t row_count() const = 0;
		virtual row_info row_at(std::size_t index) const = 0;
	};

} /* namespace vgui_editor_kit */
