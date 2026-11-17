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
 *   - 保留 vgui 需要的填充/描边、渐变、文本图集、图片、矩形裁剪、MSAA
 *     和多 FBO/窗口交换链；
 *   - 使用独立前缀 vgpu_，不会和 OVG 自带渲染器产生符号冲突。
 */

#include <vgui/backend/vgui_renderer_sdl3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "shaders/spv_c/a_vg.vert.h"
#include "shaders/spv_c/a_vg.frag.h"
#include "shaders/spv_c/a_base3d.vert.h"
#include "shaders/spv_c/a_base3d.frag.h"
// 特效：mask 覆盖度写入 + compute 两趟模糊 + 合成（与 OVG 渲染器共用着色器）
#include "shaders/spv_c/effect_mask.frag.h"
#include "shaders/spv_c/effect_present.vert.h"
#include "shaders/spv_c/effect_present.frag.h"
#include "shaders/spv_c/blur.spv.h"

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

// One full vg pipeline set (one per vg_operator_t) for a specific
// (color format, sample count) combination.  The context's own format lives
// in vgpu_ctx_t::vg_pipelines; this cache only holds alternate formats
// requested by offscreen FBOs with an explicit color-format override.
struct vg_pipeline_set {
	SDL_GPUGraphicsPipeline* pipelines[4] = {};
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

// 特效（vg_effect_t）离屏资源：mask 是路径覆盖度（R8），模糊链在 R16F 上跑
// 两趟（水平 + 垂直），合成时两者一起采样。尺寸是 ROI（包围盒 + sigma 外扩）。
struct fx_set_t {
	vgpu_texture_t* mask = nullptr;
	vgpu_texture_t* tmp = nullptr;
	vgpu_texture_t* blur = nullptr;
	int width = 0;
	int height = 0;
	bool need_blur = false;
};

// 一条 draw 命令预先算好的特效结果；tex/mask 指向本帧借用的 fx_set。
struct fx_result_t {
	vgpu_texture_t* tex = nullptr;		// 模糊后的覆盖度（无模糊时即 mask）
	vgpu_texture_t* mask = nullptr;		// 原始覆盖度
	glm::vec4 rect = {};				// ROI：x, y, w, h（设备像素）
	glm::vec4 color = {};				// 直通 RGBA
	glm::vec2 offset = {};
	float spread = 0.0f;
	float opacity = 1.0f;
	int mode = 0;						// 0 外发光 / 1 内发光 / 2 阴影
};

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
	std::unordered_map<uint64_t, vg_pipeline_set> alt_vg_pipelines;

	std::unordered_map<uint32_t, vgpu_texture_t*> textures;
	std::vector<vgpu_texture_t*> pending_destroy;
	uint32_t next_image_id = 1;

	gpu_buffer_t vbo;
	gpu_buffer_t ibo;

	// SDL3 的 transfer buffer 每次创建都会申请一块专用显存，而被释放的缓冲
	// 要等后续提交推进才真正回收；窗口最小化/纯离屏帧没有交换链提交时，
	// 逐帧“创建-释放”转移缓冲会让显存单调增长。这里改为复用一组暂存槽位。
	struct staging_slot_t {
		SDL_GPUTransferBuffer* buffer = nullptr;
		uint32_t capacity = 0;
		bool busy = false;
	};
	// 一次提交的栅栏及该帧占用的槽位。fence 为 nullptr 表示它已被
	// vgpu_take_frame_fence() 交给 resize 退休队列，槽位改由“更晚的提交
	// 已完成”这一顺序保证归还。
	struct staging_flight_t {
		SDL_GPUFence* fence = nullptr;
		std::vector<uint32_t> slots;
		// 该次提交用到的特效纹理集合；栅栏完成后归还 fx_pool。
		std::vector<fx_set_t*> fx_sets;
	};
	std::vector<staging_slot_t> staging;
	std::vector<staging_flight_t> staging_flights;
	std::vector<uint32_t> staging_frame;

	SDL_GPUCommandBuffer* current_cmd = nullptr;
	SDL_GPURenderPass* pass = nullptr;
	bool swapchain_acquired = false;
	// Fence of the most recent real submission (nullptr when taken by a retire
	// queue or none was made yet).  Submissions on the same queue complete in
	// order, so this one fence guards all earlier work.  It is borrowed from
	// the matching staging flight, which owns and releases it.
	SDL_GPUFence* last_frame_fence = nullptr;

	// ------------------------------------------------------------------
	// 特效管线（对应 OVG 渲染器里的 fxSupported / pipeEffectMask /
	// pipeEffectComposite / comp_pipe[blur] 与纹理池）。
	// 本后端按设计去掉了 compute 与路径模板裁剪，这里补的是特效需要的那部分：
	//   mask 写入 R8 → compute 两趟模糊到 R16F → 在主 pass 里合成。
	// 硬件不支持时 fx_supported 保持 false，所有特效静默跳过（不崩、不报错）。
	// ------------------------------------------------------------------
	bool fx_supported = false;
	SDL_GPUTextureFormat fx_mask_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
	SDL_GPUTextureFormat fx_blur_format = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
	SDL_GPUSampler* fx_sampler = nullptr;
	SDL_GPUGraphicsPipeline* fx_mask_pipeline = nullptr;		// 写入 mask 格式
	SDL_GPUGraphicsPipeline* fx_present_pipeline = nullptr;	// 合成到主格式 + samples
	std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline*> fx_present_pipelines;
	SDL_GPUComputePipeline* fx_blur_pipeline = nullptr;
	// 空闲集合池：按 (w, h, need_blur) 复用，避免每帧为按钮重建纹理。
	std::vector<fx_set_t*> fx_pool;
	// 本帧正在使用的集合；提交时随栅栏一起交给 staging_flight，等 GPU 完成
	// 后才归还池子——否则下一帧可能写进 GPU 还在读的纹理。
	std::vector<fx_set_t*> fx_in_use;
};

namespace {

// ---------------------------------------------------------------------------
// 帧提交栅栏与暂存槽位
// ---------------------------------------------------------------------------

// 放弃本帧已编码的上传（命令缓冲被取消）：没有 GPU 工作会读取这些槽位。
void staging_release_frame(vgpu_ctx_t* ctx)
{
	for (uint32_t index : ctx->staging_frame)
		ctx->staging[index].busy = false;
	ctx->staging_frame.clear();
	for (fx_set_t* set : ctx->fx_in_use)
		ctx->fx_pool.push_back(set);
	ctx->fx_in_use.clear();
}

// 归还已完成提交占用的槽位。同队列提交按序完成：最新提交的栅栏触发时，
// 所有更早的提交（包括栅栏已交给 resize 退休队列的）都已完成。
void staging_collect(vgpu_ctx_t* ctx)
{
	SDL_GPUDevice* device = ctx->device->gpu_device;
	const bool latest_done = ctx->last_frame_fence &&
		SDL_QueryGPUFence(device, ctx->last_frame_fence);

	for (size_t i = ctx->staging_flights.size(); i-- > 0;)
	{
		vgpu_ctx_t::staging_flight_t& flight = ctx->staging_flights[i];
		if (flight.fence)
		{
			if (!SDL_QueryGPUFence(device, flight.fence))
				continue;
			if (flight.fence == ctx->last_frame_fence)
				ctx->last_frame_fence = nullptr;
			SDL_ReleaseGPUFence(device, flight.fence);
			flight.fence = nullptr;
		}
		else if (!latest_done)
		{
			// 栅栏已被退休队列取走，且还没有可证明完成的更晚提交。
			continue;
		}

		for (uint32_t index : flight.slots)
			ctx->staging[index].busy = false;
		for (fx_set_t* set : flight.fx_sets)
			ctx->fx_pool.push_back(set);
		ctx->staging_flights.erase(
			ctx->staging_flights.begin() + static_cast<std::ptrdiff_t>(i));
	}
}

SDL_GPUTransferBuffer* staging_acquire(vgpu_ctx_t* ctx, size_t size)
{
	for (size_t i = 0; i < ctx->staging.size(); ++i)
	{
		vgpu_ctx_t::staging_slot_t& slot = ctx->staging[i];
		if (slot.busy || slot.capacity < size)
			continue;
		slot.busy = true;
		ctx->staging_frame.push_back(static_cast<uint32_t>(i));
		return slot.buffer;
	}

	SDL_GPUTransferBufferCreateInfo info = {};
	info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	info.size = static_cast<Uint32>(size);
	SDL_GPUTransferBuffer* buffer =
		SDL_CreateGPUTransferBuffer(ctx->device->gpu_device, &info);
	if (!buffer)
		return nullptr;
	ctx->staging.push_back({ buffer, static_cast<uint32_t>(size), true });
	ctx->staging_frame.push_back(
		static_cast<uint32_t>(ctx->staging.size() - 1));
	return buffer;
}

// Submits a command buffer and keeps its fence as the context's "latest
// frame" fence: any resource retired from this point on can be guarded by
// it, because submissions on one queue complete in order.  The fence is
// owned by a staging flight (which may have no slots) and released by
// staging_collect() once the submission is known complete.
void submit_with_fence(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd)
{
	staging_collect(ctx);
	SDL_GPUFence* fence =
		SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (!fence)
	{
		// 提交失败：本帧编码的上传不会被 GPU 读取，槽位可立即归还。
		staging_release_frame(ctx);
		return;
	}
	ctx->staging_flights.push_back({ fence, std::move(ctx->staging_frame) });
	ctx->staging_flights.back().fx_sets = std::move(ctx->fx_in_use);
	ctx->fx_in_use.clear();
	ctx->staging_frame.clear();
	ctx->last_frame_fence = fence;
}

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
	SDL_GPUSampler* s = SDL_CreateGPUSampler(device, &info);
	return s;
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
	SDL_GPUSampler* s = SDL_CreateGPUSampler(device, &info);
	return s;
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
	{
		char tex_name[64];
		snprintf(tex_name, sizeof(tex_name), "vgpu_tex_%dx%d_s%d",
			width, height, (int)samples);
		SDL_SetGPUTextureName(device->gpu_device, texture->texture, tex_name);
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

// Builds the four vg pipelines for one (color format, sample count)
// combination.  Used for the context's own format at context creation and,
// lazily, for offscreen FBOs with an explicit color-format override (HDR /
// sRGB targets).  Returns false when any pipeline failed to create.
bool create_vg_pipeline_set(vgpu_ctx_t* ctx, SDL_GPUTextureFormat format,
	SDL_GPUSampleCount samples, vg_pipeline_set* out)
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
		out->pipelines[op] = create_pipeline(
			ctx->device, ctx->device->vg_vert, ctx->device->vg_frag,
			format, samples,
			SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, blend,
			OVG_VERTEX_SIZE, attributes, 3,
			SDL_GPU_FILLMODE_FILL, SDL_GPU_CULLMODE_NONE,
			SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE);
		if (!out->pipelines[op])
			return false;
	}
	return true;
}

void create_vg_pipelines(vgpu_ctx_t* ctx)
{
	vg_pipeline_set set = {};
	create_vg_pipeline_set(ctx, ctx->color_format, ctx->samples, &set);
	for (int op = 0; op < 4; ++op)
		ctx->vg_pipelines[op] = set.pipelines[op];
}

SDL_GPUGraphicsPipeline* get_vg_pipeline(vgpu_ctx_t* ctx, vg_fbo_t* fbo, int op)
{
	if (op < 0 || op >= 4)
		op = static_cast<int>(vg_operator_t::VG_OPERATOR_OVER);
	// Pipelines must match the color attachment format (and sample count) of
	// the pass they are bound in.  The context's own format/count uses the
	// eagerly created set; alternates are built lazily on first use.
	const SDL_GPUSampleCount samples = fbo ? fbo->samples : ctx->samples;
	const SDL_GPUTextureFormat format =
		(fbo && fbo->color_format != SDL_GPU_TEXTUREFORMAT_INVALID)
			? fbo->color_format
			: ctx->color_format;
	if (format == ctx->color_format && samples == ctx->samples) {
		if (!ctx->vg_pipelines[op])
			return ctx->vg_pipelines[static_cast<int>(vg_operator_t::VG_OPERATOR_OVER)];
		return ctx->vg_pipelines[op];
	}

	const uint64_t key = (static_cast<uint64_t>(format) << 8) |
		static_cast<uint64_t>(samples);
	auto it = ctx->alt_vg_pipelines.find(key);
	if (it == ctx->alt_vg_pipelines.end()) {
		vg_pipeline_set set = {};
		create_vg_pipeline_set(ctx, format, samples, &set);
		it = ctx->alt_vg_pipelines.emplace(key, set).first;
	}
	if (!it->second.pipelines[op])
		return it->second.pipelines[static_cast<int>(vg_operator_t::VG_OPERATOR_OVER)];
	return it->second.pipelines[op];
}

uint64_t make_geom_key(const gem_info_t* info, SDL_GPUTextureFormat format,
	SDL_GPUSampleCount samples)
{
	// 只把真正影响管线状态的字段放进 key，避免 flags/lineWidth 造成无谓的
	// 管线变体。shader 目前只用 a_base3d，但保留在 key 中便于以后扩展。
	// 颜色格式与采样数决定管线的 target 兼容性，同样入 key。
	uint64_t key = static_cast<uint8_t>(info->blendMode);
	key |= static_cast<uint64_t>(info->topology) << 8;
	key |= static_cast<uint64_t>(info->polygon) << 16;
	key |= static_cast<uint64_t>(info->frontFace) << 24;
	key |= static_cast<uint64_t>(info->cullMode) << 32;
	key |= static_cast<uint64_t>(info->shader) << 40;
	key |= static_cast<uint64_t>(format) << 48;
	key |= static_cast<uint64_t>(samples) << 56;
	return key;
}

SDL_GPUGraphicsPipeline* create_geom_pipeline(vgpu_ctx_t* ctx, const gem_info_t* info,
	SDL_GPUTextureFormat format, SDL_GPUSampleCount samples)
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
		format, samples,
		topology, info->blendMode,
		GEOM_VERTEX_SIZE, attributes, 3,
		fill, cull, front);
}

SDL_GPUGraphicsPipeline* get_geom_pipeline(vgpu_ctx_t* ctx, vg_fbo_t* fbo,
	const gem_info_t* info)
{
	const SDL_GPUSampleCount samples = fbo ? fbo->samples : ctx->samples;
	const SDL_GPUTextureFormat format =
		(fbo && fbo->color_format != SDL_GPU_TEXTUREFORMAT_INVALID)
			? fbo->color_format
			: ctx->color_format;
	const uint64_t key = make_geom_key(info, format, samples);
	auto it = ctx->geom_pipelines.find(key);
	if (it != ctx->geom_pipelines.end())
		return it->second.pipeline;

	SDL_GPUGraphicsPipeline* pipeline = create_geom_pipeline(ctx, info, format,
		samples);
	ctx->geom_pipelines.emplace(key, geom_pipeline_t{ pipeline });
	return pipeline;
}

// ---------------------------------------------------------------------------
// 上传
// ---------------------------------------------------------------------------

void process_image_descs(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd,
	const std::vector<ovg_draw_data_t*>& items)
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
				else if (id >= ctx->next_image_id)
				{
					/* The descriptor re-uses an id that lives in the ovg state
					 * (e.g. the shared font atlas) rather than one handed out
					 * by the counter.  Advance the counter past it, otherwise
					 * a later fresh image / native texture gets the same id
					 * and silently overwrites this entry in ctx->textures,
					 * leaking the overwritten GPU texture. */
					ctx->next_image_id = id + 1;
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

			SDL_GPUTransferBuffer* transfer = staging_acquire(ctx, total_bytes);
			if (!transfer)
				continue;

			void* mapped = SDL_MapGPUTransferBuffer(ctx->device->gpu_device, transfer, false);
			if (!mapped)
				continue;

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
				if (!copy)
					continue;
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
			desc->img->valid = false;
		}
	}

	if (copy)
		SDL_EndGPUCopyPass(copy);
}

void upload_buffers(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd,
	const std::vector<ovg_draw_data_t*>& items)
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

	SDL_GPUTransferBuffer* transfer = staging_acquire(ctx, vbo_bytes + ibo_bytes);
	if (!transfer)
		return;

	void* mapped = SDL_MapGPUTransferBuffer(ctx->device->gpu_device, transfer, false);
	if (!mapped)
		return;

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
	if (!copy)
		return;
	if (vbo_bytes > 0) {
	SDL_GPUTransferBufferLocation src = {};
	src.transfer_buffer = transfer;
	src.offset = 0;
	SDL_GPUBufferRegion dst = {};
	dst.buffer = ctx->vbo.buffer;
	dst.offset = 0;
	dst.size = static_cast<Uint32>(vbo_bytes);
	// NOTE: cycle=false is deliberate.  SDL3's buffer cycling rotated the
	// backing storage on this machine/driver in a way that blanked the window
	// content entirely (verified by A/B capture); frames are drained by vsync
	// so the offset-0 overwrite race is not hit in practice.  Revisit with a
	// vgpu-side ring buffer before ever running multiple submissions per
	// frame with heavy overlap.
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

// ---------------------------------------------------------------------------
// 特效（vg_effect_t）：mask 离屏 → compute 两趟模糊 → 主 pass 合成
//
// 对应 OVG 渲染器里的 pipeEffectMask / pipeEffectComposite / comp_pipe[blur]
// 与 fx 纹理池。本后端按设计砍掉了 compute 与路径模板裁剪，这里补的正是特效
// 需要的那部分；硬件不支持（R16F 不能做 compute 写、也没有 R8/RGBA8 可渲染
// 格式）时整体关闭，特效静默失效，但主体绘制完全不受影响。
// ---------------------------------------------------------------------------

struct fx_blur_params_t {
	glm::vec2 texel;
	float sigma;
	int axis;
};

void set_viewport2d(SDL_GPURenderPass* pass, int w, int h)
{
	SDL_GPUViewport vp = {};
	vp.x = 0.0f;
	vp.y = static_cast<float>(h);
	vp.w = static_cast<float>(w);
	vp.h = -static_cast<float>(h);
	vp.min_depth = 0.0f;
	vp.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &vp);
}

void destroy_fx_set(vgpu_device_t* device, fx_set_t* set)
{
	if (!set)
		return;
	(void)device;
	destroy_texture(set->mask);
	destroy_texture(set->tmp);
	destroy_texture(set->blur);
	delete set;
}

// 合成管线的目标格式 / 采样数必须和被合成的 pass 一致，因此按 (format,
// samples) 缓存：窗口是 (ctx 格式, ctx 采样数)，离屏 FBO 可能不同。
SDL_GPUGraphicsPipeline* get_fx_present_pipeline(vgpu_ctx_t* ctx,
	SDL_GPUTextureFormat format, SDL_GPUSampleCount samples)
{
	// 与上下文同格式/同采样数的组合缓存到成员上（最常用），其余按 key 缓存。
	SDL_GPUGraphicsPipeline** member_slot = nullptr;
	uint64_t key = 0;
	if (format == ctx->color_format && samples == ctx->samples) {
		if (ctx->fx_present_pipeline)
			return ctx->fx_present_pipeline;
		member_slot = &ctx->fx_present_pipeline;
	}
	else {
		key = (static_cast<uint64_t>(format) << 8) | static_cast<uint64_t>(samples);
		auto it = ctx->fx_present_pipelines.find(key);
		if (it != ctx->fx_present_pipelines.end())
			return it->second;
	}

	SDL_GPUDevice* device = ctx->device->gpu_device;
	SDL_GPUShader* vert = compile_shader(device, SDL_GPU_SHADERSTAGE_VERTEX,
		a_effect_present_vert, sizeof(a_effect_present_vert), 0, 0, 0, 0);
	SDL_GPUShader* frag = compile_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		a_effect_present_frag, sizeof(a_effect_present_frag), 2, 1, 0, 0);
	SDL_GPUGraphicsPipeline* pipeline = nullptr;
	if (vert && frag) {
		SDL_GPUColorTargetDescription ct = {};
		ct.format = format;
		set_blend_state(ct.blend_state, static_cast<int>(blendMode_e::normal_prem));

		SDL_GPUDepthStencilState ds = {};
		ds.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
		ds.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
		ds.front_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
		ds.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
		ds.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
		ds.back_stencil_state = ds.front_stencil_state;

		SDL_GPURasterizerState rs = {};
		rs.fill_mode = SDL_GPU_FILLMODE_FILL;
		rs.cull_mode = SDL_GPU_CULLMODE_NONE;
		rs.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

		SDL_GPUMultisampleState ms = {};
		ms.sample_count = samples;

		// 全屏三角形不需要顶点输入：显式 0 个 buffer / 0 个 attribute。
		SDL_GPUVertexInputState vi = {};

		SDL_GPUGraphicsPipelineCreateInfo info = {};
		info.vertex_shader = vert;
		info.fragment_shader = frag;
		info.vertex_input_state = vi;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state = rs;
		info.multisample_state = ms;
		info.depth_stencil_state = ds;
		info.target_info.num_color_targets = 1;
		info.target_info.color_target_descriptions = &ct;
		pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
		if (!pipeline) {
			SDL_LogError(SDL_LOG_CATEGORY_GPU,
				"vgui: failed to create fx present pipeline: %s", SDL_GetError());
		}
	}
	if (vert) SDL_ReleaseGPUShader(device, vert);
	if (frag) SDL_ReleaseGPUShader(device, frag);
	if (member_slot)
		*member_slot = pipeline;
	else
		ctx->fx_present_pipelines.emplace(key, pipeline);
	return pipeline;
}

// 探测格式并建好 mask / 合成 / 模糊三条管线；任何一步失败都整体关闭。
bool create_fx_resources(vgpu_ctx_t* ctx)
{
	if (!ctx || !ctx->device || !ctx->device->gpu_device)
		return false;
	SDL_GPUDevice* device = ctx->device->gpu_device;

	const bool mask_r8 = SDL_GPUTextureSupportsFormat(device,
		SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTURETYPE_2D,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	const bool mask_rgba8 = SDL_GPUTextureSupportsFormat(device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	const bool blur_r16 = SDL_GPUTextureSupportsFormat(device,
		SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTURETYPE_2D,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
	if ((!mask_r8 && !mask_rgba8) || !blur_r16)
		return false;
	ctx->fx_mask_format = mask_r8 ? SDL_GPU_TEXTUREFORMAT_R8_UNORM
		: SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	ctx->fx_blur_format = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;

	ctx->fx_sampler = create_sampler(device);
	if (!ctx->fx_sampler)
		return false;

	// mask：与 a_vg 顶点布局相同，片元只输出覆盖度 alpha，单采样、无混合。
	SDL_GPUShader* mask_frag = compile_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		a_effect_mask_frag, sizeof(a_effect_mask_frag), 0, 0, 0, 0);
	if (!ctx->device->vg_vert || !mask_frag) {
		if (mask_frag) SDL_ReleaseGPUShader(device, mask_frag);
		return false;
	}
	{
		static const SDL_GPUVertexAttribute attributes[] = {
			{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
			{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 },
			{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 },
		};
		SDL_GPUColorTargetDescription ct = {};
		ct.format = ctx->fx_mask_format;
		set_blend_state(ct.blend_state, static_cast<int>(blendMode_e::none));
		// 只需要覆盖度：R 单通道写入，省带宽也避免 RGBA8 回退时的多余填充。
		ct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R;

		SDL_GPUDepthStencilState ds = {};
		ds.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
		ds.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
		ds.front_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
		ds.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
		ds.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
		ds.back_stencil_state = ds.front_stencil_state;

		SDL_GPURasterizerState rs = {};
		rs.fill_mode = SDL_GPU_FILLMODE_FILL;
		rs.cull_mode = SDL_GPU_CULLMODE_NONE;
		rs.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

		SDL_GPUVertexBufferDescription vbd = {};
		vbd.slot = 0;
		vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		vbd.pitch = OVG_VERTEX_SIZE;
		SDL_GPUVertexInputState vi = {};
		vi.num_vertex_buffers = 1;
		vi.vertex_buffer_descriptions = &vbd;
		vi.num_vertex_attributes = 3;
		vi.vertex_attributes = attributes;

		SDL_GPUMultisampleState ms = {};
		ms.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SDL_GPUGraphicsPipelineCreateInfo info = {};
		info.vertex_shader = ctx->device->vg_vert;
		info.fragment_shader = mask_frag;
		info.vertex_input_state = vi;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state = rs;
		info.multisample_state = ms;
		info.depth_stencil_state = ds;
		info.target_info.num_color_targets = 1;
		info.target_info.color_target_descriptions = &ct;
		ctx->fx_mask_pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
		SDL_ReleaseGPUShader(device, mask_frag);
		if (!ctx->fx_mask_pipeline)
			return false;
	}

	// 模糊：blur.comp 每次处理 8x8，读写各一张纹理。参数来自 device 的
	// threadcount 约定（8,8,1）。
	{
		SDL_GPUComputePipelineCreateInfo cp = {};
		cp.code_size = sizeof(a_blur_comp);
		cp.code = reinterpret_cast<const Uint8*>(a_blur_comp);
		cp.entrypoint = "main";
		cp.format = detect_shader_format(device);
		cp.num_samplers = 1;
		cp.num_uniform_buffers = 1;
		cp.num_readwrite_storage_textures = 1;
		cp.threadcount_x = 8;
		cp.threadcount_y = 8;
		cp.threadcount_z = 1;
		ctx->fx_blur_pipeline = SDL_CreateGPUComputePipeline(device, &cp);
		if (!ctx->fx_blur_pipeline)
			return false;
	}

	if (!get_fx_present_pipeline(ctx, ctx->color_format, ctx->samples))
		return false;

	ctx->fx_supported = true;
	return true;
}

void destroy_fx_resources(vgpu_ctx_t* ctx)
{
	if (!ctx)
		return;
	SDL_GPUDevice* device = ctx->device ? ctx->device->gpu_device : nullptr;
	for (fx_set_t* set : ctx->fx_pool)
		destroy_fx_set(ctx->device, set);
	ctx->fx_pool.clear();
	for (fx_set_t* set : ctx->fx_in_use)
		destroy_fx_set(ctx->device, set);
	ctx->fx_in_use.clear();
	if (!device)
		return;
	if (ctx->fx_mask_pipeline) {
		SDL_ReleaseGPUGraphicsPipeline(device, ctx->fx_mask_pipeline);
		ctx->fx_mask_pipeline = nullptr;
	}
	if (ctx->fx_present_pipeline) {
		SDL_ReleaseGPUGraphicsPipeline(device, ctx->fx_present_pipeline);
		ctx->fx_present_pipeline = nullptr;
	}
	for (auto& kv : ctx->fx_present_pipelines) {
		if (kv.second && kv.second != ctx->fx_present_pipeline)
			SDL_ReleaseGPUGraphicsPipeline(device, kv.second);
	}
	ctx->fx_present_pipelines.clear();
	if (ctx->fx_blur_pipeline) {
		SDL_ReleaseGPUComputePipeline(device, ctx->fx_blur_pipeline);
		ctx->fx_blur_pipeline = nullptr;
	}
	if (ctx->fx_sampler) {
		SDL_ReleaseGPUSampler(device, ctx->fx_sampler);
		ctx->fx_sampler = nullptr;
	}
	ctx->fx_supported = false;
}

// 取一套与 (w, h, need_blur) 匹配的空闲纹理集合；池里没有就新建。
// 池里保存的都是 GPU 已确认写完的集合（由 staging_flight 归还）。
fx_set_t* fx_acquire(vgpu_ctx_t* ctx, int w, int h, bool need_blur)
{
	for (size_t i = 0; i < ctx->fx_pool.size(); ++i) {
		fx_set_t* set = ctx->fx_pool[i];
		if (set->width == w && set->height == h && set->need_blur == need_blur) {
			ctx->fx_pool.erase(ctx->fx_pool.begin() + static_cast<std::ptrdiff_t>(i));
			return set;
		}
	}

	auto* set = new fx_set_t();
	set->width = w;
	set->height = h;
	set->need_blur = need_blur;
	const SDL_GPUTextureUsageFlags mask_usage =
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	set->mask = create_texture(ctx->device, w, h, ctx->fx_mask_format, mask_usage);
	if (!set->mask) {
		delete set;
		return nullptr;
	}
	if (need_blur) {
		const SDL_GPUTextureUsageFlags blur_usage =
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
		set->tmp = create_texture(ctx->device, w, h, ctx->fx_blur_format, blur_usage);
		set->blur = create_texture(ctx->device, w, h, ctx->fx_blur_format, blur_usage);
		if (!set->tmp || !set->blur) {
			destroy_fx_set(ctx->device, set);
			return nullptr;
		}
	}
	return set;
}

// 池子只缓存一小把尺寸，避免 UI 里各种临时尺寸把显存顶起来。
void fx_trim_pool(vgpu_ctx_t* ctx)
{
	constexpr size_t kMaxSets = 16;
	while (ctx->fx_pool.size() > kMaxSets) {
		destroy_fx_set(ctx->device, ctx->fx_pool.front());
		ctx->fx_pool.erase(ctx->fx_pool.begin());
	}
}

// bounds 是录制阶段 apply_effect_bounds() 算好的包围盒（已含 3*sigma 外扩）。
bool fx_roi_from_bounds(const vgcmd_t* c, const vg_fbo_t* fbo, glm::ivec4& roi)
{
	if (!c || !fbo || !(c->bounds.z > 0.0f) || !(c->bounds.w > 0.0f))
		return false;
	const float fx0 = (std::max)(c->bounds.x, 0.0f);
	const float fy0 = (std::max)(c->bounds.y, 0.0f);
	const float fx1 = (std::min)(c->bounds.x + c->bounds.z, static_cast<float>(fbo->width));
	const float fy1 = (std::min)(c->bounds.y + c->bounds.w, static_cast<float>(fbo->height));
	if (!(fx1 > fx0) || !(fy1 > fy0))
		return false;
	const int x0 = static_cast<int>(std::floor(fx0));
	const int y0 = static_cast<int>(std::floor(fy0));
	const int x1 = static_cast<int>(std::ceil(fx1));
	const int y1 = static_cast<int>(std::ceil(fy1));
	if (x1 <= x0 || y1 <= y0)
		return false;
	roi = glm::ivec4(x0, y0, x1 - x0, y1 - y0);
	return true;
}

// 把这条 fill/stroke 的几何重画到 R8 mask 上：viewport 取 ROI 尺寸，几何整体
// 平移 -roi.xy（平移落在 mat3 的第三列）。没有 stencil，裁剪靠 ROI 本身。
void render_effect_mask(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd,
	ovg_draw_data_t* kd, vgcmd_t* c, vgpu_texture_t* mask, const glm::ivec4& roi)
{
	if (!ctx || !cmd || !kd || !c || !c->state || !mask || !mask->texture)
		return;
	SDL_GPUColorTargetInfo ct = {};
	ct.texture = mask->texture;
	ct.load_op = SDL_GPU_LOADOP_CLEAR;
	ct.store_op = SDL_GPU_STOREOP_STORE;
	ct.clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
	if (!pass)
		return;

	set_viewport2d(pass, roi.z, roi.w);
	SDL_Rect sc = { 0, 0, roi.z, roi.w };
	SDL_SetGPUScissor(pass, &sc);
	SDL_BindGPUGraphicsPipeline(pass, ctx->fx_mask_pipeline);
	bind_vertex_buffer(pass, ctx->vbo, kd->vg_offset.x);
	bind_index_buffer(pass, ctx->ibo, kd->vg_offset.y);

	push_constants_t pc = c->state->pushConsts;
	pc.size = { static_cast<float>(roi.z), static_cast<float>(roi.w) };
	pc.mat[2].x -= static_cast<float>(roi.x);
	pc.mat[2].y -= static_cast<float>(roi.y);
	pc.source = { 1.0f, 1.0f, 1.0f, 1.0f };
	pc.fsq_patternType = static_cast<uint32_t>(vg_pattern_type_t::VG_PATTERN_TYPE_SOLID);
	pc.opacity = 1.0f;
	SDL_PushGPUVertexUniformData(cmd, 0, &pc, sizeof(pc));

	if (c->index.y > 0) {
		SDL_DrawGPUIndexedPrimitives(pass, static_cast<Uint32>(c->index.y), 1,
			static_cast<Uint32>(c->index.x), static_cast<int32_t>(c->vertex.x), 0);
	}
	else {
		SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(c->vertex.y), 1,
			static_cast<Uint32>(c->vertex.x), 0);
	}
	SDL_EndGPURenderPass(pass);
}

void dispatch_fx_blur(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd,
	vgpu_texture_t* dst, vgpu_texture_t* src, const fx_blur_params_t& params,
	int w, int h)
{
	if (!ctx || !cmd || !ctx->fx_blur_pipeline || !dst || !dst->texture ||
		!src || !src->texture || w <= 0 || h <= 0)
		return;
	SDL_GPUStorageTextureReadWriteBinding rw = {};
	rw.texture = dst->texture;
	rw.layer = 0;
	rw.mip_level = 0;
	rw.cycle = false;
	SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
	if (!pass)
		return;
	SDL_BindGPUComputePipeline(pass, ctx->fx_blur_pipeline);
	SDL_GPUTextureSamplerBinding binding = { src->texture, ctx->fx_sampler };
	SDL_BindGPUComputeSamplers(pass, 0, &binding, 1);
	SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
	SDL_DispatchGPUCompute(pass,
		static_cast<Uint32>((w + 7) / 8), static_cast<Uint32>((h + 7) / 8), 1);
	SDL_EndGPUComputePass(pass);
}

// 主 pass 之前把一条命令的特效算好：mask → 水平模糊 → 垂直模糊。
void prepare_effect(vgpu_ctx_t* ctx, SDL_GPUCommandBuffer* cmd,
	ovg_draw_data_t* kd, vgcmd_t* c, vg_fbo_t* fbo, fx_result_t* out)
{
	if (!out)
		return;
	out->tex = nullptr;
	if (!ctx || !ctx->fx_supported || !cmd || !kd || !c || !c->state || !fbo)
		return;
	const vg_effect_t& e = c->state->effect;
	if (c->ref < 0 || e.type == VG_EFFECT_NONE || e.opacity <= 0.0f)
		return;
	if (((e.color >> 24) & 0xFF) == 0)
		return;
	if (c->index.y == 0 && c->vertex.y == 0)
		return;
	// 合成管线绑定在目标 pass 上，格式/采样数必须对得上；对不上就跳过特效，
	// 主体照常绘制（HDR 或自定义采样的离屏 FBO 走这条路）。
	const SDL_GPUTextureFormat target_format = (fbo->color_format != SDL_GPU_TEXTUREFORMAT_INVALID)
		? fbo->color_format : ctx->color_format;
	if (!get_fx_present_pipeline(ctx, target_format, fbo->samples))
		return;

	glm::ivec4 roi;
	if (!fx_roi_from_bounds(c, fbo, roi))
		return;
	const int w = roi.z;
	const int h = roi.w;
	const bool need_blur = e.sigma > 0.01f;

	fx_set_t* set = fx_acquire(ctx, w, h, need_blur);
	if (!set)
		return;
	ctx->fx_in_use.push_back(set);
	fx_trim_pool(ctx);

	render_effect_mask(ctx, cmd, kd, c, set->mask, roi);

	if (need_blur) {
		fx_blur_params_t bp = {};
		bp.texel = { 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h) };
		bp.sigma = e.sigma;
		bp.axis = 0;
		dispatch_fx_blur(ctx, cmd, set->tmp, set->mask, bp, w, h);
		bp.axis = 1;
		dispatch_fx_blur(ctx, cmd, set->blur, set->tmp, bp, w, h);
	}

	out->tex = need_blur ? set->blur : set->mask;
	out->mask = set->mask;
	out->rect = glm::vec4(static_cast<float>(roi.x), static_cast<float>(roi.y),
		static_cast<float>(w), static_cast<float>(h));
	out->color = glm::vec4(
		static_cast<float>(e.color & 0xFF) / 255.0f,
		static_cast<float>((e.color >> 8) & 0xFF) / 255.0f,
		static_cast<float>((e.color >> 16) & 0xFF) / 255.0f,
		static_cast<float>((e.color >> 24) & 0xFF) / 255.0f);
	out->offset = e.offset;
	out->spread = e.spread;
	out->opacity = e.opacity;
	out->mode = (e.type == VG_EFFECT_OUTER_GLOW) ? 0
		: (e.type == VG_EFFECT_INNER_GLOW) ? 1 : 2;
}

// 在目标 pass 里叠一层特效：scissor 到 ROI ∩ 当前裁剪，画一个全屏三角形，
// 由 effect_present.frag 按 mode 合成，结束后把 scissor 还给调用方。
void draw_effect_quad(vg_fbo_t* fbo, SDL_GPURenderPass* pass,
	const fx_result_t* fx, SDL_Rect* clip)
{
	if (!fbo || !pass || !fx || !fx->tex)
		return;
	vgpu_ctx_t* ctx = fbo->ctx;
	if (!ctx || !ctx->fx_sampler)
		return;
	const SDL_GPUTextureFormat format = (fbo->color_format != SDL_GPU_TEXTUREFORMAT_INVALID)
		? fbo->color_format : ctx->color_format;
	SDL_GPUGraphicsPipeline* pipeline = get_fx_present_pipeline(ctx, format, fbo->samples);
	if (!pipeline)
		return;
	SDL_BindGPUGraphicsPipeline(pass, pipeline);

	int x0 = static_cast<int>(fx->rect.x);
	int y0 = static_cast<int>(fx->rect.y);
	int x1 = x0 + static_cast<int>(fx->rect.z);
	int y1 = y0 + static_cast<int>(fx->rect.w);
	if (clip && clip->w > 0 && clip->h > 0) {
		x0 = (std::max)(x0, clip->x);
		y0 = (std::max)(y0, clip->y);
		x1 = (std::min)(x1, clip->x + clip->w);
		y1 = (std::min)(y1, clip->y + clip->h);
	}
	x0 = (std::max)(x0, 0);
	y0 = (std::max)(y0, 0);
	x1 = (std::min)(x1, static_cast<int>(fbo->width));
	y1 = (std::min)(y1, static_cast<int>(fbo->height));
	if (x1 <= x0 || y1 <= y0)
		return;
	SDL_Rect rc = { x0, y0, x1 - x0, y1 - y0 };
	SDL_SetGPUScissor(pass, &rc);

	SDL_GPUTextureSamplerBinding bindings[2] = {
		{ fx->mask->texture, ctx->fx_sampler },
		{ fx->tex->texture, ctx->fx_sampler },
	};
	SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);

	// 与 effect_present.frag 的 Params 布局逐字段对齐（56 字节）。
	struct fx_present_params_t {
		int mode;
		float spread;
		float opacity;
		float pad0;
		float offset[2];
		float color[4];
		float rect[4];
	};
	static_assert(sizeof(fx_present_params_t) == 56, "fx_present_params_t must match effect_present.frag");
	fx_present_params_t pu = {};
	pu.mode = fx->mode;
	pu.spread = fx->spread;
	pu.opacity = fx->opacity;
	pu.offset[0] = fx->offset.x;
	pu.offset[1] = fx->offset.y;
	pu.color[0] = fx->color.x;
	pu.color[1] = fx->color.y;
	pu.color[2] = fx->color.z;
	pu.color[3] = fx->color.w;
	pu.rect[0] = fx->rect.x;
	pu.rect[1] = fx->rect.y;
	pu.rect[2] = fx->rect.z;
	pu.rect[3] = fx->rect.w;
	SDL_PushGPUFragmentUniformData(fbo->cmd, 0, &pu, sizeof(pu));
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

	// 恢复调用方的裁剪状态，否则后续文字/图形会被 ROI 裁掉。
	if (clip && clip->w > 0 && clip->h > 0)
		SDL_SetGPUScissor(pass, clip);
	else {
		SDL_Rect full = { 0, 0, static_cast<int>(fbo->width), static_cast<int>(fbo->height) };
		SDL_SetGPUScissor(pass, &full);
	}
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
	SDL_Rect* current_scissor, const glm::uvec2& offset,
	const fx_result_t* fx = nullptr)
{
	if (!cmd || !fbo || !fbo->ctx)
		return;

	vgpu_ctx_t* ctx = fbo->ctx;

	// 外发光/阴影叠在主体之前，内发光叠在主体之后（neon 那种“描边自己发亮”
	// 就是 outer glow，文字发光仍靠上层叠副本，见 sample/vgui_demo.cpp）。
	const bool fx_valid = fx && fx->tex && fx->mask;
	if (fx_valid && fx->mode != 1)
		draw_effect_quad(fbo, pass, fx, current_scissor);
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

	SDL_GPUGraphicsPipeline* vg_pipeline = get_vg_pipeline(ctx, fbo,
		state ? static_cast<int>(state->curOperator) : static_cast<int>(vg_operator_t::VG_OPERATOR_OVER));
	if (!vg_pipeline)
		return;
	SDL_BindGPUGraphicsPipeline(pass, vg_pipeline);
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

	// 内发光画在主体之后：掩码在主体覆盖区域内侧，先画光再画主体会被盖住。
	if (fx_valid && fx->mode == 1 && (cmd->type == 0 || cmd->type == 1))
		draw_effect_quad(fbo, pass, fx, current_scissor);
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

	SDL_GPUGraphicsPipeline* pipeline = get_geom_pipeline(ctx, fbo, &cmd->state);
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

	// Load-op over MSAA: the host wrote its scene into the swapchain, but the
	// pass renders into the separate MSAA texture and the resolve at the end
	// overwrites the swapchain wholesale.  Without preloading, the UI pass
	// would LOAD an undefined MSAA buffer and every frame would present random
	// background content (flicker).  Copy the swapchain content into the MSAA
	// texture first so LOAD sees the host's frame.
	if (fbo->color_msaa_tex && !clear && fbo->swapchain) {
		SDL_GPUBlitInfo preload = {};
		preload.source.texture = fbo->swapchain;
		preload.source.w = fbo->width;
		preload.source.h = fbo->height;
		preload.destination.texture = fbo->color_msaa_tex->texture;
		preload.destination.w = fbo->width;
		preload.destination.h = fbo->height;
		preload.load_op = SDL_GPU_LOADOP_DONT_CARE;
		preload.filter = SDL_GPU_FILTER_LINEAR;
		SDL_BlitGPUTexture(ctx->current_cmd, &preload);
	}

	SDL_GPUColorTargetInfo color_info = {};
	color_info.texture = render_target;
	color_info.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	color_info.store_op = fbo->color_msaa_tex ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE;
	color_info.clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
	// Offscreen targets rewritten every frame are sampled later in the same
	// command buffer; cycling prevents a write/write race against the still
	// pending previous frame.  Never cycle the swapchain (SDL owns it).
	color_info.cycle = fbo->cycle_target;
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
	// 特效管线是可选增强：探测失败就保持关闭，不让整个上下文建不起来。
	create_fx_resources(ctx);
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
	// last_frame_fence 只是最近一次提交的借用指针，所有权在对应的 flight 上。
	for (vgpu_ctx_t::staging_flight_t& flight : ctx->staging_flights) {
		if (flight.fence && device)
			SDL_ReleaseGPUFence(device, flight.fence);
		// 还在飞行中的特效纹理：归还到 in_use，由 destroy_fx_resources() 统一销毁。
		for (fx_set_t* set : flight.fx_sets)
			ctx->fx_in_use.push_back(set);
		flight.fx_sets.clear();
	}
	ctx->staging_flights.clear();
	ctx->last_frame_fence = nullptr;
	for (vgpu_ctx_t::staging_slot_t& slot : ctx->staging) {
		if (slot.buffer && device)
			SDL_ReleaseGPUTransferBuffer(device, slot.buffer);
	}
	ctx->staging.clear();
	ctx->staging_frame.clear();
	for (auto& [key, set] : ctx->alt_vg_pipelines) {
		for (SDL_GPUGraphicsPipeline* pipeline : set.pipelines) {
			if (pipeline && device)
				SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
		}
	}
	ctx->alt_vg_pipelines.clear();
	for (auto& [key, pipeline] : ctx->geom_pipelines) {
		if (pipeline.pipeline && device)
			SDL_ReleaseGPUGraphicsPipeline(device, pipeline.pipeline);
	}

	// 特效资源：纹理池 + 三条管线 + 采样器。
	destroy_fx_resources(ctx);

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

// SDL_GPUSampleCount is an ordinal enum (1->0, 2->1, 4->2, 8->3): a raw
// sample count must be mapped, not cast, or the value is out of range and
// SDL silently degrades the texture.
SDL_GPUSampleCount to_sdl_sample_count(int count)
{
	switch (count)
	{
	case 1:
		return SDL_GPU_SAMPLECOUNT_1;
	case 2:
		return SDL_GPU_SAMPLECOUNT_2;
	case 4:
		return SDL_GPU_SAMPLECOUNT_4;
	case 8:
		return SDL_GPU_SAMPLECOUNT_8;
	default:
		return SDL_GPU_SAMPLECOUNT_1;
	}
}

vg_fbo_t vgpu_create_fbo(vgpu_ctx_t* ctx, int width, int height,
	SDL_Window* window, int depth, SDL_GPUTextureFormat color_format,
	int sample_count)
{
	vg_fbo_t fbo = {};
	fbo.ctx = ctx;
	fbo.window = window;
	fbo.width = static_cast<uint32_t>((std::max)(width, 1));
	fbo.height = static_cast<uint32_t>((std::max)(height, 1));
	fbo.display_size = { static_cast<int>(fbo.width), static_cast<int>(fbo.height) };
	fbo.upsize = true;
	// Window FBOs render into the swapchain, which was created with the
	// context's color format; only offscreen FBOs honor the overrides.
	fbo.color_format = (ctx && color_format != SDL_GPU_TEXTUREFORMAT_INVALID &&
		!window) ? color_format : ctx->color_format;
	// Effective MSAA count: explicit override (1 disables MSAA) or the
	// context's count.  Window FBOs always follow the context.
	fbo.samples = (ctx && (sample_count == 1 || sample_count == 2 ||
		sample_count == 4 || sample_count == 8) && !window)
		? to_sdl_sample_count(sample_count)
		: ctx->samples;

	if (!ctx || !ctx->device)
		return fbo;

	if (!window) {
		fbo.color_tex = create_texture(ctx->device,
			static_cast<int>(fbo.width), static_cast<int>(fbo.height),
			fbo.color_format,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		if (fbo.color_tex && fbo.color_tex->texture)
			SDL_SetGPUTextureName(ctx->device->gpu_device, fbo.color_tex->texture,
				"fbo_color_offscreen");
	}
	if (fbo.samples != SDL_GPU_SAMPLECOUNT_1) {
		fbo.color_msaa_tex = create_texture(ctx->device,
			static_cast<int>(fbo.width), static_cast<int>(fbo.height),
			fbo.color_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, fbo.samples);
		if (fbo.color_msaa_tex && fbo.color_msaa_tex->texture)
			SDL_SetGPUTextureName(ctx->device->gpu_device, fbo.color_msaa_tex->texture,
				"fbo_color_msaa");
	}
	// Optional depth attachment for the host's own 3D passes.  Only offscreen
	// FBOs carry one (swapchains come with their own depth handling); the MSAA
	// sample count must match the color target.  Failure leaves the FBO
	// without depth -- callers that require it check vgpu_fbo_depth_texture().
	if (!window && depth) {
		const SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		if (fbo.samples == SDL_GPU_SAMPLECOUNT_1 ||
			SDL_GPUTextureSupportsSampleCount(ctx->device->gpu_device,
				kDepthFormat, fbo.samples)) {
			fbo.depth_tex = create_texture(ctx->device,
				static_cast<int>(fbo.width), static_cast<int>(fbo.height),
				kDepthFormat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
				fbo.samples);
			if (fbo.depth_tex && fbo.depth_tex->texture)
				SDL_SetGPUTextureName(ctx->device->gpu_device,
					fbo.depth_tex->texture, "fbo_depth_offscreen");
		}
	}
	if (fbo.depth_tex)
		fbo.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	return fbo;
}

SDL_GPUTexture* vgpu_fbo_color_texture(vg_fbo_t* fbo)
{
	if (!fbo) return nullptr;
	if (fbo->swapchain) return fbo->swapchain;
	if (fbo->color_tex) return fbo->color_tex->texture;
	return nullptr;
}

SDL_GPUTexture* vgpu_fbo_depth_texture(vg_fbo_t* fbo)
{
	if (!fbo || !fbo->depth_tex) return nullptr;
	return fbo->depth_tex->texture;
}

SDL_GPUTextureFormat vgpu_fbo_color_format(vg_fbo_t* fbo)
{
	if (!fbo)
		return SDL_GPU_TEXTUREFORMAT_INVALID;
	if (fbo->color_format != SDL_GPU_TEXTUREFORMAT_INVALID)
		return fbo->color_format;
	return fbo->ctx ? fbo->ctx->color_format : SDL_GPU_TEXTUREFORMAT_INVALID;
}

SDL_GPUSampleCount vgpu_fbo_samples(vg_fbo_t* fbo)
{
	// vgpu_create_fbo/acquire_swapchain always store the effective count.
	return fbo ? fbo->samples : SDL_GPU_SAMPLECOUNT_1;
}

int vgpu_supports_color_format(vgpu_ctx_t* ctx, SDL_GPUTextureFormat format,
	SDL_GPUSampleCount samples)
{
	if (!ctx || !ctx->device || !ctx->device->gpu_device ||
		format == SDL_GPU_TEXTUREFORMAT_INVALID)
		return 0;
	if (!SDL_GPUTextureSupportsFormat(ctx->device->gpu_device, format,
		SDL_GPU_TEXTURETYPE_2D,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET))
		return 0;
	if (samples != SDL_GPU_SAMPLECOUNT_1 &&
		!SDL_GPUTextureSupportsSampleCount(ctx->device->gpu_device, format,
			samples))
		return 0;
	return 1;
}

int vgpu_effects_supported(vgpu_ctx_t* ctx)
{
	return (ctx && ctx->fx_supported) ? 1 : 0;
}

SDL_GPUCommandBuffer* vgpu_current_command_buffer(vgpu_ctx_t* ctx)
{
	return ctx ? ctx->current_cmd : nullptr;
}

SDL_GPUTextureFormat vgpu_color_format(vgpu_ctx_t* ctx)
{
	return ctx ? ctx->color_format : SDL_GPU_TEXTUREFORMAT_INVALID;
}

SDL_GPUDevice* vgpu_context_device(vgpu_ctx_t* ctx)
{
	return (ctx && ctx->device) ? ctx->device->gpu_device : nullptr;
}

SDL_GPUFence* vgpu_take_frame_fence(vgpu_ctx_t* ctx)
{
	if (!ctx || !ctx->last_frame_fence)
		return nullptr;
	SDL_GPUFence* fence = ctx->last_frame_fence;
	// 所有权转交给调用方（resize 退休队列）：对应 flight 保留槽位，之后由
	// 任意一次更晚提交的完成来证明它们可以归还。
	for (vgpu_ctx_t::staging_flight_t& flight : ctx->staging_flights) {
		if (flight.fence == fence) {
			flight.fence = nullptr;
			break;
		}
	}
	ctx->last_frame_fence = nullptr;
	return fence;
}

SDL_GPUSampleCount vgpu_context_samples(vgpu_ctx_t* ctx)
{
	return ctx ? ctx->samples : SDL_GPU_SAMPLECOUNT_1;
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
	if (fbo->depth_tex) {
		destroy_texture(fbo->depth_tex);
		fbo->depth_tex = nullptr;
	}
	fbo->depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
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

void vgpu_blit_texture(vgpu_ctx_t* ctx, SDL_GPUTexture* dst, int dst_w,
	int dst_h, SDL_GPUTexture* src, int src_w, int src_h)
{
	if (!ctx || !ctx->device || !ctx->device->gpu_device ||
		!dst || !src || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
		return;

	SDL_GPUBlitInfo blit = {};
	blit.source.texture = src;
	blit.source.w = static_cast<Uint32>(src_w);
	blit.source.h = static_cast<Uint32>(src_h);
	blit.destination.texture = dst;
	blit.destination.w = static_cast<Uint32>(dst_w);
	blit.destination.h = static_cast<Uint32>(dst_h);
	blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
	blit.filter = SDL_GPU_FILTER_LINEAR;
	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(
		ctx->device->gpu_device);
	if (!cmd)
		return;
	SDL_BlitGPUTexture(cmd, &blit);
	// BlitGPUTexture 只编码：此处提交。fence 供后续 retire 队列使用。
	submit_with_fence(ctx, cmd);
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
	// A pre-existing depth_format marks an FBO that wants its depth attachment
	// recreated along with the color targets; color_format carries the explicit
	// color-format override through the reset.  The effective sample count is
	// preserved as-is (window FBOs are refreshed by acquire_swapchain).
	const SDL_GPUTextureFormat want_depth = fbo->depth_format;
	const SDL_GPUTextureFormat want_color =
		(fbo->color_format != SDL_GPU_TEXTUREFORMAT_INVALID) ? fbo->color_format
			: ctx->color_format;
	const SDL_GPUSampleCount want_samples = fbo->samples;
	vgpu_destroy_fbo(fbo);
	fbo->ctx = ctx;
	fbo->window = window;
	fbo->width = static_cast<uint32_t>((std::max)(width, 1));
	fbo->height = static_cast<uint32_t>((std::max)(height, 1));
	fbo->display_size = { static_cast<int>(fbo->width), static_cast<int>(fbo->height) };
	fbo->upsize = true;
	fbo->color_format = want_color;
	fbo->samples = want_samples;

	if (!window) {
		fbo->color_tex = create_texture(ctx->device,
			static_cast<int>(fbo->width), static_cast<int>(fbo->height),
			want_color,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	}
	if (fbo->samples != SDL_GPU_SAMPLECOUNT_1) {
		fbo->color_msaa_tex = create_texture(ctx->device,
			static_cast<int>(fbo->width), static_cast<int>(fbo->height),
			want_color, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, fbo->samples);
	}
	if (!window && want_depth != SDL_GPU_TEXTUREFORMAT_INVALID) {
		fbo->depth_tex = create_texture(ctx->device,
			static_cast<int>(fbo->width), static_cast<int>(fbo->height),
			want_depth, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, fbo->samples);
		if (fbo->depth_tex)
			fbo->depth_format = want_depth;
	}
}

int vgpu_acquire_swapchain(vgpu_ctx_t* ctx, vg_fbo_t* fbo, int count, bool wait)
{
	if (!ctx || !ctx->device || !ctx->device->gpu_device || !fbo || count < 1)
		return 0;

	SDL_GPUDevice* device = ctx->device->gpu_device;
	if (ctx->current_cmd) {
		if (ctx->swapchain_acquired)
			submit_with_fence(ctx, ctx->current_cmd);
		else {
			staging_release_frame(ctx);
			SDL_CancelGPUCommandBuffer(ctx->current_cmd);
		}
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
		current->color_format = ctx->color_format;
		current->samples = ctx->samples;
		// 每次 acquire 都获取新的 swapchain，避免复用上一帧已提交的纹理。
		current->swapchain = nullptr;
		if (!ensure_swapchain(ctx, cmd, current, wait)) {
			current->swapchain = nullptr;
			continue;
		}
		++acquired;
	}

	if (acquired == 0) {
		staging_release_frame(ctx);
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

	staging_collect(ctx);

	if (count == 0 || !data) {
		if (ctx->current_cmd) {
			if (ctx->swapchain_acquired)
				submit_with_fence(ctx, ctx->current_cmd);
			else {
				staging_release_frame(ctx);
				SDL_CancelGPUCommandBuffer(ctx->current_cmd);
			}
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
			submit_with_fence(ctx, ctx->current_cmd);
		else {
			staging_release_frame(ctx);
			SDL_CancelGPUCommandBuffer(ctx->current_cmd);
		}
		ctx->current_cmd = nullptr;
		ctx->swapchain_acquired = false;
		for (size_t i = 0; i < count; ++i) {
			if (data[i].fbo)
				data[i].fbo->swapchain = nullptr;
		}
		return;
	}

	process_image_descs(ctx, ctx->current_cmd, items);
	upload_buffers(ctx, ctx->current_cmd, items);

	// 特效必须在主 pass 之前算完：每条带 vg_effect_t 的 fill/stroke 先渲染
	// mask、跑两趟 compute 模糊，结果按命令的 ref 槽位存好，绘制时查表合成。
	// ref 由录制阶段的 apply_effect_bounds() 分配，这里是同一个 vgcmd_t。
	// 用 item 指针做键：下面会按 FBO 顺序 stable_sort，按下标存会错位。
	std::unordered_map<const ovg_draw_data_t*, std::vector<fx_result_t>> fx_by_item;
	if (ctx->fx_supported) {
		for (ovg_draw_data_t* item : items) {
			int max_slot = -1;
			for (size_t i = 0; i < item->count; ++i) {
				gcmd_t& command = item->d[i];
				if (static_cast<cmd_type_e>(command.g.stype) != cmd_type_e::DRAW_VG)
					continue;
				vgcmd_t& cc = command.vg;
				if ((cc.type == 0 || cc.type == 1) && cc.state &&
					cc.state->effect.type != VG_EFFECT_NONE && cc.ref >= 0)
					max_slot = (std::max)(max_slot, static_cast<int>(cc.ref));
			}
			if (max_slot < 0)
				continue;
			std::vector<fx_result_t>& slot =
				fx_by_item.emplace(item, std::vector<fx_result_t>()).first->second;
			slot.assign(static_cast<size_t>(max_slot) + 1, fx_result_t{});
			for (size_t i = 0; i < item->count; ++i) {
				gcmd_t& command = item->d[i];
				if (static_cast<cmd_type_e>(command.g.stype) != cmd_type_e::DRAW_VG)
					continue;
				vgcmd_t& cc = command.vg;
				if ((cc.type == 0 || cc.type == 1) && cc.state &&
					cc.state->effect.type != VG_EFFECT_NONE && cc.ref >= 0)
					prepare_effect(ctx, ctx->current_cmd, item, &cc, item->fbo,
						&slot[static_cast<size_t>(cc.ref)]);
			}
		}
	}

	// Encode one pass per FBO, in order of first appearance, so a caller that
	// submits several draw data entries in one frame (offscreen HUD pass and
	// then a window composite, say) gets the passes in that order and SDL3's
	// automatic barriers connect them inside one command buffer.
	std::vector<vg_fbo_t*> pass_order;
	for (ovg_draw_data_t* item : items) {
		if (std::find(pass_order.begin(), pass_order.end(), item->fbo) == pass_order.end())
			pass_order.push_back(item->fbo);
	}
	std::stable_sort(items.begin(), items.end(), [&pass_order](const ovg_draw_data_t* a, const ovg_draw_data_t* b) {
		const size_t ia = std::find(pass_order.begin(), pass_order.end(), a->fbo) - pass_order.begin();
		const size_t ib = std::find(pass_order.begin(), pass_order.end(), b->fbo) - pass_order.begin();
		return ia < ib;
	});
	vg_fbo_t* current_fbo = nullptr;
	for (ovg_draw_data_t* item : items) {
		vg_fbo_t* fbo = item->fbo;
		if (fbo != current_fbo) {
			if (current_fbo)
				end_pass(ctx, current_fbo);
			current_fbo = fbo;
			begin_pass(ctx, fbo, fbo->clear);
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
			case cmd_type_e::DRAW_VG: {
				// ref 是 stencil ref 与 fx 槽位共用的字段，必须同时确认这条命令
				// 确实带着特效状态，否则裁剪/裁剪透传命令也会命中同一槽位。
				const fx_result_t* fx = nullptr;
				if (!fx_by_item.empty() && command.vg.ref >= 0 && command.vg.state &&
					command.vg.state->effect.type != VG_EFFECT_NONE) {
					auto it = fx_by_item.find(item);
					if (it != fx_by_item.end() &&
						static_cast<size_t>(command.vg.ref) < it->second.size())
						fx = &it->second[static_cast<size_t>(command.vg.ref)];
				}
				draw_vg(fbo, pass, &command.vg, &scissor, item->vg_offset, fx);
				break;
			}
			case cmd_type_e::DRAW_GEOM:
				draw_geom(fbo, pass, &command.g, item->geom_offset);
				break;
			}
		}
	}

	if (current_fbo)
		end_pass(ctx, current_fbo);

	submit_with_fence(ctx, ctx->current_cmd);
	ctx->current_cmd = nullptr;
	ctx->pass = nullptr;
	ctx->swapchain_acquired = false;

	for (size_t i = 0; i < count; ++i) {
		if (data[i].fbo)
			data[i].fbo->swapchain = nullptr;
	}

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
