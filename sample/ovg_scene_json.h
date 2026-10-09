#pragma once
/*
 * ovg_scene_json.h — OVGS (OVG Scene) JSON 序列化
 *
 * 场景图格式 <-> nlohmann::json <-> OVG 录制/绘制调用
 *
 * 依赖:
 *   - ovg_c.h        (OVG 渲染接口)
 *   - nlohmann/json.hpp
 *   - C++17
 *
 * 用法:
 *   ovg::Scene scene;
 *   ovg::JsonIO::load("ui.ovgs", scene);   // 读
 *   ovg::JsonIO::save("ui.ovgs", scene);   // 写
 *
 *   // 编辑后重绘:
 *   ovg::SceneRenderer r(scene, ctx_cb);    // ctx_cb 为 ovg_ctx_cb*
 *   r.render(rvg);
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#ifndef OVG_STUB_TYPES
#include "ovg_c.h"
#endif

#include <nlohmann/json.hpp>

using njson = nlohmann::json;

namespace ovg {

	// ============================================================
	//  底层数学/颜色/变换 辅助
	// ============================================================

	inline uint32_t hex_to_color(const std::string& s, uint32_t def = 0) {
		if (s.empty()) return def;
		std::string t = s;
		if (t[0] == '#' || t[0] == '0') t.erase(0, t[0] == '#' ? 1 : 2);
		if (t.size() != 8) return def;
		return (uint32_t)std::stoul(t, nullptr, 16);
	}

	inline std::string color_to_hex(uint32_t c) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08X", c);
		return buf;
	}

	// mat3x2 行主序: m[0..1]=第一行, m[2..3]=第二行, m[4..5]=平移
	using Mat3x2 = std::array<float, 6>;
	using Mat4 = std::array<float, 16>;

	inline Mat3x2 identity3x2() { return { 1,0,0,1,0,0 }; }

	inline Mat3x2 parse_transform(const njson& j, const Mat3x2& base = identity3x2()) {
		Mat3x2 m = base;
		if (j.is_object() && j.contains("matrix")) {
			auto& a = j["matrix"];
			if (a.size() == 6) for (int i = 0; i < 6; ++i) m[i] = a[i].get<float>();
			return m;
		}
		// 分解简写
		float tx = 0, ty = 0, sx = 1, sy = 1, rot = 0;
		if (j.is_object() && j.contains("translate")) {
			auto& v = j["translate"]; tx = v[0]; ty = v[1];
		}
		if (j.is_object() && j.contains("scale")) {
			auto& v = j["scale"]; sx = v[0]; sy = v[1];
		}
		if (j.is_object() && j.contains("rotate")) rot = j["rotate"].get<float>();
		float c = std::cos(rot), si = std::sin(rot);
		m[0] = c * sx; m[1] = si * sx; m[2] = -si * sy; m[3] = c * sy; m[4] = tx; m[5] = ty;
		return m;
	}

	inline njson transform_to_json(const Mat3x2& m) {
		return njson{ {"matrix", {m[0],m[1],m[2],m[3],m[4],m[5]}} };
	}

	inline Mat4 identity4() {
		Mat4 m{}; m[0] = m[5] = m[10] = m[15] = 1.f; return m;
	}

	// ============================================================
	//  资源定义
	// ============================================================

	enum class PatternType { Solid, Linear, Radial, Sweep, Texture };

	struct ColorStop {
		float offset = 0;
		uint32_t color = 0;
	};

	struct Gradient {
		PatternType type = PatternType::Linear;
		vec2 p0{}, p1{};
		vec2 c0{}, c1{}; float r0 = 0, r1 = 0; bool ellipse = false;
		vec2 center{}; float start_angle = 0, end_angle = 6.2831853f;
		vg_extend_t extend = VG_EXTEND_PAD;
		vg_filter_t filter = VG_FILTER_BILINEAR;
		std::vector<ColorStop> stops;
	};

	struct TextureRes {
		uint32_t width = 0, height = 0;
		vg_format_t format = VG_FORMAT_RGBA8;
		std::vector<uint8_t> pixels;   // 原始像素字节
		uint32_t stride = 0;           // 0 = 自动计算
		bool multiply = false;
	};

	using Pattern = std::variant<uint32_t, Gradient, TextureRes>;

	// ============================================================
	//  绘制节点
	// ============================================================

	enum class SegmentOp {
		MoveTo, LineTo, QuadTo, CubicTo, ArcTo, Close, Rect, Circle, Ellipse
	};

	struct PathSegment {
		SegmentOp op = SegmentOp::MoveTo;
		float x = 0, y = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		float radius = 0, radius_x = 0, radius_y = 0, rotation = 0;
		bool large_arc = false, sweep = false;
	};

	struct StrokeStyle {
		std::string pattern_ref;
		float width = 1.0f;
		vg_line_cap_t cap = VG_LINE_CAP_BUTT;
		vg_line_join_t join = VG_LINE_JOIN_MITER;
		float miter_limit = 4.0f;
		std::vector<float> dash;
		float dash_offset = 0;
	};

	struct PathNode {
		std::vector<PathSegment> segments;
		std::string fill_ref;      // 空=不填充
		std::string stroke_ref;    // 空=不描边
		vg_fill_rule_t winding = VG_FILL_RULE_NON_ZERO;
		StrokeStyle stroke;
	};

	struct RectShape {
		float x = 0, y = 0, w = 0, h = 0, rx = 0, ry = 0;
		std::string fill_ref, stroke_ref;
		StrokeStyle stroke;
		vg_fill_rule_t winding = VG_FILL_RULE_NON_ZERO;
	};

	struct CircleShape { float x = 0, y = 0, radius = 0; std::string fill_ref, stroke_ref; StrokeStyle stroke; };
	struct EllipseShape { float cx = 0, cy = 0, rx = 0, ry = 0; std::string fill_ref, stroke_ref; StrokeStyle stroke; };

	struct ImageNode {
		std::string image_ref;
		ivec4 src_rect{};      // x,y,w,h
		ivec4 dst_rect{};
		ivec4 sliced{};        // 九宫格 l,t,r,b
		uint32_t color = 0xFFFFFFFFu;
		int flip = FLIP_NONE;
	};

	struct TextNode {
		std::string text;
		ivec4 box{};
		std::string font_family, font_style;
		float fontsize = 16, lineheight = 1.2f;
		vec2 align{ 0.5f,0.5f }, box_align{ 0,0 };
		uint32_t color = 0xFFC2C2C2u, stroke_color = 0xFF000000u, shadow_color = 0xCC121212u;
		float stroke_width = 0;
		vec2 shadow_offset{ 1,1 };
		int min_subpixel = 0;
		int word_wrap = 2, auto_break = 1, ellipsis = 0;
	};

	struct GeomVertex { vec3 pos{}; vec2 uv{}; uint32_t color = 0xFFFFFFFFu; };

	struct GeometryNode {
		std::string mode = "triangles";   // triangles/triangle_strip/lines/line_strip/points
		std::string blend = "normal";
		std::string cull = "none";        // none/front/back
		bool double_sided = false;
		std::string texture_ref;
		std::vector<GeomVertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Mat4> instances;
	};

	struct GroupNode;

	using SceneNode = std::variant<
		PathNode, RectShape, CircleShape, EllipseShape,
		ImageNode, TextNode, GeometryNode,
		std::shared_ptr<GroupNode>
	>;

	struct GroupNode {
		std::string name;
		Mat3x2 transform = identity3x2();
		float opacity = 1.0f;
		ivec4 clip_rect{};      // x,y,w,h ; w<=0 表示不裁剪
		std::vector<SceneNode> children;
	};

	// ============================================================
	//  场景
	// ============================================================

	class Scene {
	public:
		int width = 0, height = 0;
		uint32_t background = 0;
		std::string name;
		std::unordered_map<std::string, Pattern> resources;
		std::vector<SceneNode> nodes;

		// ---- 编辑 API ----
		void add(std::shared_ptr<GroupNode> g) { nodes.push_back(g); }
		template<class T> void add(T&& n) { nodes.emplace_back(std::forward<T>(n)); }

		std::shared_ptr<GroupNode> new_group(const std::string& name = "") {
			auto g = std::make_shared<GroupNode>();
			g->name = name;
			nodes.push_back(g);
			return g;
		}

		// 创建/获取资源
		std::string add_solid(const std::string& name, uint32_t color) {
			resources[name] = color;
			return "@" + name;
		}
		std::string add_gradient(const std::string& name, Gradient g) {
			resources[name] = std::move(g);
			return "@" + name;
		}
		std::string add_texture(const std::string& name, TextureRes t) {
			resources[name] = std::move(t);
			return "@" + name;
		}

		void remove_node(size_t idx) {
			if (idx < nodes.size()) nodes.erase(nodes.begin() + idx);
		}
		SceneNode& at(size_t idx) { return nodes.at(idx); }
		size_t size() const { return nodes.size(); }
		void clear() { nodes.clear(); resources.clear(); }
	};

	// ============================================================
	//  Base64
	// ============================================================

	inline std::string base64_encode(const uint8_t* data, size_t len) {
		static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out; out.reserve(((len + 2) / 3) * 4);
		for (size_t i = 0; i < len; i += 3) {
			uint32_t v = ((uint32_t)data[i]) << 16;
			if (i + 1 < len) v |= ((uint32_t)data[i + 1]) << 8;
			if (i + 2 < len) v |= data[i + 2];
			out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
			out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
			out += (i + 2 < len) ? tbl[v & 63] : '=';
		}
		return out;
	}

	inline std::vector<uint8_t> base64_decode(const std::string& s) {
		if (s.empty())return {};
		static const uint8_t tbl[256] = { 0 };
		static const auto init = []() {
			uint8_t t[256] = { 0 };
			for (int i = 0; i < 256; ++i)t[i] = 0xFF;
			for (int i = 0; i < 64; ++i)t[(uint8_t)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
			memcpy(const_cast<uint8_t*>(tbl), t, 256);
			return 0;
			}();
		(void)init;
		std::vector<uint8_t> out; out.reserve(s.size() * 3 / 4);
		uint32_t v = 0; int n = 0;
		for (char c : s) {
			if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
			uint8_t d = tbl[(uint8_t)c];
			if (d == 0xFF) continue;
			v = (v << 6) | d; n += 6;
			if (n == 24) { out.push_back(v >> 16); out.push_back(v >> 8); out.push_back(v); n = 0; v = 0; }
		}
		if (n == 12) out.push_back(v >> 4);
		else if (n == 18) { out.push_back(v >> 10); out.push_back(v >> 2); }
		return out;
	}

	// ============================================================
	//  枚举映射
	// ============================================================

	inline vg_line_cap_t parse_cap(const std::string& s) {
		if (s == "round") return VG_LINE_CAP_ROUND;
		if (s == "square") return VG_LINE_CAP_SQUARE;
		return VG_LINE_CAP_BUTT;
	}
	inline std::string cap_to_str(vg_line_cap_t e) {
		if (e == VG_LINE_CAP_ROUND) return "round";
		if (e == VG_LINE_CAP_SQUARE) return "square";
		return "butt";
	}
	inline vg_line_join_t parse_join(const std::string& s) {
		if (s == "round") return VG_LINE_JOIN_ROUND;
		if (s == "bevel") return VG_LINE_JOIN_BEVEL;
		return VG_LINE_JOIN_MITER;
	}
	inline std::string join_to_str(vg_line_join_t e) {
		if (e == VG_LINE_JOIN_ROUND) return "round";
		if (e == VG_LINE_JOIN_BEVEL) return "bevel";
		return "miter";
	}
	inline vg_fill_rule_t parse_winding(const std::string& s) {
		return (s == "evenodd") ? VG_FILL_RULE_EVEN_ODD : VG_FILL_RULE_NON_ZERO;
	}
	inline std::string winding_to_str(vg_fill_rule_t e) {
		return (e == VG_FILL_RULE_EVEN_ODD) ? "evenodd" : "nonzero";
	}
	inline vg_extend_t parse_extend(const std::string& s) {
		if (s == "repeat") return VG_EXTEND_REPEAT;
		if (s == "reflect") return VG_EXTEND_REFLECT;
		if (s == "none") return VG_EXTEND_NONE;
		return VG_EXTEND_PAD;
	}
	inline std::string extend_to_str(vg_extend_t e) {
		if (e == VG_EXTEND_REPEAT) return "repeat";
		if (e == VG_EXTEND_REFLECT) return "reflect";
		if (e == VG_EXTEND_NONE) return "none";
		return "pad";
	}
	inline vg_filter_t parse_filter(const std::string& s) {
		if (s == "fast") return VG_FILTER_FAST;
		if (s == "good") return VG_FILTER_GOOD;
		if (s == "best") return VG_FILTER_BEST;
		if (s == "nearest") return VG_FILTER_NEAREST;
		if (s == "gaussian") return VG_FILTER_GAUSSIAN;
		return VG_FILTER_BILINEAR;
	}
	inline std::string filter_to_str(vg_filter_t e) {
		switch (e) {
		case VG_FILTER_FAST: return "fast";
		case VG_FILTER_GOOD: return "good";
		case VG_FILTER_BEST: return "best";
		case VG_FILTER_NEAREST: return "nearest";
		case VG_FILTER_GAUSSIAN: return "gaussian";
		default: return "bilinear";
		}
	}
	inline vg_format_t parse_format(const std::string& s) {
		if (s == "bgra8") return VG_FORMAT_BGRA8;
		if (s == "rgba8_srgb") return VG_FORMAT_RGBA8_SRGB;
		if (s == "bgra8_srgb") return VG_FORMAT_BGRA8_SRGB;
		if (s == "rgba16f") return VG_FORMAT_RGBA16F;
		if (s == "rgba32f") return VG_FORMAT_RGBA32F;
		return VG_FORMAT_RGBA8;
	}
	inline std::string format_to_str(vg_format_t e) {
		switch (e) {
		case VG_FORMAT_BGRA8: return "bgra8";
		case VG_FORMAT_RGBA8_SRGB: return "rgba8_srgb";
		case VG_FORMAT_BGRA8_SRGB: return "bgra8_srgb";
		case VG_FORMAT_RGBA16F: return "rgba16f";
		case VG_FORMAT_RGBA32F: return "rgba32f";
		default: return "rgba8";
		}
	}
	inline int parse_flip(const std::string& s) {
		if (s == "horizontal") return FLIP_HORIZONTAL;
		if (s == "vertical") return FLIP_VERTICAL;
		if (s == "both") return FLIP_HORIZONTAL_AND_VERTICAL;
		return FLIP_NONE;
	}
	inline std::string flip_to_str(int f) {
		if (f == FLIP_HORIZONTAL) return "horizontal";
		if (f == FLIP_VERTICAL) return "vertical";
		if (f == FLIP_HORIZONTAL_AND_VERTICAL) return "both";
		return "none";
	}
	inline int parse_blend(const std::string& s) {
		if (s == "none") return (int)blendMode_e::none;
		if (s == "normal") return (int)blendMode_e::normal;
		if (s == "additive") return (int)blendMode_e::additive;
		if (s == "multiply") return (int)blendMode_e::multiply;
		if (s == "modulate") return (int)blendMode_e::modulate;
		if (s == "screen") return (int)blendMode_e::screen;
		if (s == "normal_prem") return (int)blendMode_e::normal_prem;
		if (s == "additive_prem") return (int)blendMode_e::additive_prem;
		return (int)blendMode_e::normal;
	}
	inline std::string blend_to_str(int b) {
		switch ((blendMode_e)b) {
		case blendMode_e::none: return "none";
		case blendMode_e::additive: return "additive";
		case blendMode_e::multiply: return "multiply";
		case blendMode_e::modulate: return "modulate";
		case blendMode_e::screen: return "screen";
		case blendMode_e::normal_prem: return "normal_prem";
		case blendMode_e::additive_prem: return "additive_prem";
		default: return "normal";
		}
	}

	inline ivec4 parse_rect4(const njson& j) {
		ivec4 r{};
		if (j.is_array() && j.size() >= 4) {
			r.x = j[0]; r.y = j[1]; r.z = j[2]; r.w = j[3];
		}
		return r;
	}
	inline njson rect4_to_json(const ivec4& r) {
		return { r.x, r.y, r.z, r.w };
	}

	// ============================================================
	//  资源序列化
	// ============================================================

	inline njson pattern_to_json(const Pattern& p) {
		if (auto* c = std::get_if<uint32_t>(&p)) {
			return { {"type","solid"},{"color",color_to_hex(*c)} };
		}
		else if (auto* g = std::get_if<Gradient>(&p)) {
			njson j;
			if (g->type == PatternType::Linear) {
				j["type"] = "linear";
				j["p0"] = { g->p0.x,g->p0.y }; j["p1"] = { g->p1.x,g->p1.y };
			}
			else if (g->type == PatternType::Radial) {
				j["type"] = "radial";
				j["c0"] = { g->c0.x,g->c0.y }; j["r0"] = g->r0;
				j["c1"] = { g->c1.x,g->c1.y }; j["r1"] = g->r1; j["ellipse"] = g->ellipse;
			}
			else {
				j["type"] = "sweep";
				j["center"] = { g->center.x,g->center.y };
				j["start_angle"] = g->start_angle; j["end_angle"] = g->end_angle;
			}
			j["extend"] = extend_to_str(g->extend);
			j["filter"] = filter_to_str(g->filter);
			for (auto& s : g->stops)
				j["stops"].push_back({ {"offset",s.offset},{"color",color_to_hex(s.color)} });
			return j;
		}
		else if (auto* t = std::get_if<TextureRes>(&p)) {
			njson j;
			j["type"] = "texture";
			j["format"] = format_to_str(t->format);
			j["width"] = t->width; j["height"] = t->height;
			j["stride"] = t->stride;
			j["multiply"] = t->multiply;
			if (!t->pixels.empty())
				j["pixels_base64"] = base64_encode(t->pixels.data(), t->pixels.size());
			return j;
		}
		return njson{};
	}

	inline Pattern pattern_from_json(const njson& j) {
		std::string type = j.value("type", "solid");
		if (type == "solid") {
			return (Pattern)hex_to_color(j.value("color", std::string("FFFFFFFF")));
		}
		else if (type == "linear" || type == "radial" || type == "sweep") {
			Gradient g;
			if (type == "linear") {
				g.type = PatternType::Linear;
				auto& p0 = j["p0"]; g.p0 = { p0[0],p0[1] };
				auto& p1 = j["p1"]; g.p1 = { p1[0],p1[1] };
			}
			else if (type == "radial") {
				g.type = PatternType::Radial;
				auto& c0 = j["c0"]; g.c0 = { c0[0],c0[1] }; g.r0 = j.value("r0", 0.f);
				auto& c1 = j["c1"]; g.c1 = { c1[0],c1[1] }; g.r1 = j.value("r1", 0.f);
				g.ellipse = j.value("ellipse", false);
			}
			else {
				g.type = PatternType::Sweep;
				auto& c = j["center"]; g.center = { c[0],c[1] };
				g.start_angle = j.value("start_angle", 0.f);
				g.end_angle = j.value("end_angle", 6.2831853f);
			}
			g.extend = parse_extend(j.value("extend", std::string("pad")));
			g.filter = parse_filter(j.value("filter", std::string("bilinear")));
			if (j.contains("stops"))
				for (auto& s : j["stops"])
					g.stops.push_back({ s.value("offset",0.f),
						hex_to_color(s.value("color",std::string("FFFFFFFF"))) });
			return g;
		}
		else if (type == "texture") {
			TextureRes t;
			t.format = parse_format(j.value("format", std::string("rgba8")));
			t.width = j.value("width", 0u); t.height = j.value("height", 0u);
			t.stride = j.value("stride", 0u); t.multiply = j.value("multiply", false);
			if (j.contains("pixels_base64"))
				t.pixels = base64_decode(j["pixels_base64"].get<std::string>());
			return t;
		}
		return (Pattern)(uint32_t)0xFFFFFFFFu;
	}

	inline njson resources_to_json(const Scene& sc) {
		njson j;
		for (auto& kv : sc.resources)
			j[kv.first] = pattern_to_json(kv.second);
		return j;
	}

	inline void resources_from_json(Scene& sc, const njson& j) {
		sc.resources.clear();
		for (auto& kv : j.items())
			sc.resources[kv.key()] = pattern_from_json(kv.value());
	}

	// ============================================================
	//  节点序列化
	// ============================================================

	inline void stroke_to_json(njson& j, const StrokeStyle& s) {
		j["width"] = s.width;
		j["cap"] = cap_to_str(s.cap);
		j["join"] = join_to_str(s.join);
		j["miter_limit"] = s.miter_limit;
		if (!s.dash.empty()) {
			j["dash"] = { {"array",s.dash},{"offset",s.dash_offset} };
		}
	}

	inline StrokeStyle stroke_from_json(const njson& j) {
		StrokeStyle s;
		s.width = j.value("width", 1.f);
		s.cap = parse_cap(j.value("cap", std::string("butt")));
		s.join = parse_join(j.value("join", std::string("miter")));
		s.miter_limit = j.value("miter_limit", 4.f);
		if (j.contains("dash")) {
			for (auto& v : j["dash"]["array"]) s.dash.push_back(v.get<float>());
			s.dash_offset = j["dash"].value("offset", 0.f);
		}
		return s;
	}

	inline void path_to_json(njson& out, const PathNode& p) {
		for (auto& seg : p.segments) {
			njson s;
			switch (seg.op) {
			case SegmentOp::MoveTo: s = { "move_to",{{"x",seg.x},{"y",seg.y}} }; break;
			case SegmentOp::LineTo: s = { "line_to",{{"x",seg.x},{"y",seg.y}} }; break;
			case SegmentOp::QuadTo: s = { "quad_to",{{"x1",seg.x1},{"y1",seg.y1},{"x2",seg.x2},{"y2",seg.y2}} }; break;
			case SegmentOp::CubicTo: s = { "cubic_to",{{"x1",seg.x1},{"y1",seg.y1},{"x2",seg.x2},{"y2",seg.y2},{"x3",seg.x},{"y3",seg.y}} }; break;
			case SegmentOp::ArcTo: s = { "arc_to",{{"x",seg.x},{"y",seg.y},{"large_arc",seg.large_arc},{"sweep",seg.sweep},{"rx",seg.radius_x},{"ry",seg.radius_y},{"phi",seg.rotation}} }; break;
			case SegmentOp::Close: s = "close"; break;
			default: break;
			}
			out.push_back(s);
		}
	}

	inline void path_from_json(const njson& segs, PathNode& p) {
		for (auto& s : segs) {
			PathSegment seg;
			if (s.is_string()) {
				std::string op = s.get<std::string>();
				if (op == "close") seg.op = SegmentOp::Close;
			}
			else {
				std::string op = s.value("op", "move_to");
				if (op == "move_to") { seg.op = SegmentOp::MoveTo; seg.x = s["x"]; seg.y = s["y"]; }
				else if (op == "line_to") { seg.op = SegmentOp::LineTo; seg.x = s["x"]; seg.y = s["y"]; }
				else if (op == "quad_to") {
					seg.op = SegmentOp::QuadTo; seg.x1 = s["x1"]; seg.y1 = s["y1"]; seg.x2 = s["x2"]; seg.y2 = s["y2"];
				}
				else if (op == "cubic_to") {
					seg.op = SegmentOp::CubicTo; seg.x1 = s["x1"]; seg.y1 = s["y1"]; seg.x2 = s["x2"]; seg.y2 = s["y2"];
					seg.x = s["x3"]; seg.y = s["y3"];
				}
				else if (op == "arc_to") {
					seg.op = SegmentOp::ArcTo; seg.x = s["x"]; seg.y = s["y"];
					seg.large_arc = s.value("large_arc", false); seg.sweep = s.value("sweep", false);
					seg.radius_x = s["rx"]; seg.radius_y = s["ry"]; seg.rotation = s.value("phi", 0.f);
				}
				else if (op == "close") seg.op = SegmentOp::Close;
			}
			p.segments.push_back(seg);
		}
		return;
	}

	inline njson node_to_json(const SceneNode& n);
	inline SceneNode node_from_json(const njson& j);

	inline njson node_to_json(const SceneNode& n) {
		return std::visit([&](auto&& arg) -> njson {
			using T = std::decay_t<decltype(arg)>;
			njson j;

			if constexpr (std::is_same_v<T, PathNode>) {
				j["type"] = "path";
				j["winding"] = winding_to_str(arg.winding);
				if (!arg.fill_ref.empty()) j["fill"] = arg.fill_ref;
				if (!arg.stroke_ref.empty()) { j["stroke"] = arg.stroke_ref; stroke_to_json(j["stroke_style"], arg.stroke); }
				j["segments"] = njson::array();
				path_to_json(j["segments"], arg);

			}
			else if constexpr (std::is_same_v<T, RectShape>) {
				j["type"] = "rect";
				j["x"] = arg.x; j["y"] = arg.y; j["w"] = arg.w; j["h"] = arg.h; j["rx"] = arg.rx; j["ry"] = arg.ry;
				if (!arg.fill_ref.empty()) j["fill"] = arg.fill_ref;
				if (!arg.stroke_ref.empty()) { j["stroke"] = arg.stroke_ref; stroke_to_json(j["stroke_style"], arg.stroke); }
				j["winding"] = winding_to_str(arg.winding);

			}
			else if constexpr (std::is_same_v<T, CircleShape>) {
				j["type"] = "circle";
				j["x"] = arg.x; j["y"] = arg.y; j["radius"] = arg.radius;
				if (!arg.fill_ref.empty()) j["fill"] = arg.fill_ref;
				if (!arg.stroke_ref.empty()) { j["stroke"] = arg.stroke_ref; stroke_to_json(j["stroke_style"], arg.stroke); }

			}
			else if constexpr (std::is_same_v<T, EllipseShape>) {
				j["type"] = "ellipse";
				j["cx"] = arg.cx; j["cy"] = arg.cy; j["rx"] = arg.rx; j["ry"] = arg.ry;
				if (!arg.fill_ref.empty()) j["fill"] = arg.fill_ref;
				if (!arg.stroke_ref.empty()) { j["stroke"] = arg.stroke_ref; stroke_to_json(j["stroke_style"], arg.stroke); }

			}
			else if constexpr (std::is_same_v<T, ImageNode>) {
				j["type"] = "image";
				j["image"] = arg.image_ref;
				j["src_rect"] = rect4_to_json(arg.src_rect);
				j["dst_rect"] = rect4_to_json(arg.dst_rect);
				if (arg.sliced.x | arg.sliced.y | arg.sliced.z | arg.sliced.w)
					j["sliced"] = rect4_to_json(arg.sliced);
				if (arg.color != 0xFFFFFFFFu) j["color"] = color_to_hex(arg.color);
				if (arg.flip != FLIP_NONE) j["flip"] = flip_to_str(arg.flip);

			}
			else if constexpr (std::is_same_v<T, TextNode>) {
				j["type"] = "text";
				j["text"] = arg.text;
				j["box"] = rect4_to_json(arg.box);
				j["font"] = { {"family",arg.font_family},{"style",arg.font_style} };
				j["style"] = {
					{"fontsize",arg.fontsize},{"lineheight",arg.lineheight},
					{"align",{arg.align.x,arg.align.y}},{"box_align",{arg.box_align.x,arg.box_align.y}},
					{"color",color_to_hex(arg.color)},{"stroke_color",color_to_hex(arg.stroke_color)},
					{"stroke_width",arg.stroke_width},
					{"shadow_color",color_to_hex(arg.shadow_color)},{"shadow_offset",{arg.shadow_offset.x,arg.shadow_offset.y}},
					{"min_subpixel",arg.min_subpixel},
					{"word_wrap",arg.word_wrap},{"auto_break",arg.auto_break},{"ellipsis",arg.ellipsis}
				};

			}
			else if constexpr (std::is_same_v<T, GeometryNode>) {
				j["type"] = "geometry";
				j["mode"] = arg.mode;
				j["blend"] = arg.blend;
				j["cull"] = arg.cull;
				j["double_sided"] = arg.double_sided;
				if (!arg.texture_ref.empty()) j["texture"] = arg.texture_ref;
				for (auto& v : arg.vertices)
					j["vertices"].push_back({ {"pos",{v.pos.x,v.pos.y,v.pos.z}},{"uv",{v.uv.x,v.uv.y}},{"color",color_to_hex(v.color)} });
				for (auto& i : arg.indices) j["indices"].push_back(i);
				for (auto& m : arg.instances) j["instances"].push_back(m);

			}
			else if constexpr (std::is_same_v<T, std::shared_ptr<GroupNode>>) {
				j["type"] = "group";
				if (!arg->name.empty()) j["name"] = arg->name;
				j["transform"] = transform_to_json(arg->transform);
				if (arg->opacity != 1.f) j["opacity"] = arg->opacity;
				if (arg->clip_rect.z > 0) j["clip_rect"] = rect4_to_json(arg->clip_rect);
				for (auto& c : arg->children) j["children"].push_back(node_to_json(c));
			}
			return j;
			}, n);
	}

	inline SceneNode node_from_json(const njson& j) {
		std::string type = j.value("type", "group");
		if (type == "path") {
			PathNode p;
			p.winding = parse_winding(j.value("winding", std::string("nonzero")));
			if (j.contains("fill")) p.fill_ref = j["fill"].get<std::string>();
			if (j.contains("stroke")) p.stroke_ref = j["stroke"].get<std::string>();
			if (j.contains("stroke_style")) p.stroke = stroke_from_json(j["stroke_style"]);
			if (j.contains("segments")) path_from_json(j["segments"], p);
			return p;
		}
		if (type == "rect") {
			RectShape r;
			r.x = j.value("x", 0.f); r.y = j.value("y", 0.f); r.w = j.value("w", 0.f); r.h = j.value("h", 0.f);
			r.rx = j.value("rx", 0.f); r.ry = j.value("ry", 0.f);
			if (j.contains("fill")) r.fill_ref = j["fill"].get<std::string>();
			if (j.contains("stroke")) r.stroke_ref = j["stroke"].get<std::string>();
			if (j.contains("stroke_style")) r.stroke = stroke_from_json(j["stroke_style"]);
			r.winding = parse_winding(j.value("winding", std::string("nonzero")));
			return r;
		}
		if (type == "circle") {
			CircleShape c; c.x = j.value("x", 0.f); c.y = j.value("y", 0.f); c.radius = j.value("radius", 0.f);
			if (j.contains("fill")) c.fill_ref = j["fill"].get<std::string>();
			if (j.contains("stroke")) c.stroke_ref = j["stroke"].get<std::string>();
			if (j.contains("stroke_style")) c.stroke = stroke_from_json(j["stroke_style"]);
			return c;
		}
		if (type == "ellipse") {
			EllipseShape e; e.cx = j.value("cx", 0.f); e.cy = j.value("cy", 0.f);
			e.rx = j.value("rx", 0.f); e.ry = j.value("ry", 0.f);
			if (j.contains("fill")) e.fill_ref = j["fill"].get<std::string>();
			if (j.contains("stroke")) e.stroke_ref = j["stroke"].get<std::string>();
			if (j.contains("stroke_style")) e.stroke = stroke_from_json(j["stroke_style"]);
			return e;
		}
		if (type == "image") {
			ImageNode im;
			im.image_ref = j.value("image", std::string());
			im.src_rect = parse_rect4(j["src_rect"]);
			im.dst_rect = parse_rect4(j["dst_rect"]);
			if (j.contains("sliced")) im.sliced = parse_rect4(j["sliced"]);
			im.color = hex_to_color(j.value("color", std::string("FFFFFFFF")));
			im.flip = parse_flip(j.value("flip", std::string("none")));
			return im;
		}
		if (type == "text") {
			TextNode t;
			t.text = j.value("text", std::string());
			t.box = parse_rect4(j["box"]);
			t.font_family = j["font"].value("family", std::string());
			t.font_style = j["font"].value("style", std::string());
			auto& st = j["style"];
			t.fontsize = st.value("fontsize", 16.f); t.lineheight = st.value("lineheight", 1.2f);
			auto& al = st["align"]; t.align = { al[0],al[1] };
			auto& ba = st["box_align"]; t.box_align = { ba[0],ba[1] };
			t.color = hex_to_color(st.value("color", std::string("FFC2C2C2")));
			t.stroke_color = hex_to_color(st.value("stroke_color", std::string("FF000000")));
			t.shadow_color = hex_to_color(st.value("shadow_color", std::string("CC121212")));
			t.stroke_width = st.value("stroke_width", 0.f);
			auto& so = st["shadow_offset"]; t.shadow_offset = { so[0],so[1] };
			t.min_subpixel = st.value("min_subpixel", 0);
			t.word_wrap = st.value("word_wrap", 2);
			t.auto_break = st.value("auto_break", 1);
			t.ellipsis = st.value("ellipsis", 0);
			return t;
		}
		if (type == "geometry") {
			GeometryNode g;
			g.mode = j.value("mode", std::string("triangles"));
			g.blend = j.value("blend", std::string("normal"));
			g.cull = j.value("cull", std::string("none"));
			g.double_sided = j.value("double_sided", false);
			if (j.contains("texture")) g.texture_ref = j["texture"].get<std::string>();
			for (auto& v : j["vertices"]) {
				GeomVertex gv;
				auto& p = v["pos"];
				gv.pos.x = p[0]; gv.pos.y = p[1];
				gv.pos.z = (p.size() > 2) ? (float)p[2] : 0.f;
				auto& uv = v["uv"]; gv.uv = { uv[0],uv[1] };
				gv.color = hex_to_color(v.value("color", std::string("FFFFFFFF")));
				g.vertices.push_back(gv);
			}
			for (auto& i : j["indices"]) g.indices.push_back(i.get<uint32_t>());
			for (auto& m : j["instances"]) {
				Mat4 mm; for (int k = 0; k < 16 && k < (int)m.size(); ++k) mm[k] = m[k].get<float>();
				g.instances.push_back(mm);
			}
			return g;
		}
		// group
		auto grp = std::make_shared<GroupNode>();
		grp->name = j.value("name", std::string());
		if (j.contains("transform")) grp->transform = parse_transform(j["transform"]);
		grp->opacity = j.value("opacity", 1.f);
		if (j.contains("clip_rect")) grp->clip_rect = parse_rect4(j["clip_rect"]);
		if (j.contains("children"))
			for (auto& c : j["children"]) grp->children.push_back(node_from_json(c));
		return grp;
	}

	// ============================================================
	//  Scene <-> JSON
	// ============================================================

	inline njson SceneToJson(const Scene& sc) {
		njson j;
		j["format"] = "ovgs";
		j["version"] = 1;
		j["width"] = sc.width;
		j["height"] = sc.height;
		if (sc.background != 0) j["background"] = color_to_hex(sc.background);
		j["resources"] = resources_to_json(sc);
		for (auto& n : sc.nodes) j["nodes"].push_back(node_to_json(n));
		return j;
	}

	inline Scene JsonToScene(const njson& j) {
		Scene sc;
		sc.width = j.value("width", 0);
		sc.height = j.value("height", 0);
		if (j.contains("background")) sc.background = hex_to_color(j["background"].get<std::string>());
		if (j.contains("resources")) resources_from_json(sc, j["resources"]);
		if (j.contains("nodes"))
			for (auto& n : j["nodes"]) sc.nodes.push_back(node_from_json(n));
		return sc;
	}

	// ============================================================
	//  文件 I/O
	// ============================================================

	struct JsonIO {
		static bool save(const std::string& path, const Scene& sc, bool pretty = true) {
			njson j = SceneToJson(sc);
			std::ofstream ofs(path, std::ios::binary);
			if (!ofs) return false;
			ofs << (pretty ? j.dump(2) : j.dump());
			return true;
		}
		static bool load(const std::string& path, Scene& out) {
			std::ifstream ifs(path, std::ios::binary);
			if (!ifs) return false;
			njson j;
			try { ifs >> j; }
			catch (...) { return false; }
			if (j.value("format", std::string()) != "ovgs") return false;
			out = JsonToScene(j);
			return true;
		}
		static bool save_to_string(const std::string& s, const Scene& sc) { (void)s; (void)sc; return false; }
	};

	// ============================================================
	//  渲染：Scene -> ovg_ctx_cb 调用
	// ============================================================

	class SceneRenderer {
	public:
		ovg_ctx_cb* cb;
		rvg_t* rvg = nullptr;
		Scene* scene = nullptr;
		std::unordered_map<std::string, vg_pattern_t*> pattern_cache;

		SceneRenderer(ovg_ctx_cb* c) : cb(c) {}
		~SceneRenderer() { clear_pattern_cache(); }

		void clear_pattern_cache() {
			for (auto& kv : pattern_cache) {
				// pattern 生命周期由调用方管理；这里不主动销毁
			}
			pattern_cache.clear();
		}

		vg_pattern_t* get_pattern(rvg_t* vg, const std::string& ref) {
			if (ref.empty()) return nullptr;
			std::string key = ref.substr(0, 1) == "@" ? ref.substr(1) : ref;
			auto it = pattern_cache.find(key);
			if (it != pattern_cache.end()) return it->second;

			auto rit = scene->resources.find(key);
			if (rit == scene->resources.end()) return nullptr;

			vg_pattern_t* pat = nullptr;
			auto& p = rit->second;
			if (auto* c = std::get_if<uint32_t>(&p)) {
				pat = nullptr; // 纯色不走 pattern，直接 set_source_color
			}
			else if (auto* g = std::get_if<Gradient>(&p)) {
				if (g->type == PatternType::Linear) {
					pat = cb->new_pattern_linear(vg, g->p0.x, g->p0.y, g->p1.x, g->p1.y);
				}
				else if (g->type == PatternType::Radial) {
					pat = cb->new_pattern_radial(vg, g->c0.x, g->c0.y, g->r0, g->c1.x, g->c1.y, g->r1, g->ellipse);
				}
				else {
					pat = cb->new_pattern_sweep(vg, g->center.x, g->center.y, g->start_angle, g->end_angle);
				}
				if (pat) {
					pat->extend = g->extend;
					pat->filter = g->filter;
					for (auto& s : g->stops) {
						float r, gc, b, a;
						uint32_t c = s.color;
						r = ((c >> 24) & 255) / 255.f; gc = ((c >> 16) & 255) / 255.f;
						b = ((c >> 8) & 255) / 255.f; a = (c & 255) / 255.f;
						cb->pattern_add_color_stop(pat, s.offset, r, gc, b, a);
					}
				}
			}
			else if (auto* t = std::get_if<TextureRes>(&p)) {
				// texture 资源在上传后通过 set_source_surface 使用
			}
			pattern_cache[key] = pat;
			return pat;
		}

		void apply_solid(uint32_t color) {
			cb->set_source_color(rvg, color);
		}

		void render(rvg_t* vg, Scene& sc) {
			scene = &sc;
			rvg = vg;
			rvg->width = scene->width;
			rvg->height = scene->height;
			if (sc.background != 0) {
				cb->set_source_color(vg, sc.background);
				cb->paint(vg);
			}
			for (auto& n : sc.nodes) render_node(n);
		}

		void render_node(SceneNode& n) {
			std::visit([&](auto&& arg) { render_impl(arg); }, n);
		}

		void render_impl(PathNode& p) {
			cb->new_path(rvg);
			for (auto& s : p.segments) {
				switch (s.op) {
				case SegmentOp::MoveTo: cb->move_to(rvg, s.x, s.y); break;
				case SegmentOp::LineTo: cb->line_to(rvg, s.x, s.y); break;
				case SegmentOp::QuadTo: cb->quadratic_to(rvg, s.x1, s.y1, s.x2, s.y2); break;
				case SegmentOp::CubicTo: cb->curve_to(rvg, s.x1, s.y1, s.x2, s.y2, s.x, s.y); break;
				case SegmentOp::Close: cb->close_path(rvg); break;
				default: break;
				}
			}
			cb->set_fill_rule(rvg, (int)p.winding);

			bool filled = !p.fill_ref.empty();
			bool stroked = !p.stroke_ref.empty();

			if (filled) {
				if (p.fill_ref[0] == '@') {
					std::string key = p.fill_ref.substr(1);
					auto it = scene->resources.find(key);
					if (it != scene->resources.end() && std::holds_alternative<uint32_t>(it->second))
						apply_solid(std::get<uint32_t>(it->second));
					else if (auto* pat = get_pattern(rvg, p.fill_ref))
						cb->set_source(rvg, pat);
				}
				cb->fill_preserve(rvg);
			}
			if (stroked) {
				cb->set_line_width(rvg, p.stroke.width);
				cb->set_line_cap(rvg, (int)p.stroke.cap);
				cb->set_line_join(rvg, (int)p.stroke.join);
				cb->set_miter_limit(rvg, p.stroke.miter_limit);
				if (!p.stroke.dash.empty())
					cb->set_dash(rvg, p.stroke.dash.data(), (uint32_t)p.stroke.dash.size(), p.stroke.dash_offset);
				else
					cb->set_dash(rvg, nullptr, 0, 0);

				if (p.stroke_ref[0] == '@') {
					std::string key = p.stroke_ref.substr(1);
					auto it = scene->resources.find(key);
					if (it != scene->resources.end() && std::holds_alternative<uint32_t>(it->second))
						apply_solid(std::get<uint32_t>(it->second));
					else if (auto* pat = get_pattern(rvg, p.stroke_ref))
						cb->set_source(rvg, pat);
				}
				cb->stroke(rvg);
			}
			if (!stroked) cb->clear_path(rvg);
		}

		void render_impl(RectShape& r) {
			// 圆角通过附加路径实现（若 rx/ry>0）
			if (r.rx > 0 || r.ry > 0) {
				// 简化：直接 rounded_rectangle
				cb->rounded_rectangle2(rvg, r.x, r.y, r.w, r.h, r.rx > 0 ? r.rx : r.ry, r.ry > 0 ? r.ry : r.rx);
				//cb->rounded_rectangle(rvg, r.x, r.y, r.w, r.h, r.rx);
			}
			else {
				cb->rectangle(rvg, r.x, r.y, r.w, r.h);
			}
			if (!r.fill_ref.empty()) {
				apply_pattern_ref(r.fill_ref);
				cb->fill_preserve(rvg);
			}
			if (!r.stroke_ref.empty()) {
				apply_stroke_style(r.stroke);
				apply_pattern_ref(r.stroke_ref);
				cb->stroke(rvg);
			}
			if (r.stroke_ref.empty()) cb->clear_path(rvg);
		}

		void render_impl(CircleShape& c) {
			cb->circle(rvg, c.x, c.y, c.radius);
			cb->arc(rvg, c.x, c.y, c.radius, 0, 2.0 * glm::pi<float>());
			if (!c.fill_ref.empty()) { apply_pattern_ref(c.fill_ref); cb->fill_preserve(rvg); }
			if (!c.stroke_ref.empty()) { apply_stroke_style(c.stroke); apply_pattern_ref(c.stroke_ref); cb->stroke(rvg); }
			if (c.stroke_ref.empty()) cb->clear_path(rvg);
		}

		void render_impl(EllipseShape& e) {
			cb->ellipse(rvg, e.rx, e.ry, e.cx, e.cy, 0);
			if (!e.fill_ref.empty()) { apply_pattern_ref(e.fill_ref); cb->fill_preserve(rvg); }
			if (!e.stroke_ref.empty()) { apply_stroke_style(e.stroke); apply_pattern_ref(e.stroke_ref); cb->stroke(rvg); }
			if (e.stroke_ref.empty()) cb->clear_path(rvg);
		}

		void render_impl(ImageNode& im) {
#ifndef OVG_STUB_TYPES
			// 图片资源需已由上层上传；此处通过 add_image 绘制
			ovg_image_r r{};
			// image_ref -> vg_image_t* 需要资源映射，由调用方填充 r.img
			r.rc = im.src_rect; r.sliced = im.sliced; r.dst = im.dst_rect; r.color = im.color; r.type = im.flip;
			// TODO: 需要把 image_ref 解析为 vg_image_t*（资源上传由调用方负责）
			if (r.img) cb->add_image(rvg, &r);
#endif
		}

		void render_impl(TextNode& t) {
#ifndef OVG_STUB_TYPES
			text_st_t ts{};
			ts.pos = { float(t.box.x),float(t.box.y) };
			ts.size = { float(t.box.z),float(t.box.w) };
			ts.text = t.text.c_str();
			ts.text_len = (int)t.text.size();
			text_style_t style{};
			// font_familys_t* 由字体名+样式在运行时解析，此处留空需上层填充
			style.family = nullptr;
			style.fontsize = t.fontsize;
			style.lineheight = t.lineheight;
			style.align = t.align;
			style.shadow_pos = t.shadow_offset;
			style.stroke = t.stroke_width;
			style.color = t.color;
			style.color_stroke = t.stroke_color;
			style.color_shadow = t.shadow_color;
			style.min_subpixel = t.min_subpixel;
			text_box_rt box{};
			box.rc = t.box;
			box.text_align = t.box_align;
			box.auto_break = t.auto_break;
			box.word_wrap = t.word_wrap;
			box.ellipsis = t.ellipsis;
			cb->add_text(rvg, &ts, &style, &box);
#endif
		}

		void render_impl(GeometryNode& g) {
			if (g.vertices.empty()) return;
			gem_info_t info{};
			info.blendMode = (int8_t)parse_blend(g.blend);
			info.topology = (g.mode == "triangle_strip") ? 2 : (g.mode == "lines") ? 3 : (g.mode == "line_strip") ? 4 : (g.mode == "points") ? 5 : 0;
			info.cullMode = (g.cull == "front") ? 1 : (g.cull == "back") ? 2 : (g.cull == "both") ? 3 : 0;
			info.frontFace = 0;
			info.shader = g.double_sided ? ST_DOUBLESIDED : ST_NONE;
			cb->set_geom_state(rvg, &info, nullptr);

			if (!g.instances.empty())
				cb->set_instance_mat(rvg, g.instances[0].data(), (uint32_t)g.instances.size());

			std::vector<float> xy, uv;
			std::vector<uint32_t> col;
			for (auto& v : g.vertices) {
				xy.insert(xy.end(), { v.pos.x,v.pos.y,v.pos.z });
				uv.insert(uv.end(), { v.uv.x,v.uv.y });
				col.push_back(v.color);
			}
			cb->add_geometry3d(rvg, nullptr,
				xy.data(), 3 * sizeof(float),
				col.data(), sizeof(uint32_t),
				uv.data(), 2 * sizeof(float),
				(int)g.vertices.size(),
				g.indices.empty() ? nullptr : g.indices.data(),
				(int)g.indices.size(),
				sizeof(uint32_t), 1);
		}

		void render_impl(std::shared_ptr<GroupNode> g) {
			// 变换/裁剪上下文
			bool has_xform = !(g->transform[0] == 1 && g->transform[1] == 0 && g->transform[2] == 0 &&
				g->transform[3] == 1 && g->transform[4] == 0 && g->transform[5] == 0);
			if (has_xform) cb->transform(rvg, g->transform.data());
			bool has_clip = g->clip_rect.z > 0;
			if (has_clip) cb->clip_rect(rvg, g->clip_rect.x, g->clip_rect.y, g->clip_rect.z, g->clip_rect.w);
			if (g->opacity != 1.f) cb->set_opacity(rvg, g->opacity);

			for (auto& c : g->children) render_node(c);

			if (g->opacity != 1.f) cb->set_opacity(rvg, 1.f);
			if (has_clip) cb->reset_clip(rvg, 0);
			if (has_xform) cb->identity_matrix(rvg);
		}

	private:
		void apply_pattern_ref(const std::string& ref) {
			if (ref.empty()) return;
			if (ref[0] == '@') {
				std::string key = ref.substr(1);
				auto it = scene->resources.find(key);
				if (it != scene->resources.end() && std::holds_alternative<uint32_t>(it->second))
					apply_solid(std::get<uint32_t>(it->second));
				else if (auto* pat = get_pattern(rvg, ref))
					cb->set_source(rvg, pat);
			}
		}
		void apply_stroke_style(const StrokeStyle& s) {
			cb->set_line_width(rvg, s.width);
			cb->set_line_cap(rvg, (int)s.cap);
			cb->set_line_join(rvg, (int)s.join);
			cb->set_miter_limit(rvg, s.miter_limit);
			if (!s.dash.empty()) cb->set_dash(rvg, s.dash.data(), (uint32_t)s.dash.size(), s.dash_offset);
			else cb->set_dash(rvg, nullptr, 0, 0);
		}
	};

	// ============================================================
	//  便利构造
	// ============================================================

	inline std::shared_ptr<GroupNode> make_rect(float x, float y, float w, float h,
		const std::string& fill, const std::string& stroke = "") {
		auto g = std::make_shared<GroupNode>();
		RectShape r; r.x = x; r.y = y; r.w = w; r.h = h; r.fill_ref = fill; r.stroke_ref = stroke;
		g->children.emplace_back(std::move(r));
		return g;
	}

	inline std::shared_ptr<GroupNode> make_text(const std::string& txt, float x, float y, float w, float h,
		const std::string& font, const std::string& style, float size) {
		auto g = std::make_shared<GroupNode>();
		TextNode t; t.text = txt; t.box = { (int)x,(int)y,(int)w,(int)h };
		t.font_family = font; t.font_style = style; t.fontsize = size;
		g->children.emplace_back(std::move(t));
		return g;
	}

} // namespace ovg
