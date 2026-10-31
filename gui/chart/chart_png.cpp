#include "chart_internal.h"

/* The implementation of stb_image_write is compiled inside libovg
 * (ovg_fonts.cpp); the vgui::chart link interface pulls libovg in, so only
 * the declarations are needed here. */
#include <stb_image_write.h>

#include <algorithm>
#include <cstring>

namespace vgchart {

	namespace {

		struct png_renderer {
			vgui_chart_png_render_fn render = nullptr;
			void* user = nullptr;
		};

		png_renderer& renderer_slot()
		{
			static png_renderer slot;
			return slot;
		}

		struct png_stream {
			vgui_chart_write_fn write = nullptr;
			void* user = nullptr;
			bool failed = false;
		};

		void png_write(void* context, void* data, int size)
		{
			png_stream* stream = (png_stream*)context;
			if (!stream || stream->failed) return;
			if (size > 0 && !stream->write(stream->user, (const char*)data,
				(size_t)size))
				stream->failed = true;
		}

		vgui_chart_png_desc read_desc(const vgui_chart_png_desc* desc)
		{
			vgui_chart_png_desc spec = {};
			size_t bytes = sizeof(spec);
			if (desc->struct_size && desc->struct_size < bytes)
				bytes = desc->struct_size;
			std::memcpy(&spec, desc, bytes);
			spec.width = std::clamp(spec.width, 1, 16384);
			spec.height = std::clamp(spec.height, 1, 16384);
			return spec;
		}

		const uint8_t* render_pixels(vgui_widget* widget,
			const vgui_chart_png_desc& spec, vgui_chart_png_render_fn render,
			void* render_user, int& out_stride)
		{
			const uint8_t* rgba = nullptr;
			out_stride = 0;
			if (!render(render_user, widget, spec.width, spec.height,
				spec.background, &rgba, &out_stride))
				return nullptr;
			if (!rgba || out_stride < spec.width * 4)
				return nullptr;
			return rgba;
		}

		bool encode_png(const uint8_t* rgba, const vgui_chart_png_desc& spec,
			int stride, vgui_chart_write_fn write, void* user)
		{
			png_stream stream;
			stream.write = write;
			stream.user = user;
			const int ok = stbi_write_png_to_func(&png_write, &stream,
				spec.width, spec.height, 4, rgba, stride);
			return ok != 0 && !stream.failed;
		}

	} /* namespace */

} /* namespace vgchart */

extern "C" {

	int vgui_chart_set_png_renderer(vgui_chart_png_render_fn render, void* user)
	{
		vgchart::png_renderer& slot = vgchart::renderer_slot();
		slot.render = render;
		slot.user = user;
		return 1;
	}

	void vgui_chart_png_desc_default(vgui_chart_png_desc* desc)
	{
		if (!desc) return;
		std::memset(desc, 0, sizeof(*desc));
		desc->struct_size = (uint32_t)sizeof(vgui_chart_png_desc);
		desc->width = 800;
		desc->height = 600;
		desc->background = 0;
	}

	int vgui_chart_export_png(vgui_chart* chart, const vgui_chart_png_desc* desc,
		vgui_chart_write_fn write, void* user)
	{
		if (!chart || !desc || !write)
		{
			vgchart::set_chart_error(chart, "invalid PNG export arguments");
			return 0;
		}
		const vgui_chart_png_desc spec = vgchart::read_desc(desc);
		const vgchart::png_renderer& slot = vgchart::renderer_slot();
		if (!slot.render)
		{
			vgchart::set_chart_error(chart, "no PNG renderer installed");
			return 0;
		}
		int stride = 0;
		const uint8_t* rgba = vgchart::render_pixels(chart->widget, spec,
			slot.render, slot.user, stride);
		if (!rgba)
		{
			vgchart::set_chart_error(chart, "PNG render failed");
			return 0;
		}
		if (!vgchart::encode_png(rgba, spec, stride, write, user))
		{
			vgchart::set_chart_error(chart, "PNG encode failed");
			return 0;
		}
		chart->last_error.clear();
		return 1;
	}

	int vgui_dashboard_export_png(vgui_dashboard* dashboard,
		const vgui_chart_png_desc* desc, vgui_chart_write_fn write, void* user)
	{
		if (!dashboard || !desc || !write)
		{
			vgchart::set_dashboard_error(dashboard,
				"invalid PNG export arguments");
			return 0;
		}
		const vgui_chart_png_desc spec = vgchart::read_desc(desc);
		const vgchart::png_renderer& slot = vgchart::renderer_slot();
		if (!slot.render)
		{
			vgchart::set_dashboard_error(dashboard, "no PNG renderer installed");
			return 0;
		}
		int stride = 0;
		const uint8_t* rgba = vgchart::render_pixels(dashboard->root, spec,
			slot.render, slot.user, stride);
		if (!rgba)
		{
			vgchart::set_dashboard_error(dashboard, "PNG render failed");
			return 0;
		}
		if (!vgchart::encode_png(rgba, spec, stride, write, user))
		{
			vgchart::set_dashboard_error(dashboard, "PNG encode failed");
			return 0;
		}
		dashboard->last_error.clear();
		return 1;
	}

} /* extern "C" */
