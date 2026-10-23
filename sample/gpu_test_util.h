#pragma once

// Small SDL_GPU helpers shared by the offscreen readback tests.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <cstring>
#include <vector>

static inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
	return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
}

static inline SDL_GPUTexture* create_texture(SDL_GPUDevice* device, int width, int height,
	const uint32_t* pixels)
{
	SDL_GPUTextureCreateInfo info = {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.width = (Uint32)width;
	info.height = (Uint32)height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
	if (!texture)
		return nullptr;

	const Uint32 bytes = (Uint32)(width * height * sizeof(uint32_t));
	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = bytes;
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
	if (!transfer)
	{
		SDL_ReleaseGPUTexture(device, texture);
		return nullptr;
	}
	void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
	if (!mapped)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		SDL_ReleaseGPUTexture(device, texture);
		return nullptr;
	}
	std::memcpy(mapped, pixels, bytes);
	SDL_UnmapGPUTransferBuffer(device, transfer);

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
	if (!cmd)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		SDL_ReleaseGPUTexture(device, texture);
		return nullptr;
	}
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo src = {};
	src.transfer_buffer = transfer;
	src.offset = 0;
	src.pixels_per_row = 0;
	src.rows_per_layer = 0;
	SDL_GPUTextureRegion dst = {};
	dst.texture = texture;
	dst.w = (Uint32)width;
	dst.h = (Uint32)height;
	dst.d = 1;
	SDL_UploadToGPUTexture(copy, &src, &dst, false);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_WaitForGPUIdle(device);
	SDL_ReleaseGPUTransferBuffer(device, transfer);
	return texture;
}

static inline bool download_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture,
	int width, int height, std::vector<uint32_t>& out)
{
	const Uint32 bytes = (Uint32)(width * height * sizeof(uint32_t));
	out.assign((size_t)width * height, 0);
	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transfer_info.size = bytes;
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
	if (!transfer)
		return false;

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
	if (!cmd)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion src = {};
	src.texture = texture;
	src.w = (Uint32)width;
	src.h = (Uint32)height;
	src.d = 1;
	SDL_GPUTextureTransferInfo dst = {};
	dst.transfer_buffer = transfer;
	dst.offset = 0;
	dst.pixels_per_row = 0;
	dst.rows_per_layer = 0;
	SDL_DownloadFromGPUTexture(copy, &src, &dst);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_WaitForGPUIdle(device);

	void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
	if (!mapped)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return false;
	}
	std::memcpy(out.data(), mapped, bytes);
	SDL_UnmapGPUTransferBuffer(device, transfer);
	SDL_ReleaseGPUTransferBuffer(device, transfer);
	return true;
}
