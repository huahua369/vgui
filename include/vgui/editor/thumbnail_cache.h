#pragma once
/*
 * thumbnail_cache.h - asynchronous thumbnail loader with an LRU texture cache.
 *
 * A05: hosts supply a CPU loader through set_load_handler().  The loader runs
 * on a background worker and must never touch vgui.  Completed pixels are
 * uploaded on the main thread by poll()/tick(); the cache owns the resulting
 * textures and destroys them before its vgui_context disappears.
 *
 * Requests carry a stable string key.  Textures stay in an LRU memory cache
 * (options::memory_limit) and optional disk cache callbacks run on the worker
 * thread.  A memory hit never calls the loader or the disk cache.  Queued jobs
 * can be cancelled when an item leaves the visible range; an in-flight result
 * is discarded through a generation token even though the worker cannot be
 * interrupted safely.  Failures are retried options::max_attempts times with
 * options::retry_delay between attempts and then remain in state::failed until
 * retry() is called explicitly.
 *
 * The component is deliberately UI-agnostic; asset_browser builds on it for
 * its visible-range thumbnails.  The host owns the vgui_context and must
 * destroy this cache (or call clear()) before destroying the context.
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vgui/vgui.h>

namespace vgui_editor_kit {

	class thumbnail_cache
	{
	public:
		struct image
		{
			int width = 0;
			int height = 0;
			int stride = 0;
			vgui_texture_format format = VGUI_TEXTURE_FORMAT_RGBA8;
			std::vector<unsigned char> pixels;
			std::string error;
		};

		struct request
		{
			std::string key;
			std::string name;
			std::string type;
		};

		enum class state
		{
			missing,
			queued,
			loading,
			ready,
			failed
		};

		enum class priority
		{
			low,
			high
		};

		struct options
		{
			/* Maximum number of uploaded textures kept in the LRU.  Keep this
			 * larger than the visible range to avoid evicting on-screen items. */
			std::size_t memory_limit = 256;
			/* Total attempts including the first load.  1 disables retries. */
			int max_attempts = 2;
			float retry_delay = 0.5f;
			/* Set to false to keep the callbacks but bypass the disk cache. */
			bool disk_cache = true;
		};

		struct statistics
		{
			std::size_t memory_entries = 0;
			std::size_t memory_limit = 0;
			std::size_t pending = 0;
			std::uint64_t memory_hits = 0;
			std::uint64_t disk_hits = 0;
			std::uint64_t loads = 0;
			std::uint64_t uploads = 0;
			std::uint64_t failures = 0;
			std::uint64_t retries = 0;
			std::uint64_t cancellations = 0;
		};

		/* Runs on the worker thread.  Fill image::pixels with CPU data; return
		 * false (optionally setting image::error) to schedule a retry. */
		using load_handler = std::function<bool(const request&, image*)>;
		/* Optional disk cache.  Both callbacks run on the worker thread and
		 * receive the same key.  A read miss falls through to the loader; a
		 * loader success is offered to the write callback. */
		using disk_read_handler = std::function<bool(const request&, image*)>;
		using disk_write_handler = std::function<bool(const request&, const image&)>;

		thumbnail_cache();
		explicit thumbnail_cache(vgui_context* context);
		~thumbnail_cache();

		thumbnail_cache(const thumbnail_cache&) = delete;
		thumbnail_cache& operator=(const thumbnail_cache&) = delete;

		/* Needed before the first completed job can be uploaded.  Changing the
		 * context drops every cached texture. */
		void set_context(vgui_context* context);
		vgui_context* context() const noexcept;

		void set_options(options opts);
		const options& settings() const noexcept;

		void set_load_handler(load_handler handler);
		void set_disk_cache(disk_read_handler read, disk_write_handler write);
		void clear_disk_cache();
		/* True when at least one of the loader or disk-read callbacks is set. */
		bool configured() const noexcept;

		/* A texture hit only touches the LRU; it never calls the loader, the
		 * disk cache or the upload path.  A queued/loading request keeps its
		 * original attempt count. */
		void enqueue(const request& req, priority prio = priority::high);
		/* Cancels a queued job and invalidates any in-flight result.  A cached
		 * texture is kept. */
		void cancel(const std::string& key);
		void cancel_all();
		/* Restarts a failed key from attempt one. */
		void retry(const std::string& key);
		/* Cancels the key and destroys its cached texture. */
		void erase(const std::string& key);

		/* Main-thread accessors.  lookup() does not update the LRU; texture()
		 * does. */
		vgui_texture* lookup(const std::string& key) const;
		vgui_texture* texture(const std::string& key);
		state state_of(const std::string& key) const;
		const std::string& error(const std::string& key) const;

		/* Drains completed jobs and uploads textures.  Returns true when at
		 * least one key changed state.  tick() also advances retry timers and
		 * calls poll(). */
		bool poll();
		bool tick(float delta_time);

		void clear();
		statistics stats() const;
		bool idle() const;

	private:
		struct impl;
		std::unique_ptr<impl> impl_;
	};

} /* namespace vgui_editor_kit */
