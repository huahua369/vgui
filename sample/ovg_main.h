// ovg.h: 标准系统包含文件的包含文件
// 或项目特定的包含文件。

#pragma once

#include <iostream>

// TODO: 在此处引用程序需要的其他标头。

#include <chrono>
#include "pch.h"


class runtime_cx
{
private:
	std::chrono::time_point<std::chrono::high_resolution_clock> _begin;
public:
	runtime_cx()
	{
		begin();
	}
	~runtime_cx()
	{}
	void begin() {
		_begin = (std::chrono::high_resolution_clock::now());
	}
	int64_t end() {

		double aa = elapsed_micro();
		aa *= 0.001;
		return aa;
	}
	int64_t get_ms() {
		auto t = std::chrono::high_resolution_clock::now().time_since_epoch();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
		return ms;
	}
	int64_t elapsed() const
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - _begin).count();
	}
	//微秒
	int64_t elapsed_micro() const
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - _begin).count();
	}
private:

};
class SmoothFpsCounter {
public:
	SmoothFpsCounter(std::size_t maxSamples = 60)
		: maxSamples_(maxSamples) {}

	void tick() {
		auto now = std::chrono::steady_clock::now();
		samples_.push_back(now);

		if (samples_.size() > maxSamples_) {
			samples_.pop_front();
		}
	}

	double getFps() const {
		if (samples_.size() < 2)
			return 0.0;

		auto duration = samples_.back() - samples_.front();
		double seconds =
			std::chrono::duration_cast<std::chrono::microseconds>(duration).count()
			/ 1'000'000.0;

		if (seconds <= 0.0)
			return 0.0;

		return (samples_.size() - 1) / seconds;
	}
	const char* c_str() {
		int n = getFps(); str = std::to_string(n);
		return str.c_str();
	}
private:
	std::deque<std::chrono::steady_clock::time_point> samples_;
	std::size_t maxSamples_;
	std::string str;
};

class FrameProfiler {
public:
	FrameProfiler(size_t avgWindow = 60)
		: avgWindow_(avgWindow) {}

	void beginFrame() {
		frameStart_ = std::chrono::steady_clock::now();
	}

	void endFrame() {
		auto now = std::chrono::steady_clock::now();

		// 单帧 CPU 耗时（微秒）
		auto frameTimeUs =
			std::chrono::duration_cast<std::chrono::microseconds>(
				now - frameStart_
			).count();

		lastFrameTimeMs_ = frameTimeUs / 1000.0;

		// 滑动窗口
		frameTimes_.push_back(frameTimeUs);
		if (frameTimes_.size() > avgWindow_) {
			frameTimes_.pop_front();
		}

		// 实时 FPS（基于最近 N 帧）
		auto windowDuration =
			std::chrono::duration<double>(now - windowStart_);

		if (windowDuration.count() >= 0.5) {
			fps_ = static_cast<double>(frameTimes_.size()) /
				windowDuration.count();
			windowStart_ = now;
		}
	}

	double getFps() const {
		return fps_;
	}

	double getLastFrameTimeMs() const {
		return lastFrameTimeMs_;
	}

	double getAverageFrameTimeMs() const {
		if (frameTimes_.empty()) return 0.0;

		uint64_t sum = std::accumulate(
			frameTimes_.begin(), frameTimes_.end(), 0ULL
		);
		return (sum / frameTimes_.size()) / 1000.0;
	}

	const char* c_str() {
		int n = getFps();
		int ms = getLastFrameTimeMs();
		str = "fps: " + std::to_string(n) + " ";
		str += "ms: " + std::to_string(ms) + " ";
		return str.c_str();
	}
private:
	std::chrono::steady_clock::time_point frameStart_;
	std::chrono::steady_clock::time_point windowStart_ =
		std::chrono::steady_clock::now();

	std::deque<uint64_t> frameTimes_;  // us
	size_t avgWindow_;

	double fps_ = 0.0;
	double lastFrameTimeMs_ = 0.0;
	std::string str;
};
