#include "phase0_renderer.h"

#include "pch.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "shaders/spv_c/a_base3d.vert.h"
#include "shaders/spv_c/a_base3d.frag.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct p0_vertex
{
	float x, y, z;
	float u, v;
	uint32_t color;
};

struct p0_uniform
{
	glm::mat4 mvp;
	uint32_t instance_pos;
	uint32_t padding[3];
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

bool upload_buffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, const void* data, uint32_t size)
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

} // namespace

struct phase0_renderer
{
	SDL_GPUDevice* device = nullptr;
	SDL_GPUShader* vert = nullptr;
	SDL_GPUShader* frag = nullptr;
	SDL_GPUGraphicsPipeline* pipeline = nullptr;
	SDL_GPUBuffer* vertex_buffer = nullptr;
	SDL_GPUTexture* white_texture = nullptr;
	SDL_GPUSampler* white_sampler = nullptr;
	SDL_GPUTexture* color = nullptr;
	SDL_GPUTexture* depth = nullptr;
	int width = 0;
	int height = 0;
};

static void release_target(phase0_renderer* r)
{
	if (!r || !r->device) return;
	if (r->color)
	{
		SDL_ReleaseGPUTexture(r->device, r->color);
		r->color = nullptr;
	}
	if (r->depth)
	{
		SDL_ReleaseGPUTexture(r->device, r->depth);
		r->depth = nullptr;
	}
	r->width = 0;
	r->height = 0;
}

static bool create_target(phase0_renderer* r, int width, int height)
{
	SDL_GPUTextureCreateInfo color_info = {};
	color_info.type = SDL_GPU_TEXTURETYPE_2D;
	color_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	color_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	color_info.width = (Uint32)width;
	color_info.height = (Uint32)height;
	color_info.layer_count_or_depth = 1;
	color_info.num_levels = 1;
	color_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	r->color = SDL_CreateGPUTexture(r->device, &color_info);
	if (!r->color)
	{
		SDL_Log("phase0: failed to create color target: %s", SDL_GetError());
		return false;
	}

	SDL_GPUTextureCreateInfo depth_info = {};
	depth_info.type = SDL_GPU_TEXTURETYPE_2D;
	depth_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	depth_info.width = (Uint32)width;
	depth_info.height = (Uint32)height;
	depth_info.layer_count_or_depth = 1;
	depth_info.num_levels = 1;
	depth_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	r->depth = SDL_CreateGPUTexture(r->device, &depth_info);
	if (!r->depth)
	{
		SDL_Log("phase0: failed to create depth target: %s", SDL_GetError());
		release_target(r);
		return false;
	}
	r->width = width;
	r->height = height;
	return true;
}

static bool create_pipeline(phase0_renderer* r)
{
	static const SDL_GPUVertexAttribute attributes[] = {
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12 },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20 },
	};

	SDL_GPUVertexBufferDescription vertex_buffer = {};
	vertex_buffer.slot = 0;
	vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vertex_buffer.pitch = sizeof(p0_vertex);

	SDL_GPUVertexInputState vertex_input = {};
	vertex_input.num_vertex_buffers = 1;
	vertex_input.vertex_buffer_descriptions = &vertex_buffer;
	vertex_input.num_vertex_attributes = 3;
	vertex_input.vertex_attributes = attributes;

	SDL_GPUColorTargetDescription color_target = {};
	color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	color_target.blend_state = {};
	color_target.blend_state.enable_blend = false;
	color_target.blend_state.enable_color_write_mask = true;
	color_target.blend_state.color_write_mask =
		SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
		SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

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
	info.vertex_shader = r->vert;
	info.fragment_shader = r->frag;
	info.vertex_input_state = vertex_input;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state = rasterizer;
	info.multisample_state = multisample;
	info.depth_stencil_state = depth;
	info.target_info.has_depth_stencil_target = true;
	info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	info.target_info.num_color_targets = 1;
	info.target_info.color_target_descriptions = &color_target;

	r->pipeline = SDL_CreateGPUGraphicsPipeline(r->device, &info);
	if (!r->pipeline)
		SDL_Log("phase0: failed to create pipeline: %s", SDL_GetError());
	return r->pipeline != nullptr;
}

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

	r->vert = make_shader(device, SDL_GPU_SHADERSTAGE_VERTEX,
		a_base3d_vert, sizeof(a_base3d_vert), 0, 1);
	r->frag = make_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
		a_base3d_frag, sizeof(a_base3d_frag), 1, 0);
	if (!r->vert || !r->frag || !create_pipeline(r))
	{
		phase0_renderer_destroy(r);
		return nullptr;
	}

	const std::vector<p0_vertex> vertices = make_cube();
	SDL_GPUBufferCreateInfo buffer_info = {};
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buffer_info.size = (Uint32)(vertices.size() * sizeof(p0_vertex));
	r->vertex_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
	if (!r->vertex_buffer ||
		!upload_buffer(device, r->vertex_buffer, vertices.data(), buffer_info.size))
	{
		phase0_renderer_destroy(r);
		return nullptr;
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
	r->white_sampler = SDL_CreateGPUSampler(device, &sampler_info);
	if (!r->white_sampler)
	{
		phase0_renderer_destroy(r);
		return nullptr;
	}
	return r;
}

void phase0_renderer_destroy(phase0_renderer* r)
{
	if (!r) return;
	if (r->device)
	{
		SDL_WaitForGPUIdle(r->device);
		release_target(r);
		if (r->white_sampler) SDL_ReleaseGPUSampler(r->device, r->white_sampler);
		if (r->white_texture) SDL_ReleaseGPUTexture(r->device, r->white_texture);
		if (r->vertex_buffer) SDL_ReleaseGPUBuffer(r->device, r->vertex_buffer);
		if (r->pipeline) SDL_ReleaseGPUGraphicsPipeline(r->device, r->pipeline);
		if (r->vert) SDL_ReleaseGPUShader(r->device, r->vert);
		if (r->frag) SDL_ReleaseGPUShader(r->device, r->frag);
	}
	delete r;
}

bool phase0_renderer_resize(phase0_renderer* r, int width, int height)
{
	if (!r || width <= 0 || height <= 0)
		return false;
	if (r->color && r->depth && width == r->width && height == r->height)
		return false;
	SDL_WaitForGPUIdle(r->device);
	release_target(r);
	return create_target(r, width, height);
}

SDL_GPUTexture* phase0_renderer_color_texture(phase0_renderer* r)
{
	return r ? r->color : nullptr;
}

int phase0_renderer_width(phase0_renderer* r)
{
	return r ? r->width : 0;
}

int phase0_renderer_height(phase0_renderer* r)
{
	return r ? r->height : 0;
}

void phase0_renderer_render(phase0_renderer* r, float time_seconds)
{
	if (!r || !r->pipeline || !r->color || !r->depth)
		return;

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(r->device);
	if (!cmd)
		return;

	SDL_GPUColorTargetInfo color = {};
	color.texture = r->color;
	color.load_op = SDL_GPU_LOADOP_CLEAR;
	color.store_op = SDL_GPU_STOREOP_STORE;
	color.clear_color = { 0.09f, 0.11f, 0.15f, 1.0f };

	SDL_GPUDepthStencilTargetInfo depth = {};
	depth.texture = r->depth;
	depth.clear_depth = 1.0f;
	depth.load_op = SDL_GPU_LOADOP_CLEAR;
	depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
	if (!pass)
	{
		SDL_CancelGPUCommandBuffer(cmd);
		return;
	}

	SDL_GPUViewport viewport = {};
	viewport.x = 0.0f;
	viewport.y = (float)r->height;
	viewport.w = (float)r->width;
	viewport.h = -(float)r->height;
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &viewport);

	SDL_BindGPUGraphicsPipeline(pass, r->pipeline);
	SDL_GPUBufferBinding binding = { r->vertex_buffer, 0 };
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

	SDL_GPUTextureSamplerBinding sampler = { r->white_texture, r->white_sampler };
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);

	const float aspect = (float)r->width / (float)r->height;
	const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(50.0f), aspect, 0.1f, 100.0f);
	const glm::mat4 view = glm::lookAt(glm::vec3(3.2f, 2.8f, 4.5f),
		glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 model = glm::rotate(glm::mat4(1.0f), time_seconds * 0.7f, glm::vec3(0.0f, 1.0f, 0.0f))
		* glm::rotate(glm::mat4(1.0f), 0.35f, glm::vec3(1.0f, 0.0f, 0.0f));
	p0_uniform uniform = {};
	uniform.mvp = projection * view * model;
	SDL_PushGPUVertexUniformData(cmd, 0, &uniform, sizeof(uniform));

	SDL_DrawGPUPrimitives(pass, 36, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
	SDL_SubmitGPUCommandBuffer(cmd);
}
