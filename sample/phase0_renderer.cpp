#include "phase0_renderer.h"

#include "pch.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <glm/gtc/quaternion.hpp>

#include "shaders/spv_c/a_base3d.vert.h"
#include "shaders/spv_c/p0_scene.frag.h"
#include "shaders/spv_c/p0_fullscreen.vert.h"
#include "shaders/spv_c/p0_tonemap.frag.h"
#include "shaders/spv_c/p0_depth_preview.frag.h"
#include "shaders/spv_c/p0_depth_only.frag.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;

struct p0_vertex
{
	float x, y, z;
	float u, v;
	uint32_t color;
};

struct p0_vertex_uniform
{
	glm::mat4 mvp;
	uint32_t instance_pos;
	uint32_t padding[3];
};

struct p0_fragment_uniform
{
	glm::vec4 tint;
};

static_assert(sizeof(p0_vertex) == 24, "phase0 vertex layout must stay packed");

uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
	return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
}

SDL_GPUShader* make_shader(SDL_GPUDevice* device, SDL_GPUShaderStage stage,
	const uint32_t* code, size_t size, int samplers, int uniform_buffers)
{
	SDL_GPUShaderCreateInfo info = {};
	info.stage = stage;
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.code = reinterpret_cast<const Uint8*>(code);
	info.code_size = size;
	info.entrypoint = "main";
	info.num_samplers = samplers;
	info.num_uniform_buffers = uniform_buffers;
	SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
	if (!shader)
		SDL_Log("phase0: shader creation failed: %s", SDL_GetError());
	return shader;
}

bool upload_buffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, const void* data,
	uint32_t size)
{
	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = size;
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
	if (!transfer)
		return false;

	void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
	if (!mapped)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	std::memcpy(mapped, data, size);
	SDL_UnmapGPUTransferBuffer(device, transfer);

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
	if (!cmd)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	if (!copy)
	{
		SDL_CancelGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	SDL_GPUTransferBufferLocation src = {};
	src.transfer_buffer = transfer;
	SDL_GPUBufferRegion dst = {};
	dst.buffer = buffer;
	dst.size = size;
	SDL_UploadToGPUBuffer(copy, &src, &dst, false);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(device, transfer);
	return true;
}

bool upload_white_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture)
{
	const uint32_t white = 0xFFFFFFFFu;
	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = sizeof(white);
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
	if (!transfer)
		return false;
	void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
	if (!mapped)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	std::memcpy(mapped, &white, sizeof(white));
	SDL_UnmapGPUTransferBuffer(device, transfer);

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
	if (!cmd)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	if (!copy)
	{
		SDL_CancelGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	SDL_GPUTextureTransferInfo src = {};
	src.transfer_buffer = transfer;
	src.pixels_per_row = 1;
	src.rows_per_layer = 1;
	SDL_GPUTextureRegion dst = {};
	dst.texture = texture;
	dst.w = 1;
	dst.h = 1;
	dst.d = 1;
	SDL_UploadToGPUTexture(copy, &src, &dst, false);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(device, transfer);
	return true;
}

std::vector<p0_vertex> make_cube()
{
	const uint32_t colors[6] = {
		rgba(210, 77, 77),    // +Z
		rgba(77, 168, 210),   // -Z
		rgba(108, 196, 118),  // +X
		rgba(224, 169, 73),   // -X
		rgba(150, 112, 214),  // +Y
		rgba(95, 184, 177),   // -Y
	};
	const float p[6][4][3] = {
		{ {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} },
		{ { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} },
		{ { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1} },
		{ {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} },
		{ {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} },
		{ {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} },
	};
	const int order[6] = { 0, 1, 2, 0, 2, 3 };

	std::vector<p0_vertex> vertices;
	vertices.reserve(36);
	for (int face = 0; face < 6; ++face)
	{
		for (int i = 0; i < 6; ++i)
		{
			const float* v = p[face][order[i]];
			vertices.push_back({ v[0], v[1], v[2], 0.5f, 0.5f, colors[face] });
		}
	}
	return vertices;
}

SDL_GPUSampleCount sample_count_from_int(int samples)
{
	switch (samples)
	{
	case 2: return SDL_GPU_SAMPLECOUNT_2;
	case 4: return SDL_GPU_SAMPLECOUNT_4;
	case 8: return SDL_GPU_SAMPLECOUNT_8;
	default: return SDL_GPU_SAMPLECOUNT_1;
	}
}

int sample_count_to_int(SDL_GPUSampleCount samples)
{
	switch (samples)
	{
	case SDL_GPU_SAMPLECOUNT_2: return 2;
	case SDL_GPU_SAMPLECOUNT_4: return 4;
	case SDL_GPU_SAMPLECOUNT_8: return 8;
	default: return 1;
	}
}

SDL_GPUTexture* create_texture(SDL_GPUDevice* device, SDL_GPUTextureFormat format,
	SDL_GPUTextureUsageFlags usage, int width, int height,
	SDL_GPUSampleCount samples)
{
	SDL_GPUTextureCreateInfo info = {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = format;
	info.usage = usage;
	info.width = (Uint32)width;
	info.height = (Uint32)height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = samples;
	return SDL_CreateGPUTexture(device, &info);
}

} // namespace

struct phase0_renderer;

struct phase0_view
{
	phase0_renderer* renderer = nullptr;
	int width = 0;
	int height = 0;
	phase0_view_options options = {};
	bool targets_valid = false;
	bool depth_preview_supported = false;
	bool depth_sampleable = false;

	SDL_GPUTexture* display = nullptr;      // RGBA8, always bound to the UI
	SDL_GPUTexture* scene = nullptr;        // HDR or multisampled scene target
	SDL_GPUTexture* resolve = nullptr;      // HDR multisample resolve target
	SDL_GPUTexture* depth = nullptr;        // scene depth target
	SDL_GPUTexture* preview_depth = nullptr; // single-sample depth for MSAA preview
	SDL_GPUTexture* preview_color = nullptr; // RGBA8 depth visualisation

	bool has_camera = false;
	phase0_camera camera = {};

	bool object_transform_set = false;
	float object_position[3] = { 0.0f, 0.0f, 0.0f };
	float object_yaw = 0.0f;

	bool instances_set = false;
	std::vector<phase0_instance> instances;

	std::vector<phase0_grid_line> grid_lines;
	SDL_GPUBuffer* grid_buffer = nullptr;
	Uint32 grid_capacity = 0;
	Uint32 grid_vertex_count = 0;
};

namespace {

struct p0_pipeline_set
{
	SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUGraphicsPipeline* scene = nullptr;
	SDL_GPUGraphicsPipeline* grid = nullptr;
};

struct p0_camera_matrices
{
	glm::mat4 projection;
	glm::mat4 view;
};

} // namespace

struct phase0_renderer
{
	SDL_GPUDevice* device = nullptr;

	SDL_GPUShader* scene_vert = nullptr;
	SDL_GPUShader* scene_frag = nullptr;
	SDL_GPUShader* fullscreen_vert = nullptr;
	SDL_GPUShader* tonemap_frag = nullptr;
	SDL_GPUShader* depth_frag = nullptr;
	SDL_GPUShader* depth_only_frag = nullptr;

	SDL_GPUBuffer* vertex_buffer = nullptr;
	SDL_GPUBuffer* ghost_vertex_buffer = nullptr;
	SDL_GPUTexture* white_texture = nullptr;
	SDL_GPUSampler* linear_sampler = nullptr;
	SDL_GPUSampler* nearest_sampler = nullptr;

	SDL_GPUGraphicsPipeline* tonemap_pipeline = nullptr;
	SDL_GPUGraphicsPipeline* depth_preview_pipeline = nullptr;
	/* Single-sample, depth-only pass used to expose MSAA depth as a texture. */
	SDL_GPUGraphicsPipeline* depth_only_pipeline = nullptr;
	std::vector<p0_pipeline_set> pipelines;

	// R5-1 world-space UI billboard (texture borrowed from the host).
	SDL_GPUTexture* billboard_texture = nullptr;
	float billboard_pos[3] = { 0.0f, 0.0f, 0.0f };
	float billboard_w = 0.0f;
	float billboard_h = 0.0f;
	SDL_GPUBuffer* billboard_buffer = nullptr;
	Uint32 billboard_capacity = 0;

	std::vector<std::unique_ptr<phase0_view>> views;
	phase0_view* default_view = nullptr;
};

//---------------------------------------------------------------------------
// Shared pipeline/shader setup
//---------------------------------------------------------------------------

namespace {

SDL_GPUVertexInputState scene_vertex_input()
{
	static const SDL_GPUVertexAttribute attributes[] = {
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12 },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20 },
	};
	static const SDL_GPUVertexBufferDescription vertex_buffer = {
		0, sizeof(p0_vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0
	};

	SDL_GPUVertexInputState input = {};
	input.num_vertex_buffers = 1;
	input.vertex_buffer_descriptions = &vertex_buffer;
	input.num_vertex_attributes = 3;
	input.vertex_attributes = attributes;
	return input;
}

SDL_GPUGraphicsPipeline* create_scene_pipeline(phase0_renderer* r,
	SDL_GPUTextureFormat color_format, SDL_GPUSampleCount samples, bool grid)
{
	const SDL_GPUVertexInputState vertex_input = scene_vertex_input();

	SDL_GPUColorTargetDescription color_target = {};
	color_target.format = color_format;
	if (grid)
	{
		color_target.blend_state.enable_blend = true;
		color_target.blend_state.src_color_blendfactor =
			SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		color_target.blend_state.dst_color_blendfactor =
			SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		color_target.blend_state.src_alpha_blendfactor =
			SDL_GPU_BLENDFACTOR_ONE;
		color_target.blend_state.dst_alpha_blendfactor =
			SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	}
	color_target.blend_state.enable_color_write_mask = true;
	color_target.blend_state.color_write_mask =
		SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
		SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

	SDL_GPUDepthStencilState depth = {};
	depth.enable_depth_test = true;
	depth.enable_depth_write = !grid;
	depth.compare_op = SDL_GPU_COMPAREOP_LESS;
	depth.enable_stencil_test = false;

	SDL_GPURasterizerState rasterizer = {};
	rasterizer.fill_mode = SDL_GPU_FILLMODE_FILL;
	rasterizer.cull_mode = SDL_GPU_CULLMODE_NONE;
	rasterizer.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

	SDL_GPUMultisampleState multisample = {};
	multisample.sample_count = samples;

	SDL_GPUGraphicsPipelineCreateInfo info = {};
	info.vertex_shader = r->scene_vert;
	info.fragment_shader = r->scene_frag;
	info.vertex_input_state = vertex_input;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state = rasterizer;
	info.multisample_state = multisample;
	info.depth_stencil_state = depth;
	info.target_info.has_depth_stencil_target = true;
	info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	info.target_info.num_color_targets = 1;
	info.target_info.color_target_descriptions = &color_target;
	return SDL_CreateGPUGraphicsPipeline(r->device, &info);
}

SDL_GPUGraphicsPipeline* create_depth_only_pipeline(phase0_renderer* r)
{
	const SDL_GPUVertexInputState vertex_input = scene_vertex_input();

	SDL_GPUDepthStencilState depth = {};
	depth.enable_depth_test = true;
	depth.enable_depth_write = true;
	depth.compare_op = SDL_GPU_COMPAREOP_LESS;
	depth.enable_stencil_test = false;

	SDL_GPURasterizerState rasterizer = {};
	rasterizer.fill_mode = SDL_GPU_FILLMODE_FILL;
	rasterizer.cull_mode = SDL_GPU_CULLMODE_NONE;
	rasterizer.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

	SDL_GPUMultisampleState multisample = {};
	multisample.sample_count = SDL_GPU_SAMPLECOUNT_1;

	SDL_GPUGraphicsPipelineCreateInfo info = {};
	info.vertex_shader = r->scene_vert;
	info.fragment_shader = r->depth_only_frag;
	info.vertex_input_state = vertex_input;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state = rasterizer;
	info.multisample_state = multisample;
	info.depth_stencil_state = depth;
	info.target_info.has_depth_stencil_target = true;
	info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	info.target_info.num_color_targets = 0;
	return SDL_CreateGPUGraphicsPipeline(r->device, &info);
}

p0_pipeline_set* ensure_pipeline_set(phase0_renderer* r,
	SDL_GPUTextureFormat color_format, SDL_GPUSampleCount samples)
{
	for (p0_pipeline_set& set : r->pipelines)
	{
		if (set.color_format == color_format && set.samples == samples &&
			set.scene && set.grid)
			return &set;
	}

	p0_pipeline_set set;
	set.color_format = color_format;
	set.samples = samples;
	set.scene = create_scene_pipeline(r, color_format, samples, false);
	set.grid = create_scene_pipeline(r, color_format, samples, true);
	if (!set.scene || !set.grid)
	{
		SDL_Log("phase0: failed to create scene pipelines: %s", SDL_GetError());
		if (set.scene) SDL_ReleaseGPUGraphicsPipeline(r->device, set.scene);
		if (set.grid) SDL_ReleaseGPUGraphicsPipeline(r->device, set.grid);
		return nullptr;
	}
	r->pipelines.push_back(set);
	return &r->pipelines.back();
}

SDL_GPUGraphicsPipeline* create_fullscreen_pipeline(phase0_renderer* r,
	SDL_GPUShader* fragment_shader)
{
	SDL_GPUColorTargetDescription color_target = {};
	color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	color_target.blend_state.enable_blend = false;
	color_target.blend_state.enable_color_write_mask = true;
	color_target.blend_state.color_write_mask =
		SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
		SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

	SDL_GPURasterizerState rasterizer = {};
	rasterizer.fill_mode = SDL_GPU_FILLMODE_FILL;
	rasterizer.cull_mode = SDL_GPU_CULLMODE_NONE;
	rasterizer.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

	SDL_GPUMultisampleState multisample = {};
	multisample.sample_count = SDL_GPU_SAMPLECOUNT_1;

	SDL_GPUGraphicsPipelineCreateInfo info = {};
	info.vertex_shader = r->fullscreen_vert;
	info.fragment_shader = fragment_shader;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state = rasterizer;
	info.multisample_state = multisample;
	info.target_info.num_color_targets = 1;
	info.target_info.color_target_descriptions = &color_target;
	return SDL_CreateGPUGraphicsPipeline(r->device, &info);
}

//---------------------------------------------------------------------------
// Per-view attachments
//---------------------------------------------------------------------------

void release_view_targets(phase0_view* view)
{
	if (!view || !view->renderer || !view->renderer->device)
		return;
	SDL_GPUDevice* device = view->renderer->device;
	auto release = [device](SDL_GPUTexture*& texture) {
		if (texture)
		{
			SDL_ReleaseGPUTexture(device, texture);
			texture = nullptr;
		}
	};
	release(view->display);
	release(view->scene);
	release(view->resolve);
	release(view->depth);
	release(view->preview_depth);
	release(view->preview_color);
	view->targets_valid = false;
	view->depth_preview_supported = false;
}

bool create_view_targets(phase0_view* view)
{
	release_view_targets(view);
	if (!view || !view->renderer || view->width <= 0 || view->height <= 0)
		return false;

	phase0_renderer* r = view->renderer;
	SDL_GPUDevice* device = r->device;
	const int width = view->width;
	const int height = view->height;
	const bool hdr = view->options.hdr;
	const SDL_GPUSampleCount samples =
		sample_count_from_int(view->options.msaa_samples);
	const bool msaa = samples != SDL_GPU_SAMPLECOUNT_1;
	const SDL_GPUTextureFormat scene_format = hdr
		? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
		: SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

	view->display = create_texture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
		width, height, SDL_GPU_SAMPLECOUNT_1);
	if (!view->display)
		goto fail;

	if (hdr || msaa)
	{
		const SDL_GPUTextureUsageFlags usage =
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
			(msaa ? 0 : SDL_GPU_TEXTUREUSAGE_SAMPLER);
		view->scene = create_texture(device, scene_format, usage, width, height,
			samples);
		if (!view->scene)
			goto fail;
	}

	if (hdr && msaa)
	{
		view->resolve = create_texture(device, scene_format,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
			width, height, SDL_GPU_SAMPLECOUNT_1);
		if (!view->resolve)
			goto fail;
	}

	view->depth = create_texture(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
			(msaa ? 0 : SDL_GPU_TEXTUREUSAGE_SAMPLER),
		width, height, samples);
	view->depth_sampleable = view->depth != nullptr;
	if (!view->depth)
	{
		/* Sampling the depth texture is only needed by the preview.  Keep the
		 * scene renderable on backends that cannot expose D32 as a sampler. */
		view->depth = create_texture(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, width, height, samples);
		if (!view->depth)
			goto fail;
	}

	if (msaa)
	{
		view->preview_depth = create_texture(device,
			SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
				SDL_GPU_TEXTUREUSAGE_SAMPLER,
			width, height, SDL_GPU_SAMPLECOUNT_1);
	}

	view->preview_color = create_texture(device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
		width, height, SDL_GPU_SAMPLECOUNT_1);

	view->depth_preview_supported = view->preview_color != nullptr &&
		(msaa ? view->preview_depth != nullptr : view->depth_sampleable);
	if (view->preview_color)
	{
		/* The preview target exists even while disabled so toggling depth
		 * preview never invalidates the UI texture binding. */
	}
	else
	{
		SDL_Log("phase0: depth preview texture creation failed: %s",
			SDL_GetError());
	}

	view->targets_valid = true;
	return true;

fail:
	SDL_Log("phase0: view target creation failed: %s", SDL_GetError());
	release_view_targets(view);
	return false;
}

int supported_msaa_samples(SDL_GPUDevice* device, bool hdr, int requested)
{
	int want = std::clamp(requested, 1, 8);
	const SDL_GPUTextureFormat color_format = hdr
		? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
		: SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	for (int samples = want; samples >= 1; --samples)
	{
		if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
			continue;
		const SDL_GPUSampleCount count = sample_count_from_int(samples);
		if (samples == 1 ||
			(SDL_GPUTextureSupportsSampleCount(device, color_format, count) &&
				SDL_GPUTextureSupportsSampleCount(device,
					SDL_GPU_TEXTUREFORMAT_D32_FLOAT, count)))
			return samples;
	}
	return 1;
}

p0_camera_matrices view_camera(const phase0_view* view)
{
	p0_camera_matrices result = {};
	const float aspect = view->height > 0
		? (float)view->width / (float)view->height : 1.0f;
	float fov_deg = 50.0f;
	glm::vec3 eye(3.2f, 2.8f, 4.5f);
	glm::vec3 target(0.0f);
	if (view->has_camera)
	{
		fov_deg = view->camera.fov_deg;
		eye = glm::vec3(view->camera.eye[0], view->camera.eye[1],
			view->camera.eye[2]);
		target = glm::vec3(view->camera.target[0], view->camera.target[1],
			view->camera.target[2]);
	}
	if (glm::distance(eye, target) < 1e-4f)
		eye.z += 1.0f;

	result.projection = glm::perspectiveRH_ZO(glm::radians(fov_deg), aspect,
		kNearPlane, kFarPlane) *
		glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));
	result.view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
	return result;
}

glm::mat4 instance_model(const phase0_instance& instance)
{
	glm::quat rotation(instance.rotation[3], instance.rotation[0],
		instance.rotation[1], instance.rotation[2]);
	if (glm::dot(rotation, rotation) < 1e-8f)
		rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	rotation = glm::normalize(rotation);
	const glm::vec3 scale(
		instance.scale[0] > 0.0f ? instance.scale[0] : 1.0f,
		instance.scale[1] > 0.0f ? instance.scale[1] : 1.0f,
		instance.scale[2] > 0.0f ? instance.scale[2] : 1.0f);
	return glm::translate(glm::mat4(1.0f),
		glm::vec3(instance.position[0], instance.position[1],
			instance.position[2])) *
		glm::mat4_cast(rotation) *
		glm::scale(glm::mat4(1.0f), scale);
}

glm::mat4 legacy_model(const phase0_view* view, float time_seconds)
{
	if (view->object_transform_set)
	{
		return glm::translate(glm::mat4(1.0f),
			glm::vec3(view->object_position[0], view->object_position[1],
				view->object_position[2])) *
			glm::rotate(glm::mat4(1.0f), view->object_yaw,
				glm::vec3(0.0f, 1.0f, 0.0f));
	}
	return glm::rotate(glm::mat4(1.0f), time_seconds * 0.7f,
		glm::vec3(0.0f, 1.0f, 0.0f)) *
		glm::rotate(glm::mat4(1.0f), 0.35f, glm::vec3(1.0f, 0.0f, 0.0f));
}

void push_draw_uniforms(SDL_GPUCommandBuffer* cmd, const glm::mat4& mvp,
	const glm::vec4& tint, bool fragment_stage = true)
{
	p0_vertex_uniform vertex = {};
	vertex.mvp = mvp;
	SDL_PushGPUVertexUniformData(cmd, 0, &vertex, sizeof(vertex));
	if (!fragment_stage)
		return;
	p0_fragment_uniform fragment = {};
	fragment.tint = tint;
	SDL_PushGPUFragmentUniformData(cmd, 0, &fragment, sizeof(fragment));
}

void draw_scene_geometry(phase0_renderer* r, phase0_view* view,
	SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* cmd,
	const glm::mat4& view_projection, float time_seconds, bool depth_only)
{
	const glm::vec4 opaque_tint = view->options.hdr
		? glm::vec4(1.35f, 1.35f, 1.35f, 1.0f)
		: glm::vec4(1.0f);

	SDL_GPUBufferBinding binding = { r->vertex_buffer, 0 };
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

	if (!view->instances_set)
	{
		push_draw_uniforms(cmd, view_projection * legacy_model(view, time_seconds),
			opaque_tint, !depth_only);
		SDL_DrawGPUPrimitives(pass, 36, 1, 0, 0);
	}
	else
	{
		for (const phase0_instance& instance : view->instances)
		{
			if (instance.ghost)
				continue;
			push_draw_uniforms(cmd, view_projection * instance_model(instance),
				opaque_tint, !depth_only);
			SDL_DrawGPUPrimitives(pass, 36, 1, 0, 0);
		}
	}

	if (depth_only)
		return;

	/* Grid and ghosts are drawn by the caller through their blended
	 * pipelines; the depth-only pass stops after opaque geometry. */
}

void draw_view_grid(phase0_renderer* r, phase0_view* view,
	SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* cmd,
	const glm::mat4& view_projection, const p0_pipeline_set* set)
{
	if (view->grid_vertex_count == 0 || !view->grid_buffer || !set)
		return;
	SDL_BindGPUGraphicsPipeline(pass, set->grid);
	SDL_GPUBufferBinding binding = { view->grid_buffer, 0 };
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
	push_draw_uniforms(cmd, view_projection, glm::vec4(1.0f));
	SDL_DrawGPUPrimitives(pass, view->grid_vertex_count, 1, 0, 0);
}

void draw_view_ghosts(phase0_renderer* r, phase0_view* view,
	SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* cmd,
	const glm::mat4& view_projection, const p0_pipeline_set* set)
{
	if (!view->instances_set || !r->ghost_vertex_buffer || !set)
		return;
	bool any_ghost = false;
	for (const phase0_instance& instance : view->instances)
	{
		if (instance.ghost)
		{
			any_ghost = true;
			break;
		}
	}
	if (!any_ghost)
		return;

	SDL_BindGPUGraphicsPipeline(pass, set->grid);
	SDL_GPUBufferBinding binding = { r->ghost_vertex_buffer, 0 };
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
	for (const phase0_instance& instance : view->instances)
	{
		if (!instance.ghost)
			continue;
		push_draw_uniforms(cmd, view_projection * instance_model(instance),
			glm::vec4(1.0f));
		SDL_DrawGPUPrimitives(pass, 36, 1, 0, 0);
	}
}

// R5-1: draws the world-space UI billboard (see set_billboard) with the
// blended grid pipeline and the host texture bound instead of white.
void draw_view_billboard(phase0_renderer* r, phase0_view* view,
	SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* cmd,
	const glm::mat4& view_projection, const glm::mat4& view_matrix,
	const p0_pipeline_set* set)
{
	if (!r->billboard_texture || r->billboard_w <= 0.0f || r->billboard_h <= 0.0f
		|| !set)
		return;

	// Camera basis from the view rotation rows (orthonormal, world space).
	glm::vec3 right(view_matrix[0][0], view_matrix[1][0], view_matrix[2][0]);
	glm::vec3 up(view_matrix[0][1], view_matrix[1][1], view_matrix[2][1]);
	right = glm::normalize(right);
	up = glm::normalize(up);
	const glm::vec3 center(r->billboard_pos[0], r->billboard_pos[1],
		r->billboard_pos[2]);
	const glm::vec3 ex = right * (r->billboard_w * 0.5f);
	const glm::vec3 ey = up * (r->billboard_h * 0.5f);
	const uint32_t white = 0xFFFFFFFFu;

	// CCW from the camera; uv (0,0) is the top-left of the UI texture.
	const p0_vertex verts[6] = {
		{ (center - ex + ey).x, (center - ex + ey).y, (center - ex + ey).z,
			0.0f, 0.0f, white },
		{ (center - ex - ey).x, (center - ex - ey).y, (center - ex - ey).z,
			0.0f, 1.0f, white },
		{ (center + ex - ey).x, (center + ex - ey).y, (center + ex - ey).z,
			1.0f, 1.0f, white },
		{ (center - ex + ey).x, (center - ex + ey).y, (center - ex + ey).z,
			0.0f, 0.0f, white },
		{ (center + ex - ey).x, (center + ex - ey).y, (center + ex - ey).z,
			1.0f, 1.0f, white },
		{ (center + ex + ey).x, (center + ex + ey).y, (center + ex + ey).z,
			1.0f, 0.0f, white },
	};

	constexpr Uint32 kBytes = sizeof(verts);
	if (!r->billboard_buffer || r->billboard_capacity < kBytes)
	{
		if (r->billboard_buffer)
			SDL_ReleaseGPUBuffer(r->device, r->billboard_buffer);
		SDL_GPUBufferCreateInfo info = {};
		info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		info.size = 256;
		r->billboard_buffer = SDL_CreateGPUBuffer(r->device, &info);
		r->billboard_capacity = r->billboard_buffer ? 256 : 0;
		if (!r->billboard_buffer)
			return;
	}
	upload_buffer(r->device, r->billboard_buffer, verts, kBytes);

	SDL_BindGPUGraphicsPipeline(pass, set->grid);
	SDL_GPUBufferBinding binding = { r->billboard_buffer, 0 };
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
	SDL_GPUTextureSamplerBinding sampler = { r->billboard_texture,
		r->linear_sampler };
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
	push_draw_uniforms(cmd, view_projection, glm::vec4(1.0f));
	SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

void draw_scene_target(phase0_renderer* r, phase0_view* view,
	SDL_GPUCommandBuffer* cmd, const p0_camera_matrices& camera,
	const p0_pipeline_set* set, float time_seconds)
{
	const bool msaa = view->options.msaa_samples > 1;
	SDL_GPUTexture* target = view->scene ? view->scene : view->display;
	SDL_GPUTexture* resolve = view->resolve
		? view->resolve : (msaa ? view->display : nullptr);

	SDL_GPUColorTargetInfo color = {};
	color.texture = target;
	color.load_op = SDL_GPU_LOADOP_CLEAR;
	color.clear_color = view->options.hdr
		? SDL_FColor{ 0.01f, 0.015f, 0.025f, 1.0f }
		: SDL_FColor{ 0.09f, 0.11f, 0.15f, 1.0f };
	color.cycle = true;
	if (resolve)
	{
		color.store_op = SDL_GPU_STOREOP_RESOLVE;
		color.resolve_texture = resolve;
		color.resolve_mip_level = 0;
		color.resolve_layer = 0;
		color.cycle_resolve_texture = true;
	}
	else
		color.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPUDepthStencilTargetInfo depth = {};
	depth.texture = view->depth;
	depth.clear_depth = 1.0f;
	depth.load_op = SDL_GPU_LOADOP_CLEAR;
	depth.store_op = (!msaa && view->options.depth_preview)
		? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	depth.cycle = true;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
	if (!pass)
		return;

	SDL_GPUViewport viewport = {};
	viewport.x = 0.0f;
	viewport.y = (float)view->height;
	viewport.w = (float)view->width;
	viewport.h = -(float)view->height;
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &viewport);

	SDL_BindGPUGraphicsPipeline(pass, set->scene);
	SDL_GPUTextureSamplerBinding sampler = { r->white_texture,
		r->linear_sampler };
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);

	const glm::mat4 view_projection = camera.projection * camera.view;
	draw_scene_geometry(r, view, pass, cmd, view_projection, time_seconds, false);
	draw_view_grid(r, view, pass, cmd, view_projection, set);
	draw_view_ghosts(r, view, pass, cmd, view_projection, set);
	draw_view_billboard(r, view, pass, cmd, view_projection, camera.view, set);

	SDL_EndGPURenderPass(pass);
}

void draw_hdr_tonemap(phase0_renderer* r, phase0_view* view,
	SDL_GPUCommandBuffer* cmd)
{
	SDL_GPUTexture* source = view->resolve ? view->resolve : view->scene;
	if (!source || !r->tonemap_pipeline)
		return;

	SDL_GPUColorTargetInfo color = {};
	color.texture = view->display;
	color.load_op = SDL_GPU_LOADOP_DONT_CARE;
	color.store_op = SDL_GPU_STOREOP_STORE;
	color.cycle = true;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
	if (!pass)
		return;

	SDL_GPUViewport viewport = {};
	viewport.w = (float)view->width;
	viewport.h = (float)view->height;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &viewport);
	SDL_BindGPUGraphicsPipeline(pass, r->tonemap_pipeline);

	SDL_GPUTextureSamplerBinding sampler = { source, r->linear_sampler };
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
	const glm::vec4 params(1.0f, 2.2f, 0.0f, 0.0f);
	SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
}

void draw_depth_only(phase0_renderer* r, phase0_view* view,
	SDL_GPUCommandBuffer* cmd, const p0_camera_matrices& camera,
	float time_seconds)
{
	if (!view->preview_depth || !r->depth_only_pipeline)
		return;

	SDL_GPUDepthStencilTargetInfo depth = {};
	depth.texture = view->preview_depth;
	depth.clear_depth = 1.0f;
	depth.load_op = SDL_GPU_LOADOP_CLEAR;
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	depth.cycle = true;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, nullptr, 0, &depth);
	if (!pass)
		return;

	SDL_GPUViewport viewport = {};
	viewport.y = (float)view->height;
	viewport.w = (float)view->width;
	viewport.h = -(float)view->height;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &viewport);
	SDL_BindGPUGraphicsPipeline(pass, r->depth_only_pipeline);
	/* The depth-only fragment shader has no outputs and no samplers, so the
	 * pass binds vertex uniforms only. */
	draw_scene_geometry(r, view, pass, cmd, camera.projection * camera.view,
		time_seconds, true);
	SDL_EndGPURenderPass(pass);
}

void draw_depth_preview(phase0_renderer* r, phase0_view* view,
	SDL_GPUCommandBuffer* cmd)
{
	if (!view->options.depth_preview || !view->preview_color)
		return;
	SDL_GPUTexture* source = view->options.msaa_samples > 1
		? view->preview_depth : view->depth;
	if (!source || !r->depth_preview_pipeline)
		return;

	SDL_GPUColorTargetInfo color = {};
	color.texture = view->preview_color;
	color.load_op = SDL_GPU_LOADOP_DONT_CARE;
	color.store_op = SDL_GPU_STOREOP_STORE;
	color.cycle = true;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
	if (!pass)
		return;

	SDL_GPUViewport viewport = {};
	viewport.w = (float)view->width;
	viewport.h = (float)view->height;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &viewport);
	SDL_BindGPUGraphicsPipeline(pass, r->depth_preview_pipeline);

	SDL_GPUTextureSamplerBinding sampler = { source, r->nearest_sampler };
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
	const glm::vec4 params(kNearPlane, kFarPlane, 0.0f, 0.0f);
	SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
}

void render_view(phase0_renderer* r, phase0_view* view,
	SDL_GPUCommandBuffer* cmd, float time_seconds)
{
	if (!view->targets_valid)
		return;
	const SDL_GPUTextureFormat format =
		view->options.hdr
			? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
			: SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	p0_pipeline_set* set = ensure_pipeline_set(r, format,
		sample_count_from_int(view->options.msaa_samples));
	if (!set)
		return;

	const p0_camera_matrices camera = view_camera(view);
	draw_scene_target(r, view, cmd, camera, set, time_seconds);
	if (view->options.hdr)
		draw_hdr_tonemap(r, view, cmd);
	if (view->options.depth_preview)
	{
		if (view->options.msaa_samples > 1)
			draw_depth_only(r, view, cmd, camera, time_seconds);
		draw_depth_preview(r, view, cmd);
	}
}

std::vector<p0_vertex> build_grid_vertices(
	const std::vector<phase0_grid_line>& lines)
{
	std::vector<p0_vertex> vertices;
	vertices.reserve(lines.size() * 6);
	for (const phase0_grid_line& line : lines)
	{
		const glm::vec3 a(line.a[0], line.a[1], line.a[2]);
		const glm::vec3 b(line.b[0], line.b[1], line.b[2]);
		const glm::vec3 dir = b - a;
		if (glm::length(dir) < 1e-6f)
			continue;
		glm::vec3 perpendicular = glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(perpendicular) < 1e-6f)
			perpendicular = glm::vec3(1.0f, 0.0f, 0.0f);
		perpendicular = glm::normalize(perpendicular) *
			(std::max(line.width, 1e-5f) * 0.5f);

		const glm::vec3 c0 = a - perpendicular;
		const glm::vec3 c1 = a + perpendicular;
		const glm::vec3 c2 = b + perpendicular;
		const glm::vec3 c3 = b - perpendicular;
		const uint32_t color = line.color;
		auto push = [&](const glm::vec3& p) {
			vertices.push_back({ p.x, p.y, p.z, 0.5f, 0.5f, color });
		};
		push(c0); push(c1); push(c2);
		push(c0); push(c2); push(c3);
	}
	return vertices;
}

bool ensure_grid_buffer(phase0_view* view, Uint32 bytes)
{
	if (!view || !view->renderer || bytes == 0)
		return false;
	SDL_GPUDevice* device = view->renderer->device;
	if (view->grid_buffer && view->grid_capacity >= bytes)
		return true;
	if (view->grid_buffer)
	{
		SDL_ReleaseGPUBuffer(device, view->grid_buffer);
		view->grid_buffer = nullptr;
		view->grid_capacity = 0;
	}
	Uint32 capacity = 256;
	while (capacity < bytes)
		capacity *= 2;
	SDL_GPUBufferCreateInfo info = {};
	info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	info.size = capacity;
	view->grid_buffer = SDL_CreateGPUBuffer(device, &info);
	if (!view->grid_buffer)
	{
		SDL_Log("phase0: failed to create grid buffer: %s", SDL_GetError());
		return false;
	}
	view->grid_capacity = capacity;
	return true;
}

} // namespace

phase0_renderer* phase0_renderer_create(SDL_GPUDevice* device)
{
	if (!device)
		return nullptr;
	const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
	if (!(formats & SDL_GPU_SHADERFORMAT_SPIRV))
	{
		SDL_Log("phase0: SPIR-V shader support is required");
		return nullptr;
	}

	auto* r = new phase0_renderer();
	r->device = device;

	r->scene_vert = make_shader(device, SDL_GPU_SHADERSTAGE_VERTEX,
		a_base3d_vert, sizeof(a_base3d_vert), 0, 1);
	r->scene_frag = make_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		p0_scene_frag, sizeof(p0_scene_frag), 1, 1);
	r->fullscreen_vert = make_shader(device, SDL_GPU_SHADERSTAGE_VERTEX,
		p0_fullscreen_vert, sizeof(p0_fullscreen_vert), 0, 0);
	r->tonemap_frag = make_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		p0_tonemap_frag, sizeof(p0_tonemap_frag), 1, 1);
	r->depth_frag = make_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		p0_depth_preview_frag, sizeof(p0_depth_preview_frag), 1, 1);
	r->depth_only_frag = make_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		p0_depth_only_frag, sizeof(p0_depth_only_frag), 0, 0);
	if (!r->scene_vert || !r->scene_frag || !r->fullscreen_vert ||
		!r->tonemap_frag || !r->depth_frag || !r->depth_only_frag)
	{
		phase0_renderer_destroy(r);
		return nullptr;
	}

	r->tonemap_pipeline = create_fullscreen_pipeline(r, r->tonemap_frag);
	r->depth_preview_pipeline = create_fullscreen_pipeline(r, r->depth_frag);
	r->depth_only_pipeline = create_depth_only_pipeline(r);
	if (!r->tonemap_pipeline || !r->depth_preview_pipeline ||
		!r->depth_only_pipeline)
	{
		SDL_Log("phase0: failed to create fullscreen/depth pipelines: %s",
			SDL_GetError());
		phase0_renderer_destroy(r);
		return nullptr;
	}

	const std::vector<p0_vertex> vertices = make_cube();
	SDL_GPUBufferCreateInfo buffer_info = {};
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buffer_info.size = (Uint32)(vertices.size() * sizeof(p0_vertex));
	r->vertex_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
	if (!r->vertex_buffer ||
		!upload_buffer(device, r->vertex_buffer, vertices.data(),
			buffer_info.size))
	{
		phase0_renderer_destroy(r);
		return nullptr;
	}

	/* A translucent copy of the cube feeds the drop-preview pass; failure is
	 * non-fatal because the host still has the ground marker. */
	std::vector<p0_vertex> ghost_vertices = make_cube();
	for (p0_vertex& vertex : ghost_vertices)
		vertex.color = (vertex.color & 0x00FFFFFFu) | (112u << 24);
	r->ghost_vertex_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
	if (!r->ghost_vertex_buffer ||
		!upload_buffer(device, r->ghost_vertex_buffer, ghost_vertices.data(),
			buffer_info.size))
	{
		if (r->ghost_vertex_buffer)
		{
			SDL_ReleaseGPUBuffer(device, r->ghost_vertex_buffer);
			r->ghost_vertex_buffer = nullptr;
		}
	}

	SDL_GPUTextureCreateInfo texture_info = {};
	texture_info.type = SDL_GPU_TEXTURETYPE_2D;
	texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	texture_info.width = 1;
	texture_info.height = 1;
	texture_info.layer_count_or_depth = 1;
	texture_info.num_levels = 1;
	texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	r->white_texture = SDL_CreateGPUTexture(device, &texture_info);
	if (!r->white_texture || !upload_white_texture(device, r->white_texture))
	{
		phase0_renderer_destroy(r);
		return nullptr;
	}

	SDL_GPUSamplerCreateInfo sampler_info = {};
	sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
	sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
	sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	r->linear_sampler = SDL_CreateGPUSampler(device, &sampler_info);
	if (!r->linear_sampler)
	{
		phase0_renderer_destroy(r);
		return nullptr;
	}

	sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
	sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
	r->nearest_sampler = SDL_CreateGPUSampler(device, &sampler_info);
	if (!r->nearest_sampler)
	{
		phase0_renderer_destroy(r);
		return nullptr;
	}

	auto default_view = std::make_unique<phase0_view>();
	default_view->renderer = r;
	default_view->options.hdr = false;
	default_view->options.msaa_samples = 1;
	default_view->options.depth_preview = false;
	r->default_view = default_view.get();
	r->views.push_back(std::move(default_view));
	return r;
}

void phase0_renderer_destroy(phase0_renderer* r)
{
	if (!r) return;
	if (r->device)
	{
		SDL_WaitForGPUIdle(r->device);
		for (std::unique_ptr<phase0_view>& view : r->views)
		{
			if (!view)
				continue;
			release_view_targets(view.get());
			if (view->grid_buffer)
				SDL_ReleaseGPUBuffer(r->device, view->grid_buffer);
			view->grid_buffer = nullptr;
			view->grid_capacity = 0;
		}
		r->views.clear();
		r->default_view = nullptr;

		for (p0_pipeline_set& set : r->pipelines)
		{
			if (set.scene) SDL_ReleaseGPUGraphicsPipeline(r->device, set.scene);
			if (set.grid) SDL_ReleaseGPUGraphicsPipeline(r->device, set.grid);
		}
		r->pipelines.clear();
		if (r->tonemap_pipeline)
			SDL_ReleaseGPUGraphicsPipeline(r->device, r->tonemap_pipeline);
		if (r->depth_preview_pipeline)
			SDL_ReleaseGPUGraphicsPipeline(r->device, r->depth_preview_pipeline);
		if (r->depth_only_pipeline)
			SDL_ReleaseGPUGraphicsPipeline(r->device, r->depth_only_pipeline);
		if (r->nearest_sampler)
			SDL_ReleaseGPUSampler(r->device, r->nearest_sampler);
		if (r->linear_sampler)
			SDL_ReleaseGPUSampler(r->device, r->linear_sampler);
		if (r->white_texture)
			SDL_ReleaseGPUTexture(r->device, r->white_texture);
		if (r->ghost_vertex_buffer)
			SDL_ReleaseGPUBuffer(r->device, r->ghost_vertex_buffer);
		if (r->billboard_buffer)
			SDL_ReleaseGPUBuffer(r->device, r->billboard_buffer);
		if (r->vertex_buffer)
			SDL_ReleaseGPUBuffer(r->device, r->vertex_buffer);
		if (r->depth_only_frag)
			SDL_ReleaseGPUShader(r->device, r->depth_only_frag);
		if (r->depth_frag)
			SDL_ReleaseGPUShader(r->device, r->depth_frag);
		if (r->tonemap_frag)
			SDL_ReleaseGPUShader(r->device, r->tonemap_frag);
		if (r->fullscreen_vert)
			SDL_ReleaseGPUShader(r->device, r->fullscreen_vert);
		if (r->scene_frag)
			SDL_ReleaseGPUShader(r->device, r->scene_frag);
		if (r->scene_vert)
			SDL_ReleaseGPUShader(r->device, r->scene_vert);
	}
	delete r;
}

phase0_view* phase0_renderer_default_view(phase0_renderer* r)
{
	return r ? r->default_view : nullptr;
}

phase0_view* phase0_renderer_create_view(phase0_renderer* r)
{
	if (!r)
		return nullptr;
	auto view = std::make_unique<phase0_view>();
	view->renderer = r;
	view->options.hdr = false;
	view->options.msaa_samples = 1;
	view->options.depth_preview = false;
	phase0_view* result = view.get();
	r->views.push_back(std::move(view));
	return result;
}

bool phase0_renderer_destroy_view(phase0_renderer* r, phase0_view* view)
{
	if (!r || !view || view == r->default_view)
		return false;
	auto it = std::find_if(r->views.begin(), r->views.end(),
		[view](const std::unique_ptr<phase0_view>& candidate) {
			return candidate.get() == view;
		});
	if (it == r->views.end())
		return false;
	SDL_WaitForGPUIdle(r->device);
	release_view_targets(view);
	if (view->grid_buffer)
		SDL_ReleaseGPUBuffer(r->device, view->grid_buffer);
	r->views.erase(it);
	return true;
}

bool phase0_view_resize(phase0_view* view, int width, int height)
{
	if (!view || !view->renderer || width <= 0 || height <= 0)
		return false;
	if (view->width == width && view->height == height && view->targets_valid)
		return false;
	SDL_WaitForGPUIdle(view->renderer->device);
	view->width = width;
	view->height = height;
	create_view_targets(view);
	return true;
}

int phase0_view_width(const phase0_view* view)
{
	return view ? view->width : 0;
}

int phase0_view_height(const phase0_view* view)
{
	return view ? view->height : 0;
}

SDL_GPUTexture* phase0_view_color_texture(phase0_view* view)
{
	return view ? view->display : nullptr;
}

SDL_GPUTexture* phase0_view_depth_texture(phase0_view* view)
{
	if (!view || view->options.msaa_samples > 1)
		return nullptr;
	return view->depth;
}

SDL_GPUTexture* phase0_view_depth_preview_texture(phase0_view* view)
{
	return view ? view->preview_color : nullptr;
}

phase0_view_options phase0_view_get_options(const phase0_view* view)
{
	return view ? view->options : phase0_view_options{};
}

bool phase0_view_set_options(phase0_view* view,
	const phase0_view_options* options)
{
	if (!view || !view->renderer || !options)
		return false;

	phase0_view_options normalized = *options;
	normalized.msaa_samples = supported_msaa_samples(view->renderer->device,
		normalized.hdr, normalized.msaa_samples);

	const bool attachments_changed =
		normalized.hdr != view->options.hdr ||
		normalized.msaa_samples != view->options.msaa_samples;
	view->options = normalized;
	if (!attachments_changed)
		return false;

	SDL_WaitForGPUIdle(view->renderer->device);
	if (view->width > 0 && view->height > 0)
		create_view_targets(view);
	else
		release_view_targets(view);
	return true;
}

bool phase0_view_set_hdr(phase0_view* view, bool hdr)
{
	phase0_view_options options = phase0_view_get_options(view);
	options.hdr = hdr;
	return phase0_view_set_options(view, &options);
}

bool phase0_view_set_msaa_samples(phase0_view* view, int samples)
{
	phase0_view_options options = phase0_view_get_options(view);
	options.msaa_samples = samples;
	return phase0_view_set_options(view, &options);
}

bool phase0_view_set_depth_preview(phase0_view* view, bool enabled)
{
	phase0_view_options options = phase0_view_get_options(view);
	options.depth_preview = enabled;
	return phase0_view_set_options(view, &options);
}

void phase0_view_set_camera(phase0_view* view, const phase0_camera* camera)
{
	if (!view) return;
	if (camera)
	{
		view->camera = *camera;
		view->has_camera = true;
	}
	else
	{
		view->camera = phase0_camera{};
		view->has_camera = false;
	}
}

void phase0_view_set_grid(phase0_view* view, const phase0_grid_line* lines,
	int count)
{
	if (!view) return;
	view->grid_lines.clear();
	if (!lines || count <= 0)
		return;
	view->grid_lines.assign(lines, lines + count);
}

void phase0_view_set_object_transform(phase0_view* view,
	const float position[3], float yaw_radians)
{
	if (!view) return;
	view->object_transform_set = true;
	if (position)
	{
		view->object_position[0] = position[0];
		view->object_position[1] = position[1];
		view->object_position[2] = position[2];
	}
	else
	{
		view->object_position[0] = 0.0f;
		view->object_position[1] = 0.0f;
		view->object_position[2] = 0.0f;
	}
	view->object_yaw = yaw_radians;
}

void phase0_view_set_instances(phase0_view* view,
	const phase0_instance* instances, int count)
{
	if (!view) return;
	view->instances_set = true;
	view->instances.clear();
	if (!instances || count <= 0)
		return;
	view->instances.assign(instances, instances + count);
}

void phase0_renderer_render(phase0_renderer* r, float time_seconds)
{
	if (!r)
		return;

	/* Upload each view's grid into its own buffer before recording the frame.
	 * Views share the cube/ghost buffers and pipelines but never overwrite
	 * another view's geometry or attachments. */
	for (std::unique_ptr<phase0_view>& entry : r->views)
	{
		phase0_view* view = entry.get();
		view->grid_vertex_count = 0;
		if (!view->targets_valid || view->grid_lines.empty())
			continue;
		std::vector<p0_vertex> vertices = build_grid_vertices(view->grid_lines);
		if (vertices.empty())
			continue;
		const Uint32 bytes = (Uint32)(vertices.size() * sizeof(p0_vertex));
		if (ensure_grid_buffer(view, bytes) &&
			upload_buffer(r->device, view->grid_buffer, vertices.data(), bytes))
			view->grid_vertex_count = (Uint32)vertices.size();
	}

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(r->device);
	if (!cmd)
		return;
	for (std::unique_ptr<phase0_view>& entry : r->views)
		render_view(r, entry.get(), cmd, time_seconds);
	SDL_SubmitGPUCommandBuffer(cmd);
}

//---------------------------------------------------------------------------
// Legacy single-view API (operates on the default view)
//---------------------------------------------------------------------------

bool phase0_renderer_resize(phase0_renderer* r, int width, int height)
{
	return phase0_view_resize(phase0_renderer_default_view(r), width, height);
}

SDL_GPUTexture* phase0_renderer_color_texture(phase0_renderer* r)
{
	return phase0_view_color_texture(phase0_renderer_default_view(r));
}

int phase0_renderer_width(phase0_renderer* r)
{
	return phase0_view_width(phase0_renderer_default_view(r));
}

int phase0_renderer_height(phase0_renderer* r)
{
	return phase0_view_height(phase0_renderer_default_view(r));
}

void phase0_renderer_set_billboard(phase0_renderer* r,
	SDL_GPUTexture* texture, const float position[3], float width, float height)
{
	if (!r)
		return;
	r->billboard_texture = texture;
	if (position)
	{
		r->billboard_pos[0] = position[0];
		r->billboard_pos[1] = position[1];
		r->billboard_pos[2] = position[2];
	}
	r->billboard_w = width;
	r->billboard_h = height;
}

void phase0_renderer_set_camera(phase0_renderer* r, const phase0_camera* camera)
{
	phase0_view_set_camera(phase0_renderer_default_view(r), camera);
}

void phase0_renderer_set_grid(phase0_renderer* r, const phase0_grid_line* lines,
	int count)
{
	phase0_view_set_grid(phase0_renderer_default_view(r), lines, count);
}

void phase0_renderer_set_object_transform(phase0_renderer* r,
	const float position[3], float yaw_radians)
{
	phase0_view_set_object_transform(phase0_renderer_default_view(r), position,
		yaw_radians);
}

void phase0_renderer_set_instances(phase0_renderer* r,
	const phase0_instance* instances, int count)
{
	phase0_view_set_instances(phase0_renderer_default_view(r), instances, count);
}
