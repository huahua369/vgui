#include "thumbnail_cache.h"

#include "pch.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vgui_editor_kit {

	namespace {

		bool valid_image(const thumbnail_cache::image& value)
		{
			if (value.width <= 0 || value.height <= 0 || value.pixels.empty())
				return false;
			/* Every vgui_texture_format is four bytes per pixel. */
			const int minimum_stride = value.width * 4;
			const int stride = value.stride > 0 ? value.stride : minimum_stride;
			if (stride < minimum_stride)
				return false;
			const std::size_t required = (std::size_t)stride *
				(std::size_t)value.height;
			return value.pixels.size() >= required;
		}

	} /* namespace */

	struct thumbnail_cache::impl
	{
		struct job
		{
			request req;
			load_handler loader;
			disk_read_handler disk_read;
			disk_write_handler disk_write;
			priority prio = priority::high;
			int attempt = 1;
			std::uint64_t token = 0;
			bool use_disk = true;
		};

		struct completed_job
		{
			request req;
			image value;
			std::uint64_t token = 0;
			int attempt = 1;
			bool success = false;
			bool from_disk = false;
		};

		struct entry
		{
			request req;
			state status = state::missing;
			int attempts = 0;
			std::uint64_t token = 0;
			bool wanted = false;
			bool retry_pending = false;
			float retry_left = 0.0f;
			std::string error;
		};

		struct cached_texture
		{
			vgui_texture* texture = nullptr;
			std::list<std::string>::iterator lru;
		};

		impl()
			: worker([this]() { run(); })
		{
		}

		~impl()
		{
			shutdown();
			destroy_textures();
		}

		void run()
		{
			std::unique_lock<std::mutex> lock(mutex);
			for (;;)
			{
				cond.wait(lock, [this]() {
					return quit || !high_jobs.empty() || !low_jobs.empty();
				});
				if (quit)
					break;

				job current;
				if (!high_jobs.empty())
				{
					current = std::move(high_jobs.front());
					high_jobs.pop_front();
				}
				else
				{
					current = std::move(low_jobs.front());
					low_jobs.pop_front();
				}
				active_key = current.req.key;
				lock.unlock();

				image value;
				bool success = false;
				bool from_disk = false;
				if (current.use_disk && current.disk_read)
				{
					try
					{
						success = current.disk_read(current.req, &value);
					}
					catch (...)
					{
						success = false;
					}
					if (success && valid_image(value))
					{
						from_disk = true;
					}
					else
					{
						success = false;
						value = image{};
					}
				}

				if (!success)
				{
					loads.fetch_add(1);
					try
					{
						if (current.loader)
						{
							success = current.loader(current.req, &value);
						}
						else
						{
							value.error = "No thumbnail loader configured";
						}
					}
					catch (...)
					{
						success = false;
						value.error = "Thumbnail loader raised an exception";
					}

					if (success && !valid_image(value))
					{
						success = false;
						if (value.error.empty())
							value.error = "Thumbnail loader returned no pixels";
					}
				}

				if (success && !from_disk && current.use_disk &&
					current.disk_write)
				{
					try
					{
						(void)current.disk_write(current.req, value);
					}
					catch (...)
					{
						/* A cache write must never turn a valid load into a
						 * failure. */
					}
				}

				lock.lock();
				active_key.clear();
				if (quit)
					break;
				completed_job result;
				result.req = std::move(current.req);
				result.value = std::move(value);
				result.token = current.token;
				result.attempt = current.attempt;
				result.success = success;
				result.from_disk = from_disk;
				results.push_back(std::move(result));
			}
		}

		void shutdown()
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				quit = true;
				high_jobs.clear();
				low_jobs.clear();
			}
			cond.notify_all();
			if (worker.joinable())
				worker.join();
		}

		void destroy_textures()
		{
			for (auto& item : textures)
			{
				if (item.second.texture)
					vgui_texture_destroy(item.second.texture);
			}
			textures.clear();
			lru.clear();
		}

		/* Main-thread helpers. */
		void touch(const std::string& key)
		{
			auto found = textures.find(key);
			if (found == textures.end())
				return;
			lru.splice(lru.begin(), lru, found->second.lru);
		}

		void drop_texture(const std::string& key)
		{
			auto found = textures.find(key);
			if (found == textures.end())
				return;
			lru.erase(found->second.lru);
			if (found->second.texture)
				vgui_texture_destroy(found->second.texture);
			textures.erase(found);
		}

		void enforce_limit()
		{
			while (textures.size() > settings.memory_limit && !lru.empty())
			{
				const std::string key = lru.back();
				drop_texture(key);
				auto entry_it = entries.find(key);
				if (entry_it != entries.end() && entry_it->second.status == state::ready)
					entry_it->second.status = state::missing;
			}
		}

		void store_texture(const std::string& key, vgui_texture* texture)
		{
			drop_texture(key);
			lru.push_front(key);
			cached_texture cached;
			cached.texture = texture;
			cached.lru = lru.begin();
			textures[key] = cached;
			enforce_limit();
		}

		void push_job(entry& item, const request& req, int attempt, priority prio)
		{
			item.req = req;
			item.status = state::queued;
			item.wanted = true;
			item.token = ++next_token;
			item.retry_pending = false;
			item.retry_left = 0.0f;

			job queued;
			queued.req = req;
			queued.prio = prio;
			queued.attempt = attempt;
			queued.token = item.token;

			{
				std::lock_guard<std::mutex> lock(mutex);
				queued.loader = loader;
				queued.disk_read = disk_read;
				queued.disk_write = disk_write;
				queued.use_disk = settings.disk_cache;
				if (prio == priority::high)
					high_jobs.push_back(std::move(queued));
				else
					low_jobs.push_back(std::move(queued));
			}
			cond.notify_one();
		}

		void upgrade_queued(const std::string& key)
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (auto it = low_jobs.begin(); it != low_jobs.end(); ++it)
			{
				if (it->req.key != key)
					continue;
				it->prio = priority::high;
				job lifted = std::move(*it);
				low_jobs.erase(it);
				high_jobs.push_back(std::move(lifted));
				return;
			}
		}

		bool remove_queued(const std::string& key)
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (auto it = high_jobs.begin(); it != high_jobs.end(); ++it)
			{
				if (it->req.key == key)
				{
					high_jobs.erase(it);
					return true;
				}
			}
			for (auto it = low_jobs.begin(); it != low_jobs.end(); ++it)
			{
				if (it->req.key == key)
				{
					low_jobs.erase(it);
					return true;
				}
			}
			return false;
		}

		bool active(const std::string& key) const
		{
			std::lock_guard<std::mutex> lock(mutex);
			return !active_key.empty() && active_key == key;
		}

		/* Shared state with the worker. */
		mutable std::mutex mutex;
		std::condition_variable cond;
		std::thread worker;
		bool quit = false;
		std::deque<job> high_jobs;
		std::deque<job> low_jobs;
		std::string active_key;
		std::vector<completed_job> results;
		load_handler loader;
		disk_read_handler disk_read;
		disk_write_handler disk_write;
		std::atomic<std::uint64_t> loads{ 0 };

		/* Main-thread state. */
		vgui_context* ctx = nullptr;
		options settings;
		std::unordered_map<std::string, entry> entries;
		std::unordered_map<std::string, cached_texture> textures;
		std::list<std::string> lru;
		std::uint64_t next_token = 0;
		std::uint64_t memory_hits = 0;
		std::uint64_t disk_hits = 0;
		std::uint64_t uploads = 0;
		std::uint64_t failures = 0;
		std::uint64_t retries = 0;
		std::uint64_t cancellations = 0;
	};

	thumbnail_cache::thumbnail_cache()
		: impl_(std::make_unique<impl>())
	{
	}

	thumbnail_cache::thumbnail_cache(vgui_context* context)
		: impl_(std::make_unique<impl>())
	{
		impl_->ctx = context;
	}

	thumbnail_cache::~thumbnail_cache()
	{
		/* Worker first: the loader may capture host data, and the texture
		 * destruction requires the context to still be alive. */
		impl_.reset();
	}

	void thumbnail_cache::set_context(vgui_context* context)
	{
		if (!impl_ || impl_->ctx == context)
			return;
		/* Textures cannot move between contexts.  Keep statistics but drop
		 * cached pixels; visible requests are re-issued on the next tick. */
		if (impl_->ctx)
			clear();
		impl_->ctx = context;
	}

	vgui_context* thumbnail_cache::context() const noexcept
	{
		return impl_ ? impl_->ctx : nullptr;
	}

	void thumbnail_cache::set_options(options opts)
	{
		if (!impl_)
			return;
		opts.memory_limit = std::max<std::size_t>(1, opts.memory_limit);
		opts.max_attempts = std::max(1, opts.max_attempts);
		opts.retry_delay = std::max(0.0f, opts.retry_delay);
		impl_->settings = opts;
		impl_->enforce_limit();
	}

	const thumbnail_cache::options& thumbnail_cache::settings() const noexcept
	{
		return impl_->settings;
	}

	void thumbnail_cache::set_load_handler(load_handler handler)
	{
		if (!impl_)
			return;
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->loader = std::move(handler);
	}

	void thumbnail_cache::set_disk_cache(disk_read_handler read,
		disk_write_handler write)
	{
		if (!impl_)
			return;
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->disk_read = std::move(read);
		impl_->disk_write = std::move(write);
	}

	void thumbnail_cache::clear_disk_cache()
	{
		if (!impl_)
			return;
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->disk_read = {};
		impl_->disk_write = {};
	}

	bool thumbnail_cache::configured() const noexcept
	{
		if (!impl_)
			return false;
		std::lock_guard<std::mutex> lock(impl_->mutex);
		return (bool)impl_->loader || (bool)impl_->disk_read;
	}

	void thumbnail_cache::enqueue(const request& req, priority prio)
	{
		if (!impl_ || req.key.empty())
			return;

		auto texture_it = impl_->textures.find(req.key);
		if (texture_it != impl_->textures.end())
		{
			impl_->touch(req.key);
			++impl_->memory_hits;
			return;
		}

		auto found = impl_->entries.find(req.key);
		if (found == impl_->entries.end())
		{
			impl::entry item;
			item.req = req;
			item.wanted = true;
			auto inserted = impl_->entries.emplace(req.key, std::move(item));
			impl_->push_job(inserted.first->second, req, 1, prio);
			return;
		}

		impl::entry& item = found->second;
		item.req = req;
		item.wanted = true;
		if (item.status == state::ready)
		{
			/* The texture was evicted but the entry was not updated. */
			item.status = state::missing;
		}
		if (item.status == state::failed)
			return;
		if (item.status == state::queued || item.status == state::loading)
		{
			if (prio == priority::high)
				impl_->upgrade_queued(req.key);
			return;
		}
		if (item.retry_pending)
			return;

		item.attempts = 0;
		item.error.clear();
		impl_->push_job(item, req, 1, prio);
	}

	void thumbnail_cache::cancel(const std::string& key)
	{
		if (!impl_ || key.empty())
			return;
		auto found = impl_->entries.find(key);
		if (found == impl_->entries.end())
			return;

		impl::entry& item = found->second;
		const bool was_ready = item.status == state::ready;
		item.wanted = false;
		item.retry_pending = false;
		item.retry_left = 0.0f;
		++item.token;	/* invalidate any in-flight result */
		if (impl_->remove_queued(key) || impl_->active(key))
			++impl_->cancellations;
		if (!was_ready)
			item.status = state::missing;
	}

	void thumbnail_cache::cancel_all()
	{
		if (!impl_)
			return;
		std::vector<std::string> keys;
		keys.reserve(impl_->entries.size());
		for (const auto& item : impl_->entries)
			keys.push_back(item.first);
		for (const std::string& key : keys)
			cancel(key);
	}

	void thumbnail_cache::retry(const std::string& key)
	{
		if (!impl_ || key.empty())
			return;
		auto found = impl_->entries.find(key);
		if (found == impl_->entries.end())
			return;
		impl::entry& item = found->second;
		item.attempts = 0;
		item.error.clear();
		item.wanted = true;
		if (impl_->textures.find(key) != impl_->textures.end())
		{
			item.status = state::ready;
			impl_->touch(key);
			return;
		}
		impl_->push_job(item, item.req, 1, priority::high);
	}

	void thumbnail_cache::erase(const std::string& key)
	{
		if (!impl_)
			return;
		cancel(key);
		impl_->drop_texture(key);
		impl_->entries.erase(key);
	}

	vgui_texture* thumbnail_cache::lookup(const std::string& key) const
	{
		if (!impl_)
			return nullptr;
		auto found = impl_->textures.find(key);
		return found == impl_->textures.end() ? nullptr : found->second.texture;
	}

	vgui_texture* thumbnail_cache::texture(const std::string& key)
	{
		if (!impl_)
			return nullptr;
		vgui_texture* value = lookup(key);
		if (value)
			impl_->touch(key);
		return value;
	}

	thumbnail_cache::state thumbnail_cache::state_of(const std::string& key) const
	{
		if (!impl_)
			return state::missing;
		auto found = impl_->entries.find(key);
		if (found == impl_->entries.end())
			return state::missing;
		const state status = found->second.status;
		if (status == state::queued && impl_->active(key))
			return state::loading;
		return status;
	}

	const std::string& thumbnail_cache::error(const std::string& key) const
	{
		static const std::string empty;
		if (!impl_)
			return empty;
		auto found = impl_->entries.find(key);
		return found == impl_->entries.end() ? empty : found->second.error;
	}

	bool thumbnail_cache::poll()
	{
		if (!impl_)
			return false;
		std::vector<impl::completed_job> completed;
		{
			std::lock_guard<std::mutex> lock(impl_->mutex);
			completed.swap(impl_->results);
		}

		bool changed = false;
		for (impl::completed_job& job : completed)
		{
			auto found = impl_->entries.find(job.req.key);
			if (found == impl_->entries.end())
				continue;
			impl::entry& item = found->second;
			if (!item.wanted || item.token != job.token)
				continue;

			if (job.success)
			{
				if (!impl_->ctx)
				{
					job.value.error = "No vgui context for thumbnail upload";
				}
				else if (!valid_image(job.value))
				{
					job.value.error = "Invalid thumbnail pixels";
				}
				else
				{
					vgui_texture_desc desc = {};
					desc.width = job.value.width;
					desc.height = job.value.height;
					desc.stride = job.value.stride > 0 ? job.value.stride : 0;
					desc.format = job.value.format;
					desc.pixels = job.value.pixels.data();
					vgui_texture* uploaded = vgui_texture_create(impl_->ctx,
						&desc);
					if (uploaded)
					{
						impl_->store_texture(job.req.key, uploaded);
						item.status = state::ready;
						item.attempts = job.attempt;
						item.error.clear();
						item.retry_pending = false;
						item.retry_left = 0.0f;
						++impl_->uploads;
						if (job.from_disk)
							++impl_->disk_hits;
						changed = true;
						continue;
					}
					job.value.error = "Unable to create thumbnail texture";
				}
			}

			item.attempts = std::max(item.attempts, job.attempt);
			item.error = job.value.error.empty()
				? "Thumbnail load failed" : job.value.error;
			item.status = state::queued;
			if (item.attempts < impl_->settings.max_attempts)
			{
				item.retry_pending = true;
				item.retry_left = impl_->settings.retry_delay;
				++impl_->retries;
			}
			else
			{
				item.status = state::failed;
				++impl_->failures;
			}
			changed = true;
		}
		return changed;
	}

	bool thumbnail_cache::tick(float delta_time)
	{
		if (!impl_)
			return false;
		bool changed = poll();

		const float delta = std::max(0.0f, delta_time);
		std::vector<std::string> due;
		for (auto& pair : impl_->entries)
		{
			impl::entry& item = pair.second;
			if (!item.retry_pending || !item.wanted)
				continue;
			item.retry_left -= delta;
			if (item.retry_left <= 0.0f)
				due.push_back(pair.first);
		}
		for (const std::string& key : due)
		{
			auto found = impl_->entries.find(key);
			if (found == impl_->entries.end())
				continue;
			impl::entry& item = found->second;
			if (!item.retry_pending || !item.wanted)
				continue;
			impl_->push_job(item, item.req, item.attempts + 1,
				priority::high);
			changed = true;
		}
		return changed;
	}

	void thumbnail_cache::clear()
	{
		if (!impl_)
			return;
		cancel_all();
		impl_->destroy_textures();
		impl_->entries.clear();
		{
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->results.clear();
		}
	}

	thumbnail_cache::statistics thumbnail_cache::stats() const
	{
		statistics out;
		if (!impl_)
			return out;
		out.memory_entries = impl_->textures.size();
		out.memory_limit = impl_->settings.memory_limit;
		for (const auto& item : impl_->entries)
		{
			if (item.second.status == state::queued ||
				item.second.status == state::loading)
				++out.pending;
		}
		out.memory_hits = impl_->memory_hits;
		out.disk_hits = impl_->disk_hits;
		out.loads = impl_->loads.load();
		out.uploads = impl_->uploads;
		out.failures = impl_->failures;
		out.retries = impl_->retries;
		out.cancellations = impl_->cancellations;
		return out;
	}

	bool thumbnail_cache::idle() const
	{
		if (!impl_)
			return true;
		for (const auto& item : impl_->entries)
		{
			if (item.second.status == state::queued ||
				item.second.status == state::loading ||
				item.second.retry_pending)
				return false;
		}
		return true;
	}

} /* namespace vgui_editor_kit */
