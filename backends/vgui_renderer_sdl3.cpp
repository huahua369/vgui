/*
 * vgui_renderer_sdl3.cpp
 * vgui 的 SDL3 GPU 2D 矢量渲染后端实现。
 *
 * 设计目标：
 *   1. 只使用 SDL3 GPU API，不直接依赖 Vulkan/D3D12 调用；
 *   2. 消费 OVG 已经打包好的 ovg_draw_data_t，不修改 OVG 的绘制数据格式；
 *   3. 资源生命周期集中在本文件：设备、上下文、FBO、纹理、GPU 缓冲；
 *   4. 不申请 depth/stencil 附件，矩形裁剪使用 scissor。
 *
 * 与 third_party/ovg/ovg/ovg_renderer_sdl3.cpp 的差异：
 *   - 去掉了 3D、实例化、compute、路径模板裁剪和管线状态缓存中的
 *     depth/stencil 分支；
 *   - 保留 vgui 需要的填充/描边、渐变、文本图集、图片、矩形裁剪；
 *   - 使用独立前缀 vgpu_，不会和 OVG 自带渲染器产生符号冲突。
 */

#include <vgui/backend/vgui_renderer_sdl3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "shaders/spv_c/a_vg.vert.h"
#include "shaders/spv_c/a_vg.frag.h"
#include "shaders/spv_c/a_base3d.vert.h"
#include "shaders/spv_c/a_base3d.frag.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// 纹理句柄是公共头文件中的前置声明类型，必须定义在全局命名空间。
struct vgpu_texture_t {
	SDL_GPUTexture* texture = nullptr;
	SDL_GPUSampler* sampler = nullptr;
	SDL_GPUDevice* device = nullptr;
	SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
	int width = 0;
	int height = 0;
	uint32_t id = 0;
	// External textures are borrowed from the application; the renderer must
	// never release them.
	bool owns_texture = true;
};

namespace {

constexpr uint32_t FULLSCREEN_BIT = 0x10000000u;
constexpr uint32_t SRCTYPE_MASK = 0x000000FFu;

constexpr uint32_t OVG_VERTEX_SIZE = sizeof(ovgVertex);
constexpr uint32_t GEOM_VERTEX_SIZE = sizeof(geomVertex1);

inline size_t align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

inline SDL_GPUTextureFormat to_sdl_format(vg_format_t format)
{
	switch (format) {
	case VG_FORMAT_RGBA8:      return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	case VG_FORMAT_BGRA8:      return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	case VG_FORMAT_RGBA8_SRGB: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
	case VG_FORMAT_BGRA8_SRGB: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
	case VG_FORMAT_RGBA16F:    return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
	case VG_FORMAT_RGBA32F:    return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
	default:                   return SDL_GPU_TEXTUREFORMAT_INVALID;
	}
}

inline uint32_t format_bpp(vg_format_t format)
{
	switch (format) {
	case VG_FORMAT_RGBA16F: return 8;
	case VG_FORMAT_RGBA32F: return 16;
	default:                return 4;
	}
}

SDL_GPUShaderFormat detect_shader_format(SDL_GPUDevice* device)
{
	const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
	if (formats & SDL_GPU_SHADERFORMAT_SPIRV)
		return SDL_GPU_SHADERFORMAT_SPIRV;
	return SDL_GPU_SHADERFORMAT_INVALID;
}

SDL_GPUShader* compile_shader(SDL_GPUDevice* device, SDL_GPUShaderStage stage,
	const uint32_t* code, size_t code_size,
	int samplers, int uniform_buffers, int storage_buffers, int storage_textures)
{
	const SDL_GPUShaderFormat format = detect_shader_format(device);
	if (format == SDL_GPU_SHADERFORMAT_INVALID) {
		SDL_LogError(SDL_LOG_CATEGORY_GPU,
			"vgui sdl3 renderer requires SPIR-V shader support");
		return nullptr;
	}

	SDL_GPUShaderCreateInfo info = {};
	info.stage = stage;
	info.format = format;
	info.code = reinterpret_cast<const Uint8*>(code);
	info.code_size = code_size;
	info.entrypoint = "main";
	info.num_samplers = samplers;
	info.num_uniform_buffers = uniform_buffers;
	info.num_storage_buffers = storage_buffers;
	info.num_storage_textures = storage_textures;

	SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
	if (!shader) {
		SDL_LogError(SDL_LOG_CATEGORY_GPU, "vgui: failed to create %s shader: %s",
			stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vertex" : "fragment",
			SDL_GetError());
	}
	return shader;
}

// OVG 的默认混合。保留参考实现中的 normal_prem 分支，确保颜色结果不变。
void set_blend_state(SDL_GPUColorTargetBlendState& blend, int mode)
{
	blend = {};
	blend.enable_color_write_mask = true;
	blend.color_write_mask = SDL_GPU_COLORCOMPONENT_R |
		SDL_GPU_COLORCOMPONENT_G |
		SDL_GPU_COLORCOMPONENT_B |
		SDL_GPU_COLORCOMPONENT_A;

	switch (static_cast<blendMode_e>(mode)) {
	case blendMode_e::none:
		blend.enable_blend = false;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	case blendMode_e::normal:
		blend.enable_blend = true;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	case blendMode_e::additive:
		blend.enable_blend = true;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	case blendMode_e::multiply:
		blend.enable_blend = true;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	case blendMode_e::modulate:
		blend.enable_blend = true;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	case blendMode_e::screen:
		blend.enable_blend = true;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	case blendMode_e::additive_prem:
		blend.enable_blend = true;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	case blendMode_e::normal_prem:
	default:
		blend.enable_blend = true;
		blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		break;
	}
}

struct geom_pipeline_t {
	SDL_GPUGraphicsPipeline* pipeline = nullptr;
};

struct gpu_buffer_t {
	SDL_GPUBuffer* buffer = nullptr;
	size_t capacity = 0;

	void release(SDL_GPUDevice* device)
	{
		if (buffer && device)
			SDL_ReleaseGPUBuffer(device, buffer);
		buffer = nullptr;
		capacity = 0;
	}

	bool ensure(SDL_GPUDevice* device, size_t size, SDL_GPUBufferUsageFlags usage)
	{
		if (size == 0)
			return true;
		if (buffer && capacity >= size)
			return true;

		if (buffer) {
			// 仅在新旧缓冲尺寸跨越一帧时重建；等待 GPU 完成后释放旧句柄。
			SDL_WaitForGPUIdle(device);
			SDL_ReleaseGPUBuffer(device, buffer);
			buffer = nullptr;
			capacity = 0;
		}

		SDL_GPUBufferCreateInfo info = {};
		info.usage = usage;
		info.size = static_cast<Uint32>(align_up(size, 256));
		buffer = SDL_CreateGPUBuffer(device, &info);
		if (!buffer) {
			SDL_LogError(SDL_LOG_CATEGORY_GPU, "vgui: failed to create GPU buffer: %s", SDL_GetError());
			return false;
		}
		capacity = info.size;
		return true;
	}
};

} // namespace

struct vgpu_device_t {
	SDL_GPUDevice* gpu_device = nullptr;
	SDL_GPUShader* vg_vert = nullptr;
	SDL_GPUShader* vg_frag = nullptr;
	SDL_GPUShader* geom_vert = nullptr;
	SDL_GPUShader* geom_frag = nullptr;
	vgpu_texture_t* empty_texture = nullptr;
};

struct vgpu_ctx_t {
	vgpu_device_t* device = nullptr;
	SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;

	SDL_GPUGraphicsPipeline* vg_pipelines[4] = {};
	std::unordered_map<uint64_t, geom_pipeline_t> geom_pipelines;

	std::unordered_map<uint32_t, vgpu_texture_t*> textures;
	std::vector<vgpu_texture_t*> pending_destroy;
	uint32_t next_image_id = 1;

	gpu_buffer_t vbo;
	gpu_buffer_t ibo;

	SDL_GPUCommandBuffer* current_cmd = nullptr;
	SDL_GPURenderPass* pass = nullptr;
	bool swapchain_acquired = false;
};

namespace {

// ---------------------------------------------------------------------------
// 纹理与采样器
// ---------------------------------------------------------------------------

SDL_GPUSampler* create_sampler(SDL_GPUDevice* device)
{
	SDL_GPUSamplerCreateInfo info = {};
	info.min_filter = SDL_GPU_FILTER_NEAREST;
	info.mag_filter = SDL_GPU_FILTER_NEAREST;
	info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.max_anisotropy = 1.0f;
	return SDL_CreateGPUSampler(device, &info);
}

// 原生纹理默认采样器：linear + clamp。CPU 纹理保持 nearest，避免改变
// 现有 UI 图集的采样行为。
SDL_GPUSampler* create_sampler_linear(SDL_GPUDevice* device)
{
	SDL_GPUSamplerCreateInfo info = {};
	info.min_filter = SDL_GPU_FILTER_LINEAR;
	info.mag_filter = SDL_GPU_FILTER_LINEAR;
	info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.max_anisotropy = 1.0f;
	return SDL_CreateGPUSampler(device, &info);
}

vgpu_texture_t* create_texture(vgpu_device_t* device, int width, int height,
	SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
	SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1)
{
	if (!device || !device->gpu_device || width <= 0 || height <= 0 ||
		format == SDL_GPU_TEXTUREFORMAT_INVALID)
		return nullptr;

	auto* texture = new vgpu_texture_t();
	texture->device = device->gpu_device;
	texture->format = format;
	texture->width = width;
	texture->height = height;

	SDL_GPUTextureCreateInfo info = {};
	info.format = format;
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.width = static_cast<Uint32>(width);
	info.height = static_cast<Uint32>(height);
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = samples;
	info.usage = usage;
	texture->texture = SDL_CreateGPUTexture(device->gpu_device, &info);
	if (!texture->texture) {
		SDL_LogError(SDL_LOG_CATEGORY_GPU, "vgui: failed to create texture: %s", SDL_GetError());
		delete texture;
		return nullptr;
	}

	if (samples == SDL_GPU_SAMPLECOUNT_1 && (usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
		texture->sampler = create_sampler(device->gpu_device);
		if (!texture->sampler) {
			SDL_ReleaseGPUTexture(device->gpu_device, texture->texture);
			delete texture;
			return nullptr;
		}
	}
	return texture;
}

void destroy_texture(vgpu_texture_t* texture)
{
	if (!texture)
		return;
	if (texture->sampler)
		SDL_ReleaseGPUSampler(texture->device, texture->sampler);
	if (texture->texture && texture->owns_texture)
		SDL_ReleaseGPUTexture(texture->device, texture->texture);
	delete texture;
}

vgpu_texture_t* create_empty_texture(vgpu_device_t* device)
{
	auto* texture = create_texture(device, 1, 1,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (!texture)
		return nullptr;

	const uint32_t white = 0xFFFFFFFFu;
	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = sizeof(white);
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device->gpu_device, &transfer_info);
	if (!transfer) {
		destroy_texture(texture);
		return nullptr;
	}

	void* mapped = SDL_MapGPUTransferBuffer(device->gpu_device, transfer, false);
	if (!mapped) {
		SDL_ReleaseGPUTransferBuffer(device->gpu_device, transfer);
		destroy_texture(texture);
		return nullptr;
	}
	std::memcpy(mapped, &white, sizeof(white));
	SDL_UnmapGPUTransferBuffer(device->gpu_device, transfer);

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device->gpu_device);
	if (!cmd) {
		SDL_ReleaseGPUTransferBuffer(device->gpu_device, transfer);
		destroy_texture(texture);
		return nullptr;
	}
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	if (!copy) {
		SDL_CancelGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(device->gpu_device, transfer);
		destroy_texture(texture);
		return nullptr;
	}
	SDL_GPUTextureTransferInfo src = {};
	src.transfer_buffer = transfer;
	src.offset = 0;
	src.pixels_per_row = 1;
	src.rows_per_layer = 1;
	SDL_GPUTextureRegion dst = {};
	dst.texture = texture->texture;
	dst.w = 1;
	dst.h = 1;
	dst.d = 1;
	SDL_UploadToGPUTexture(copy, &src, &dst, false);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_WaitForGPUIdle(device->gpu_device);
	SDL_ReleaseGPUTransferBuffer(device->gpu_device, transfer);
	return texture;
}

// ---------------------------------------------------------------------------
// 管线
// ---------------------------------------------------------------------------

SDL_GPUGraphicsPipeline* create_pipeline(
	vgpu_device_t* device,
	SDL_GPUShader* vert, SDL_GPUShader* frag,
	SDL_GPUTextureFormat color_format,
	SDL_GPUSampleCount samples,
	SDL_GPUPrimitiveType topology,
	int blend_mode,
	uint32_t vertex_stride,
	const SDL_GPUVertexAttribute* attributes,
	uint32_t attribute_count,
	SDL_GPUFillMode fill_mode,
	SDL_GPUCullMode cull_mode,
	SDL_GPUFrontFace front_face)
{
	SDL_GPUColorTargetDescription color_target = {};
	color_target.format = color_format;
	set_blend_state(color_target.blend_state, blend_mode);

	SDL_GPUDepthStencilState depth_stencil = {};
	depth_stencil.enable_depth_test = false;
	depth_stencil.enable_depth_write = false;
	depth_stencil.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
	depth_stencil.enable_stencil_test = false;
	depth_stencil.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
	depth_stencil.front_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
	depth_stencil.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
	depth_stencil.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
	depth_stencil.back_stencil_state = depth_stencil.front_stencil_state;
	depth_stencil.compare_mask = 0;
	depth_stencil.write_mask = 0;

	SDL_GPURasterizerState rasterizer = {};
	rasterizer.fill_mode = fill_mode;
	rasterizer.cull_mode = cull_mode;
	rasterizer.front_face = front_face;
	rasterizer.enable_depth_bias = false;
	rasterizer.enable_depth_clip = false;

	SDL_GPUMultisampleState multisample = {};
	multisample.sample_count = samples;
	multisample.sample_mask = 0;

	SDL_GPUVertexBufferDescription vertex_buffer = {};
	vertex_buffer.slot = 0;
	vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vertex_buffer.pitch = vertex_stride;

	SDL_GPUVertexInputState vertex_input = {};
	vertex_input.num_vertex_buffers = 1;
	vertex_input.vertex_buffer_descriptions = &vertex_buffer;
	vertex_input.num_vertex_attributes = attribute_count;
	vertex_input.vertex_attributes = attributes;

	SDL_GPUGraphicsPipelineCreateInfo info = {};
	info.vertex_shader = vert;
	info.fragment_shader = frag;
	info.vertex_input_state = vertex_input;
	info.primitive_type = topology;
	info.rasterizer_state = rasterizer;
	info.multisample_state = multisample;
	info.depth_stencil_state = depth_stencil;
	info.target_info.has_depth_stencil_target = false;
	info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
	info.target_info.num_color_targets = 1;
	info.target_info.color_target_descriptions = &color_target;

	SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device->gpu_device, &info);
	if (!pipeline) {
		SDL_LogError(SDL_LOG_CATEGORY_GPU, "vgui: failed to create graphics pipeline: %s", SDL_GetError());
	}
	return pipeline;
}

void create_vg_pipelines(vgpu_ctx_t* ctx)
{
	static const SDL_GPUVertexAttribute attributes[] = {
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 },
	};

	for (int op = 0; op < 4; ++op) {
		// 只有 CLEAR 使用无混合；其余操作保持参考实现的预乘 OVER 结果。
		const int blend = (op == static_cast<int>(vg_operator_t::VG_OPERATOR_CLEAR))
			? static_cast<int>(blendMode_e::none)
			: static_cast<int>(blendMode_e::normal_prem);
		ctx->vg_pipelines[op] = create_pipeline(
			ctx->device, ctx->device->vg_vert, ctx->device->vg_frag,
			ctx->color_format, ctx->samples,
			SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, blend,
			OVG_VERTEX_SIZE, attributes, 3,
			SDL_GPU_FILLMODE_FILL, SDL_GPU_CULLMODE_NONE,
			SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE);
	}
}

SDL_GPUGraphicsPipeline* get_vg_pipeline(vgpu_ctx_t* ctx, int op)
{
	if (op < 0 || op >= 4)
		op = static_cast<int>(vg_operator_t::VG_OPERATOR_OVER);
	if (!ctx->vg_pipelines[op])
		return ctx->vg_pipelines[static_cast<int>(vg_operator_t::VG_OPERATOR_OVER)];
	return ctx->vg_pipelines[op];
}

uint64_t make_geom_key(const gem_info_t* info)
{
	// 只把真正影响管线状态的字段放进 key，避免 flags/lineWidth 造成无谓的
	// 管线变体。shader 目前只用 a_base3d，但保留在 key 中便于以后扩展。
	uint64_t key = static_cast<uint8_t>(info->blendMode);
	key |= static_cast<uint64_t>(info->topology) << 8;
	key |= static_cast<uint64_t>(info->polygon) << 16;
	key |= static_cast<uint64_t>(info->frontFace) << 24;
	key |= static_cast<uint64_t>(info->cullMode) << 32;
	key |= static_cast<uint64_t>(info->shader) << 40;
	return key;
}

SDL_GPUGraphicsPipeline* create_geom_pipeline(vgpu_ctx_t* ctx, const gem_info_t* info)
{
	static const SDL_GPUVertexAttribute attributes[] = {
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12 },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20 },
	};

	SDL_GPUPrimitiveType topology = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	if (info->topology <= SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP)
		topology = static_cast<SDL_GPUPrimitiveType>(info->topology);

	SDL_GPUFillMode fill = info->polygon == 1 ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
	SDL_GPUFrontFace front = info->frontFace ? SDL_GPU_FRONTFACE_CLOCKWISE : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	SDL_GPUCullMode cull = SDL_GPU_CULLMODE_NONE;
	switch (info->cullMode) {
	case 1: cull = SDL_GPU_CULLMODE_FRONT; break;
	case 2: cull = SDL_GPU_CULLMODE_BACK; break;
	case 3: cull = SDL_GPU_CULLMODE_BACK; break;
	default: break;
	}

	return create_pipeline(
		ctx->device, ctx->device->geom_vert, ctx->device->geom_frag,
		ctx->color_format, ctx->samples,
		topology, info->blendMode,
		GEOM_VERTEX_SIZE, attributes, 3,
		fill, cull, front);
}

SDL_GPUGraphicsPipeline* get_geom_pipeline(vgpu_ctx_t* ctx, const gem_info_t* info)
{
	const uint64_t key = make_geom_key(info);
	auto it = ctx->geom_pipelines.find(key);
	if (it != ctx->geom_pipelines.end())
		return it->second.pipeline;

	SDL_GPUGraphicsPipeline* pipeline = create_geom_pipeline(ctx, info);
	ctx->geom_pipelines.emplace(key, geom_pipeline_t{ pipeline });
	return pipeline;
}

// ---------------------------------------------------------------------------
// 上传
// ---------------------------------------------------------------------------

void process_image_descs(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd,
	const std::vector<ovg_draw_data_t*>& items,
	std::vector<SDL_GPUTransferBuffer*>& transfers)
{
	SDL_GPUCopyPass* copy = nullptr;

	for (ovg_draw_data_t* data : items) {
		if (!data)
			continue;

		for (size_t i = 0; i < data->image_desc_count; ++i) {
			vg_image_desc_t* desc = data->image_desc[i];
			if (!desc || !desc->img)
				continue;

			vgpu_texture_t* texture = nullptr;
			const uint32_t old_id = desc->img->id;
			auto found = ctx->textures.find(old_id);
			if (found != ctx->textures.end())
				texture = found->second;

			if (desc->is_destroy) {
				if (texture && texture != ctx->device->empty_texture) {
					ctx->pending_destroy.push_back(texture);
					ctx->textures.erase(old_id);
				}
				desc->img->id = 0;
				continue;
			}

			const SDL_GPUTextureFormat format = to_sdl_format(static_cast<vg_format_t>(desc->format));
			if (format == SDL_GPU_TEXTUREFORMAT_INVALID || desc->width == 0 || desc->height == 0)
				continue;

			const bool needs_new = !texture ||
				texture == ctx->device->empty_texture ||
				texture->width != static_cast<int>(desc->width) ||
				texture->height != static_cast<int>(desc->height) ||
				texture->format != format;

			if (needs_new) {
				if (texture && texture != ctx->device->empty_texture) {
					ctx->pending_destroy.push_back(texture);
					ctx->textures.erase(old_id);
				}

				uint32_t id = old_id;
				if (id == 0) {
					id = ctx->next_image_id++;
					if (id == 0)
						id = ctx->next_image_id++;
				}
				texture = create_texture(ctx->device,
					static_cast<int>(desc->width), static_cast<int>(desc->height),
					format, SDL_GPU_TEXTUREUSAGE_SAMPLER);
				if (!texture) {
					desc->img->id = 0;
					continue;
				}
				texture->id = id;
				desc->img->id = id;
				ctx->textures[id] = texture;
			}

			if (!desc->pixels || !texture)
				continue;

			const uint32_t bpp = format_bpp(static_cast<vg_format_t>(desc->format));
			const uint32_t stride = desc->stride ? desc->stride : desc->width * bpp;
			const uint32_t x = (std::min)(desc->x, desc->width);
			const uint32_t y = (std::min)(desc->y, desc->height);
			uint32_t w = desc->w ? desc->w : desc->width;
			uint32_t h = desc->h ? desc->h : desc->height;
			w = (std::min)(w, desc->width - x);
			h = (std::min)(h, desc->height - y);
			if (w == 0 || h == 0 || stride < w * bpp)
				continue;

			const size_t row_bytes = static_cast<size_t>(w) * bpp;
			const size_t total_bytes = row_bytes * h;
			if (total_bytes == 0)
				continue;

			SDL_GPUTransferBufferCreateInfo transfer_info = {};
			transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transfer_info.size = static_cast<Uint32>(total_bytes);
			SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(ctx->device->gpu_device, &transfer_info);
			if (!transfer)
				continue;

			void* mapped = SDL_MapGPUTransferBuffer(ctx->device->gpu_device, transfer, false);
			if (!mapped) {
				SDL_ReleaseGPUTransferBuffer(ctx->device->gpu_device, transfer);
				continue;
			}

			const auto* source = static_cast<const uint8_t*>(desc->pixels) +
				static_cast<size_t>(y) * stride + static_cast<size_t>(x) * bpp;
			auto* destination = static_cast<uint8_t*>(mapped);
			for (uint32_t row = 0; row < h; ++row) {
				std::memcpy(destination + static_cast<size_t>(row) * row_bytes,
					source + static_cast<size_t>(row) * stride, row_bytes);
			}
			SDL_UnmapGPUTransferBuffer(ctx->device->gpu_device, transfer);

			if (!copy) {
				copy = SDL_BeginGPUCopyPass(cmd);
				if (!copy) {
					SDL_ReleaseGPUTransferBuffer(ctx->device->gpu_device, transfer);
					continue;
				}
			}

			SDL_GPUTextureTransferInfo src = {};
			src.transfer_buffer = transfer;
			src.offset = 0;
			src.pixels_per_row = w;
			src.rows_per_layer = h;

			SDL_GPUTextureRegion dst = {};
			dst.texture = texture->texture;
			dst.x = x;
			dst.y = y;
			dst.w = w;
			dst.h = h;
			dst.d = 1;
			SDL_UploadToGPUTexture(copy, &src, &dst, false);
			transfers.push_back(transfer);
			desc->img->valid = false;
		}
	}

	if (copy)
		SDL_EndGPUCopyPass(copy);
}

void upload_buffers(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd,
	const std::vector<ovg_draw_data_t*>& items,
	std::vector<SDL_GPUTransferBuffer*>& transfers)
{
	size_t vbo_bytes = 0;
	size_t ibo_bytes = 0;

	for (ovg_draw_data_t* data : items) {
		if (!data)
			continue;
		data->vg_offset.x = static_cast<uint32_t>(vbo_bytes);
		vbo_bytes += data->v_count * sizeof(ovgVertex);
		vbo_bytes = align_up(vbo_bytes, 4);

		data->vg_offset.y = static_cast<uint32_t>(ibo_bytes);
		ibo_bytes += data->i_count * sizeof(uint32_t);
		ibo_bytes = align_up(ibo_bytes, 4);

		data->geom_offset.x = static_cast<uint32_t>(vbo_bytes);
		vbo_bytes += data->v1_count * sizeof(geomVertex1);
		vbo_bytes = align_up(vbo_bytes, 4);

		// v2 在本后端不绘制，但仍保留偏移，避免数据布局和 OVG 不一致。
		data->geom_offset.y = static_cast<uint32_t>(vbo_bytes);
		vbo_bytes += data->v2_count * sizeof(geomVertex2);
		vbo_bytes = align_up(vbo_bytes, 4);

		data->geom_offset.z = static_cast<uint32_t>(ibo_bytes);
		ibo_bytes += data->ig_count * sizeof(uint32_t);
		ibo_bytes = align_up(ibo_bytes, 4);
	}

	if (!ctx->vbo.ensure(ctx->device->gpu_device, vbo_bytes, SDL_GPU_BUFFERUSAGE_VERTEX))
		return;
	if (!ctx->ibo.ensure(ctx->device->gpu_device, ibo_bytes, SDL_GPU_BUFFERUSAGE_INDEX))
		return;
	if (vbo_bytes == 0 && ibo_bytes == 0)
		return;

	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = static_cast<Uint32>(vbo_bytes + ibo_bytes);
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(ctx->device->gpu_device, &transfer_info);
	if (!transfer)
		return;

	void* mapped = SDL_MapGPUTransferBuffer(ctx->device->gpu_device, transfer, false);
	if (!mapped) {
		SDL_ReleaseGPUTransferBuffer(ctx->device->gpu_device, transfer);
		return;
	}

	auto* base = static_cast<uint8_t*>(mapped);
	for (ovg_draw_data_t* data : items) {
		if (!data)
			continue;
		if (data->v_count && data->vg_vertex) {
			std::memcpy(base + data->vg_offset.x, data->vg_vertex,
				data->v_count * sizeof(ovgVertex));
		}
		if (data->i_count && data->vg_indices) {
			std::memcpy(base + vbo_bytes + data->vg_offset.y, data->vg_indices,
				data->i_count * sizeof(uint32_t));
		}
		if (data->v1_count && data->vertex1) {
			std::memcpy(base + data->geom_offset.x, data->vertex1,
				data->v1_count * sizeof(geomVertex1));
		}
		if (data->v2_count && data->vertex2) {
			std::memcpy(base + data->geom_offset.y, data->vertex2,
				data->v2_count * sizeof(geomVertex2));
		}
		if (data->ig_count && data->geom_indices) {
			std::memcpy(base + vbo_bytes + data->geom_offset.z, data->geom_indices,
				data->ig_count * sizeof(uint32_t));
		}
	}
	SDL_UnmapGPUTransferBuffer(ctx->device->gpu_device, transfer);

	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	if (!copy) {
		SDL_ReleaseGPUTransferBuffer(ctx->device->gpu_device, transfer);
		return;
	}
	if (vbo_bytes > 0) {
		SDL_GPUTransferBufferLocation src = {};
		src.transfer_buffer = transfer;
		src.offset = 0;
		SDL_GPUBufferRegion dst = {};
		dst.buffer = ctx->vbo.buffer;
		dst.offset = 0;
		dst.size = static_cast<Uint32>(vbo_bytes);
		SDL_UploadToGPUBuffer(copy, &src, &dst, false);
	}
	if (ibo_bytes > 0) {
		SDL_GPUTransferBufferLocation src = {};
		src.transfer_buffer = transfer;
		src.offset = static_cast<Uint32>(vbo_bytes);
		SDL_GPUBufferRegion dst = {};
		dst.buffer = ctx->ibo.buffer;
		dst.offset = 0;
		dst.size = static_cast<Uint32>(ibo_bytes);
		SDL_UploadToGPUBuffer(copy, &src, &dst, false);
	}
	SDL_EndGPUCopyPass(copy);
	transfers.push_back(transfer);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void bind_vertex_buffer(SDL_GPURenderPass* pass, gpu_buffer_t& buffer, uint32_t offset)
{
	if (!buffer.buffer)
		return;
	SDL_GPUBufferBinding binding = {};
	binding.buffer = buffer.buffer;
	binding.offset = offset;
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
}

void bind_index_buffer(SDL_GPURenderPass* pass, gpu_buffer_t& buffer, uint32_t offset)
{
	if (!buffer.buffer)
		return;
	SDL_GPUBufferBinding binding = {};
	binding.buffer = buffer.buffer;
	binding.offset = offset;
	SDL_BindGPUIndexBuffer(pass, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

void apply_scissor(vg_fbo_t* fbo, SDL_GPURenderPass* pass,
	const glm::vec4* rect, SDL_Rect* current)
{
	SDL_Rect result = { 0, 0, static_cast<int>(fbo->width), static_cast<int>(fbo->height) };
	if (rect) {
		int x = static_cast<int>(rect->x);
		int y = static_cast<int>(rect->y);
		int w = static_cast<int>(rect->z);
		int h = static_cast<int>(rect->w);
		if (w < 0 || h < 0) {
			result = { 0, 0, static_cast<int>(fbo->width), static_cast<int>(fbo->height) };
		}
		else {
			const int max_x = (std::max)(static_cast<int>(fbo->width) - 1, 0);
			const int max_y = (std::max)(static_cast<int>(fbo->height) - 1, 0);
			int x0 = (std::min)((std::max)(x, 0), max_x);
			int y0 = (std::min)((std::max)(y, 0), max_y);
			int x1 = (std::clamp)(x + w, x0 + 1, static_cast<int>(fbo->width));
			int y1 = (std::clamp)(y + h, y0 + 1, static_cast<int>(fbo->height));
			result.x = x0;
			result.y = y0;
			result.w = (std::max)(x1 - x0, 1);
			result.h = (std::max)(y1 - y0, 1);
		}
	}
	SDL_SetGPUScissor(pass, &result);
	if (current)
		*current = result;
}

void draw_ct(SDL_GPURenderPass* pass, vgcmd_t* cmd)
{
	if (cmd->index.y > 0) {
		SDL_DrawGPUIndexedPrimitives(pass, static_cast<Uint32>(cmd->index.y), 1,
			static_cast<Uint32>(cmd->index.x), static_cast<int32_t>(cmd->vertex.x), 0);
	}
	else {
		SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(cmd->vertex.y), 1,
			static_cast<Uint32>(cmd->vertex.x), 0);
	}
}

void sort_gradient_stops(vg_gradient_t* gradient, float* stops, uint32_t count)
{
	auto* colors = gradient->colors;
	for (uint32_t i = 1; i < count; ++i) {
		const float key_stop = stops[i];
		const glm::vec4 key_color = colors[i];
		int j = static_cast<int>(i) - 1;
		while (j >= 0 && stops[j] > key_stop) {
			stops[j + 1] = stops[j];
			colors[j + 1] = colors[j];
			--j;
		}
		stops[j + 1] = key_stop;
		colors[j + 1] = key_color;
	}
}

void transform_gradient(vg_gradient_t* gradient, int type,
	const glm::mat3& mat, const glm::mat3& pattern_mat)
{
	glm::vec3 cp0[2] = {
		glm::vec3(gradient->cp[0].x, gradient->cp[0].y, 1.0f),
		glm::vec3(gradient->cp[0].z, gradient->cp[0].w, 1.0f)
	};
	glm::vec3 cp1[2] = {
		glm::vec3(gradient->cp[1].x, gradient->cp[1].y, 1.0f),
		glm::vec3(gradient->cp[1].z, gradient->cp[1].w, 1.0f)
	};

	const glm::mat3 m = mat * glm::inverse(pattern_mat);
	cp0[0] = m * cp0[0];
	if (type == static_cast<int>(vg_pattern_type_t::VG_PATTERN_TYPE_LINEAR)) {
		cp0[1] = m * cp0[1];
	}
	else {
		cp1[0] = m * cp1[0];
		cp0[1].z = 0.0f;
		cp0[1] = m * cp0[1];
		cp1[1].z = 0.0f;
		cp1[1] = m * cp1[1];
	}

	gradient->cp[0] = glm::vec4(glm::vec2(cp0[0]), glm::vec2(cp0[1]));
	gradient->cp[1] = glm::vec4(glm::vec2(cp1[0]), glm::vec2(cp1[1]));
	sort_gradient_stops(gradient, gradient->stops, gradient->count);
}

void draw_vg(vg_fbo_t* fbo, SDL_GPURenderPass* pass, vgcmd_t* cmd,
	SDL_Rect* current_scissor, const glm::uvec2& offset)
{
	if (!cmd || !fbo || !fbo->ctx)
		return;

	vgpu_ctx_t* ctx = fbo->ctx;
	bind_vertex_buffer(pass, ctx->vbo, offset.x);
	bind_index_buffer(pass, ctx->ibo, offset.y);

	vg_state_save_t* state = cmd->state;
	push_constants_t pc = {};
	vg_gradient_t gradient = {};
	SDL_GPUTextureSamplerBinding binding = {
		fbo->ctx->device->empty_texture->texture,
		fbo->ctx->device->empty_texture->sampler
	};

	if (state) {
		pc = state->pushConsts;
		pc.size = { static_cast<float>(fbo->width), static_cast<float>(fbo->height) };
		glm::mat3x3 inverse_matrix = pc.mat;
		pc.matInv = glm::inverse(inverse_matrix);

		if (state->pattern && state->pattern->type != vg_pattern_type_t::VG_PATTERN_TYPE_SOLID) {
			const float smax = static_cast<float>((std::max)(fbo->width, fbo->height));
			pc.source = { smax, smax, 0.0f, 0.0f };

			if (state->pattern->type == vg_pattern_type_t::VG_PATTERN_TYPE_SURFACE) {
				auto* texture = static_cast<vgpu_texture_t*>(state->pattern->data);
				if (texture && texture->texture) {
					binding.texture = texture->texture;
					binding.sampler = texture->sampler ? texture->sampler : fbo->ctx->device->empty_texture->sampler;
					pc.source.z = static_cast<float>(fbo->width);
					pc.source.w = static_cast<float>(fbo->height);
					if (state->pattern->hasMatrix)
						pc.matInv = pc.matInv * glm::mat3(state->pattern->matrix);
				}
			}
		}

		if (state->pattern && state->pattern->data &&
			state->pattern->type != vg_pattern_type_t::VG_PATTERN_TYPE_SURFACE) {
			gradient = *static_cast<vg_gradient_t*>(state->pattern->data);
			glm::mat3 pattern_matrix = glm::mat3(1.0f);
			if (state->pattern->hasMatrix)
				pattern_matrix = state->pattern->matrix;
			transform_gradient(&gradient, static_cast<int>(state->pattern->type),
				pc.mat, pattern_matrix);
		}

		const uint32_t pattern_type = state->pattern
			? static_cast<uint32_t>(state->pattern->type)
			: static_cast<uint32_t>(vg_pattern_type_t::VG_PATTERN_TYPE_SOLID);
		pc.fsq_patternType = (pc.fsq_patternType & FULLSCREEN_BIT) | (pattern_type & SRCTYPE_MASK);
	}

	SDL_BindGPUGraphicsPipeline(pass, get_vg_pipeline(ctx,
		state ? static_cast<int>(state->curOperator) : static_cast<int>(vg_operator_t::VG_OPERATOR_OVER)));
	SDL_PushGPUVertexUniformData(fbo->cmd, 0, &pc, sizeof(pc));
	SDL_PushGPUFragmentUniformData(fbo->cmd, 0, &gradient, sizeof(gradient));
	SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

	switch (cmd->type) {
	case 0:
	case 1:
		draw_ct(pass, cmd);
		break;
	case 2: {
		const int w = static_cast<int>(cmd->bounds.z);
		const int h = static_cast<int>(cmd->bounds.w);
		if (w != 0 && h != 0) {
			apply_scissor(fbo, pass, &cmd->bounds, current_scissor);
		}
		// bounds 为 0 且存在几何数据时，是路径模板裁剪。本后端不支持，
		// vgui 不使用该路径；保留 scissor 状态即可。
		break;
	}
	case 3: {
		pc.fsq_patternType |= FULLSCREEN_BIT;
		SDL_PushGPUVertexUniformData(fbo->cmd, 0, &pc, sizeof(pc));
		SDL_DrawGPUPrimitives(pass, 3, 1, static_cast<Uint32>(cmd->full_screen_quad), 0);
		pc.fsq_patternType &= ~FULLSCREEN_BIT;
		SDL_PushGPUVertexUniformData(fbo->cmd, 0, &pc, sizeof(pc));
		break;
	}
	case 4:
		// clear 由 render pass 的 LOADOP_CLEAR 负责。
		break;
	default:
		break;
	}
}

void draw_geom(vg_fbo_t* fbo, SDL_GPURenderPass* pass, geom_cmd_t* cmd,
	const glm::uvec3& offset)
{
	if (!cmd || !fbo || !fbo->ctx)
		return;

	vgpu_ctx_t* ctx = fbo->ctx;
	if (cmd->v_offset != 0 || cmd->texture_mask || cmd->instance_count > 1) {
		// 双面、蒙版和实例化需要参考实现中的额外管线；vgui 不使用。
		return;
	}

	SDL_GPUGraphicsPipeline* pipeline = get_geom_pipeline(ctx, &cmd->state);
	if (!pipeline)
		return;

	SDL_BindGPUGraphicsPipeline(pass, pipeline);
	bind_vertex_buffer(pass, ctx->vbo, offset.x);
	bind_index_buffer(pass, ctx->ibo, offset.z);

	SDL_GPUTextureSamplerBinding binding = {
		ctx->device->empty_texture->texture,
		ctx->device->empty_texture->sampler
	};
	if (cmd->texture) {
		auto* image = static_cast<vg_image_t*>(cmd->texture);
		auto it = ctx->textures.find(image->id);
		if (it != ctx->textures.end() && it->second->texture) {
			binding.texture = it->second->texture;
			binding.sampler = it->second->sampler ? it->second->sampler : ctx->device->empty_texture->sampler;
		}
	}
	SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

	struct PushConstants {
		glm::mat4 mvp;
		uint32_t instance_pos;
	};
	PushConstants pc = { cmd->mat, cmd->instance_ssbo_pos };
	SDL_PushGPUVertexUniformData(fbo->cmd, 0, &pc, sizeof(pc));

	if (cmd->firstIndex >= 0) {
		SDL_DrawGPUIndexedPrimitives(pass, cmd->elemCount, 1,
			static_cast<Uint32>(cmd->firstIndex), cmd->vertexOffset, 0);
	}
	else {
		SDL_DrawGPUPrimitives(pass, cmd->elemCount, 1, cmd->vertexOffset, 0);
	}
}

// ---------------------------------------------------------------------------
// FBO / pass
// ---------------------------------------------------------------------------

void begin_pass(vgpu_ctx_t* ctx, vg_fbo_t* fbo, bool clear)
{
	if (!ctx || !fbo || !ctx->current_cmd)
		return;

	SDL_GPUTexture* target = fbo->swapchain;
	if (!target && fbo->color_tex)
		target = fbo->color_tex->texture;
	if (!target)
		return;

	SDL_GPUTexture* render_target = fbo->color_msaa_tex
		? fbo->color_msaa_tex->texture
		: target;

	SDL_GPUColorTargetInfo color_info = {};
	color_info.texture = render_target;
	color_info.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	color_info.store_op = fbo->color_msaa_tex ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE;
	color_info.clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (fbo->color_msaa_tex) {
		color_info.resolve_texture = target;
		color_info.resolve_mip_level = 0;
		color_info.resolve_layer = 0;
	}

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(ctx->current_cmd, &color_info, 1, nullptr);
	if (!pass)
		return;

	fbo->cmd = ctx->current_cmd;
	ctx->pass = pass;

	SDL_GPUViewport viewport = {};
	viewport.x = 0.0f;
	viewport.y = static_cast<float>(fbo->height);
	viewport.w = static_cast<float>(fbo->width);
	viewport.h = -static_cast<float>(fbo->height);
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &viewport);

	SDL_Rect scissor = { 0, 0, static_cast<int>(fbo->width), static_cast<int>(fbo->height) };
	SDL_SetGPUScissor(pass, &scissor);
}

void end_pass(vgpu_ctx_t* ctx, vg_fbo_t* fbo)
{
	if (ctx && ctx->pass) {
		SDL_EndGPURenderPass(ctx->pass);
		ctx->pass = nullptr;
	}
	if (fbo)
		fbo->cmd = nullptr;
}

bool ensure_swapchain(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd, vg_fbo_t* fbo, bool wait)
{
	if (!fbo->window)
		return fbo->color_tex != nullptr;

	if (fbo->swapchain) {
		ctx->swapchain_acquired = true;
		return true;
	}

	SDL_GPUTexture* swapchain = nullptr;
	Uint32 width = 0;
	Uint32 height = 0;
	const bool ok = wait
		? SDL_WaitAndAcquireGPUSwapchainTexture(cmd, fbo->window, &swapchain, &width, &height)
		: SDL_AcquireGPUSwapchainTexture(cmd, fbo->window, &swapchain, &width, &height);
	if (!ok || !swapchain || width == 0 || height == 0)
		return false;

	if (width != fbo->width || height != fbo->height) {
		SDL_WaitForGPUSwapchain(ctx->device->gpu_device, fbo->window);
		vgpu_reset_fbo(fbo, static_cast<int>(width), static_cast<int>(height));
	}
	fbo->swapchain = swapchain;
	fbo->display_size = { static_cast<int>(width), static_cast<int>(height) };
	ctx->swapchain_acquired = true;
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------

vgpu_device_t* vgpu_create_device(SDL_GPUDevice* gpu_device)
{
	if (!gpu_device)
		return nullptr;

	auto* device = new vgpu_device_t();
	device->gpu_device = gpu_device;

	device->vg_vert = compile_shader(gpu_device, SDL_GPU_SHADERSTAGE_VERTEX,
		vg_vert, sizeof(vg_vert), 0, 1, 0, 0);
	device->vg_frag = compile_shader(gpu_device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		vg_frag, sizeof(vg_frag), 1, 1, 0, 0);
	device->geom_vert = compile_shader(gpu_device, SDL_GPU_SHADERSTAGE_VERTEX,
		a_base3d_vert, sizeof(a_base3d_vert), 0, 1, 0, 0);
	device->geom_frag = compile_shader(gpu_device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		a_base3d_frag, sizeof(a_base3d_frag), 1, 0, 0, 0);

	if (!device->vg_vert || !device->vg_frag || !device->geom_vert || !device->geom_frag) {
		vgpu_destroy_device(device);
		return nullptr;
	}

	device->empty_texture = create_empty_texture(device);
	if (!device->empty_texture) {
		vgpu_destroy_device(device);
		return nullptr;
	}
	return device;
}

void vgpu_destroy_device(vgpu_device_t* device)
{
	if (!device)
		return;
	if (device->gpu_device)
		SDL_WaitForGPUIdle(device->gpu_device);
	if (device->empty_texture) {
		destroy_texture(device->empty_texture);
		device->empty_texture = nullptr;
	}
	if (device->gpu_device) {
		if (device->vg_vert)
			SDL_ReleaseGPUShader(device->gpu_device, device->vg_vert);
		if (device->vg_frag)
			SDL_ReleaseGPUShader(device->gpu_device, device->vg_frag);
		if (device->geom_vert)
			SDL_ReleaseGPUShader(device->gpu_device, device->geom_vert);
		if (device->geom_frag)
			SDL_ReleaseGPUShader(device->gpu_device, device->geom_frag);
	}
	delete device;
}

vgpu_ctx_t* vgpu_create_context(vgpu_device_t* device,
	SDL_GPUTextureFormat color_format,
	SDL_GPUTextureFormat depth_format,
	SDL_GPUSampleCount samples)
{
	(void)depth_format;
	if (!device || !device->gpu_device)
		return nullptr;

	if (color_format == SDL_GPU_TEXTUREFORMAT_INVALID)
		color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

	auto* ctx = new vgpu_ctx_t();
	ctx->device = device;
	ctx->color_format = color_format;

	ctx->samples = SDL_GPU_SAMPLECOUNT_1;
	if (samples > SDL_GPU_SAMPLECOUNT_1) {
		static constexpr SDL_GPUSampleCount candidates[] = {
			SDL_GPU_SAMPLECOUNT_8,
			SDL_GPU_SAMPLECOUNT_4,
			SDL_GPU_SAMPLECOUNT_2,
		};
		for (SDL_GPUSampleCount candidate : candidates) {
			if (candidate <= samples &&
				SDL_GPUTextureSupportsSampleCount(device->gpu_device, color_format, candidate)) {
				ctx->samples = candidate;
				break;
			}
		}
	}

	create_vg_pipelines(ctx);
	for (SDL_GPUGraphicsPipeline* pipeline : ctx->vg_pipelines) {
		if (!pipeline) {
			vgpu_destroy_context(ctx);
			return nullptr;
		}
	}
	ctx->textures[0] = device->empty_texture;
	return ctx;
}

void vgpu_destroy_context(vgpu_ctx_t* ctx)
{
	if (!ctx)
		return;

	SDL_GPUDevice* device = ctx->device ? ctx->device->gpu_device : nullptr;
	if (device)
		SDL_WaitForGPUIdle(device);

	if (ctx->pass) {
		SDL_EndGPURenderPass(ctx->pass);
		ctx->pass = nullptr;
	}
	if (ctx->current_cmd && device) {
		if (ctx->swapchain_acquired)
			SDL_SubmitGPUCommandBuffer(ctx->current_cmd);
		else
			SDL_CancelGPUCommandBuffer(ctx->current_cmd);
		ctx->current_cmd = nullptr;
	}
	ctx->swapchain_acquired = false;

	for (SDL_GPUGraphicsPipeline* pipeline : ctx->vg_pipelines) {
		if (pipeline && device)
			SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
	}
	for (auto& [key, pipeline] : ctx->geom_pipelines) {
		if (pipeline.pipeline && device)
			SDL_ReleaseGPUGraphicsPipeline(device, pipeline.pipeline);
	}

	vgpu_texture_t* empty_texture = ctx->device ? ctx->device->empty_texture : nullptr;
	for (auto& [id, texture] : ctx->textures) {
		if (texture && texture != empty_texture)
			destroy_texture(texture);
	}
	for (vgpu_texture_t* texture : ctx->pending_destroy) {
		if (texture && texture != empty_texture)
			destroy_texture(texture);
	}

	ctx->vbo.release(device);
	ctx->ibo.release(device);
	delete ctx;
}

vg_fbo_t vgpu_create_fbo(vgpu_ctx_t* ctx, int width, int height, SDL_Window* window)
{
	vg_fbo_t fbo = {};
	fbo.ctx = ctx;
	fbo.window = window;
	fbo.width = static_cast<uint32_t>((std::max)(width, 1));
	fbo.height = static_cast<uint32_t>((std::max)(height, 1));
	fbo.display_size = { static_cast<int>(fbo.width), static_cast<int>(fbo.height) };
	fbo.upsize = true;

	if (!ctx || !ctx->device)
		return fbo;

	if (!window) {
		fbo.color_tex = create_texture(ctx->device,
			static_cast<int>(fbo.width), static_cast<int>(fbo.height),
			ctx->color_format,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	}
	if (ctx->samples != SDL_GPU_SAMPLECOUNT_1) {
		fbo.color_msaa_tex = create_texture(ctx->device,
			static_cast<int>(fbo.width), static_cast<int>(fbo.height),
			ctx->color_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, ctx->samples);
	}
	return fbo;
}

SDL_GPUTexture* vgpu_fbo_color_texture(vg_fbo_t* fbo)
{
	if (!fbo) return nullptr;
	if (fbo->swapchain) return fbo->swapchain;
	if (fbo->color_tex) return fbo->color_tex->texture;
	return nullptr;
}

void vgpu_destroy_fbo(vg_fbo_t* fbo)
{
	if (!fbo)
		return;
	if (fbo->color_tex) {
		destroy_texture(fbo->color_tex);
		fbo->color_tex = nullptr;
	}
	if (fbo->color_msaa_tex) {
		destroy_texture(fbo->color_msaa_tex);
		fbo->color_msaa_tex = nullptr;
	}
	fbo->swapchain = nullptr;
	fbo->cmd = nullptr;
}

bool vgpu_texture_init_native(vgpu_ctx_t* ctx, ovg_texture_t* tex,
	SDL_GPUTexture* texture, int width, int height, SDL_GPUTextureFormat format)
{
	if (!ctx || !ctx->device || !tex || !texture || width <= 0 || height <= 0 ||
		format == SDL_GPU_TEXTUREFORMAT_INVALID)
		return false;
	if (tex->image.id != 0)
		return false;

	auto* native = new vgpu_texture_t();
	native->texture = texture;
	native->device = ctx->device->gpu_device;
	native->format = format;
	native->width = width;
	native->height = height;
	native->owns_texture = false;
	native->sampler = create_sampler_linear(ctx->device->gpu_device);
	if (!native->sampler) {
		delete native;
		return false;
	}

	uint32_t id = ctx->next_image_id++;
	if (id == 0)
		id = ctx->next_image_id++;
	native->id = id;
	ctx->textures[id] = native;

	tex->image.id = id;
	tex->image.width = width;
	tex->image.height = height;
	tex->image.valid = false;
	tex->image.copy_status = false;
	return true;
}

bool vgpu_texture_update_native(vgpu_ctx_t* ctx, ovg_texture_t* tex,
	SDL_GPUTexture* texture, int width, int height, SDL_GPUTextureFormat format)
{
	if (!ctx || !tex || tex->image.id == 0 || !texture || width <= 0 || height <= 0 ||
		format == SDL_GPU_TEXTUREFORMAT_INVALID)
		return false;
	auto it = ctx->textures.find(tex->image.id);
	if (it == ctx->textures.end() || !it->second || it->second->owns_texture)
		return false;

	vgpu_texture_t* native = it->second;
	native->texture = texture;
	native->format = format;
	native->width = width;
	native->height = height;
	tex->image.width = width;
	tex->image.height = height;
	tex->image.valid = false;
	return true;
}

void vgpu_texture_fini_native(vgpu_ctx_t* ctx, ovg_texture_t* tex)
{
	if (!ctx || !tex || tex->image.id == 0)
		return;
	auto it = ctx->textures.find(tex->image.id);
	if (it != ctx->textures.end() && it->second && !it->second->owns_texture)
	{
		vgpu_texture_t* native = it->second;
		ctx->textures.erase(it);
		ctx->pending_destroy.push_back(native);
	}
	tex->image.id = 0;
	tex->image.width = 0;
	tex->image.height = 0;
	tex->image.valid = false;
}

void vgpu_reset_fbo(vg_fbo_t* fbo, int width, int height)
{
	if (!fbo || !fbo->ctx)
		return;

	vgpu_ctx_t* ctx = fbo->ctx;
	SDL_Window* window = fbo->window;
	vgpu_destroy_fbo(fbo);
	fbo->ctx = ctx;
	fbo->window = window;
	fbo->width = static_cast<uint32_t>((std::max)(width, 1));
	fbo->height = static_cast<uint32_t>((std::max)(height, 1));
	fbo->display_size = { static_cast<int>(fbo->width), static_cast<int>(fbo->height) };
	fbo->upsize = true;

	if (!window) {
		fbo->color_tex = create_texture(ctx->device,
			static_cast<int>(fbo->width), static_cast<int>(fbo->height),
			ctx->color_format,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	}
	if (ctx->samples != SDL_GPU_SAMPLECOUNT_1) {
		fbo->color_msaa_tex = create_texture(ctx->device,
			static_cast<int>(fbo->width), static_cast<int>(fbo->height),
			ctx->color_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, ctx->samples);
	}
}

int vgpu_acquire_swapchain(vgpu_ctx_t* ctx, vg_fbo_t* fbo, int count, bool wait)
{
	if (!ctx || !ctx->device || !ctx->device->gpu_device || !fbo || count < 1)
		return 0;

	SDL_GPUDevice* device = ctx->device->gpu_device;
	if (ctx->current_cmd) {
		if (ctx->swapchain_acquired)
			SDL_SubmitGPUCommandBuffer(ctx->current_cmd);
		else
			SDL_CancelGPUCommandBuffer(ctx->current_cmd);
		ctx->current_cmd = nullptr;
		ctx->swapchain_acquired = false;
	}

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
	if (!cmd)
		return 0;

	int acquired = 0;
	for (int i = 0; i < count; ++i) {
		vg_fbo_t* current = fbo + i;
		if (!current->ctx)
			current->ctx = ctx;
		// 每次 acquire 都获取新的 swapchain，避免复用上一帧已提交的纹理。
		current->swapchain = nullptr;
		if (!ensure_swapchain(ctx, cmd, current, wait)) {
			current->swapchain = nullptr;
			continue;
		}
		++acquired;
	}

	if (acquired == 0) {
		SDL_CancelGPUCommandBuffer(cmd);
		ctx->swapchain_acquired = false;
		return 0;
	}
	ctx->current_cmd = cmd;
	return acquired;
}

void vgpu_render_frame(vgpu_ctx_t* ctx, ovg_draw_data_t* data, size_t count)
{
	if (!ctx || !ctx->device || !ctx->device->gpu_device)
		return;

	if (count == 0 || !data) {
		if (ctx->current_cmd) {
			if (ctx->swapchain_acquired)
				SDL_SubmitGPUCommandBuffer(ctx->current_cmd);
			else
				SDL_CancelGPUCommandBuffer(ctx->current_cmd);
			ctx->current_cmd = nullptr;
			ctx->swapchain_acquired = false;
		}
		if (data) {
			for (size_t i = 0; i < count; ++i) {
				if (data[i].fbo)
					data[i].fbo->swapchain = nullptr;
			}
		}
		return;
	}

	if (!ctx->current_cmd) {
		ctx->current_cmd = SDL_AcquireGPUCommandBuffer(ctx->device->gpu_device);
		if (!ctx->current_cmd)
			return;
	}

	std::vector<ovg_draw_data_t*> items;
	items.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		ovg_draw_data_t* item = data + i;
		if (!item->fbo)
			continue;
		if (item->fbo->window && !item->fbo->swapchain) {
			if (!ensure_swapchain(ctx, ctx->current_cmd, item->fbo, true))
				continue;
		}
		if (!item->fbo->window && !item->fbo->color_tex)
			continue;
		items.push_back(item);
	}

	if (items.empty()) {
		if (ctx->swapchain_acquired)
			SDL_SubmitGPUCommandBuffer(ctx->current_cmd);
		else
			SDL_CancelGPUCommandBuffer(ctx->current_cmd);
		ctx->current_cmd = nullptr;
		ctx->swapchain_acquired = false;
		for (size_t i = 0; i < count; ++i) {
			if (data[i].fbo)
				data[i].fbo->swapchain = nullptr;
		}
		return;
	}

	std::vector<SDL_GPUTransferBuffer*> transfers;
	process_image_descs(ctx, ctx->current_cmd, items, transfers);
	upload_buffers(ctx, ctx->current_cmd, items, transfers);

	std::stable_sort(items.begin(), items.end(), [](const ovg_draw_data_t* a, const ovg_draw_data_t* b) {
		return a->fbo < b->fbo;
	});

	vg_fbo_t* current_fbo = nullptr;
	for (ovg_draw_data_t* item : items) {
		vg_fbo_t* fbo = item->fbo;
		if (fbo != current_fbo) {
			if (current_fbo)
				end_pass(ctx, current_fbo);
			current_fbo = fbo;
			begin_pass(ctx, fbo, true);
		}
		SDL_GPURenderPass* pass = ctx->pass;
		if (!pass)
			continue;

		// 每个 draw data 是独立 rvg 的绘制列表，裁剪状态在列表边界复位。
		SDL_Rect scissor = { 0, 0, static_cast<int>(fbo->width), static_cast<int>(fbo->height) };
		SDL_SetGPUScissor(pass, &scissor);

		for (size_t i = 0; i < item->count; ++i) {
			gcmd_t& command = item->d[i];
			switch (static_cast<cmd_type_e>(command.g.stype)) {
			case cmd_type_e::DRAW_VG:
				draw_vg(fbo, pass, &command.vg, &scissor, item->vg_offset);
				break;
			case cmd_type_e::DRAW_GEOM:
				draw_geom(fbo, pass, &command.g, item->geom_offset);
				break;
			}
		}
	}

	if (current_fbo)
		end_pass(ctx, current_fbo);

	SDL_SubmitGPUCommandBuffer(ctx->current_cmd);
	ctx->current_cmd = nullptr;
	ctx->pass = nullptr;
	ctx->swapchain_acquired = false;

	for (size_t i = 0; i < count; ++i) {
		if (data[i].fbo)
			data[i].fbo->swapchain = nullptr;
	}

	for (SDL_GPUTransferBuffer* transfer : transfers)
		SDL_ReleaseGPUTransferBuffer(ctx->device->gpu_device, transfer);

	if (!ctx->pending_destroy.empty()) {
		SDL_WaitForGPUIdle(ctx->device->gpu_device);
		for (vgpu_texture_t* texture : ctx->pending_destroy) {
			if (texture && texture != ctx->device->empty_texture)
				destroy_texture(texture);
		}
		ctx->pending_destroy.clear();
	}
}

void vgpu_wait_idle(vgpu_ctx_t* ctx)
{
	if (!ctx || !ctx->device || !ctx->device->gpu_device)
		return;
	SDL_WaitForGPUIdle(ctx->device->gpu_device);
}
