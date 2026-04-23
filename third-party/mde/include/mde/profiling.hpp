/**
 * @file profiling.hpp
 * @brief Enables basic code-based profiling metrics in MDE.
 */

#ifndef MDE_PROFILING_H
#define MDE_PROFILING_H

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace mde {

/**
 * @brief      Utility class for enabling code-based profiling.
 */
struct PerformanceStatistics {
	using Count = uint64_t;
	using String = std::string;
	using TimePoint = std::chrono::steady_clock::time_point;
	template <typename K, typename V> using Map = std::map<K, V>;

	using Mutex = std::mutex;
	using WriteLock = std::lock_guard<std::mutex>;
	using ThreadID = std::thread::id;

	std::mutex mutex;

	struct Duration {
		bool started = false;
		TimePoint t1;
		TimePoint t2;
		long double duration = 0;

		long double get_curr_duration_ms() {
			using namespace std::chrono;
			return (duration_cast<microseconds>(t2.time_since_epoch()).count() -
					duration_cast<microseconds>(t1.time_since_epoch()).count()) /
				    1000.0;
		}

		long double get_cumul_duration_ms() { return duration; }
	};

	Map<String, Count> counters;
	Map<ThreadID, Map<String, Duration>> timers;

	// Timer Functions

	static inline const ThreadID currthread() {
		return std::this_thread::get_id();
	}

	Duration &get_timer(const String &s) {
		if (timers.count(std::this_thread::get_id()) == 0) {
			timers[std::this_thread::get_id()] = {};
		}

		if (timers[std::this_thread::get_id()].count(s) == 0) {
			timers[std::this_thread::get_id()][s] = Duration{};
		}

		return timers[std::this_thread::get_id()][s];
	}

	void timer_start(const String &s) {
		WriteLock m(mutex);
		auto &d = get_timer(s);
		assert(!d.started && "timer already started");
		d.started = true;
		d.t1 = std::chrono::steady_clock::now();
	}

	void timer_end(const String &s) {
		WriteLock m(mutex);
		auto &d = get_timer(s);
		assert(d.started && "timer already stopped");
		d.started = false;
		d.t2 = std::chrono::steady_clock::now();
		d.duration += d.get_curr_duration_ms();
	}

	// Counter Functions

	Count &get_counter(const String &s) {
		if (counters.count(s) == 0) {
			counters[s] = 0;
		}

		return counters[s];
	}

	void inc_counter(const String &s) {
		WriteLock m(mutex);
		get_counter(s)++;
	}

	// dump

	String dump() const {
		using namespace std;
		stringstream s;

		if (counters.size() < 1 && timers.size() < 1) {
			s << '\n' << "Profiler: No statistics generated" << '\n';
			return s.str();
		}

		s << '\n' << "Profiler Statistics:" << '\n';
		for (const auto& k : counters) {
			s << "    "
				 << "'" << k.first << "'"
				 << ": " << k.second << '\n';
		}
		for (const auto& t : timers) {
			if (timers.size() > 1) {
				s << "Thread " << t.first << ":" << '\n';
			}
			for (auto k : t.second) {
				s << "    "
				 << "'" << k.first << "'"
				 << ": " << k.second.get_cumul_duration_ms() << " ms" << '\n';
			}
		}

		return s.str();
	}
};

/**
 * @brief      The object used to enable the duration capturing mechanism.
 */
struct __CalcTime {
	std::string key;
	PerformanceStatistics &stat;

	__CalcTime(PerformanceStatistics &stat, const std::string &key): key(key), stat(stat) {
		stat.timer_start(key);
	}

	~__CalcTime() {
		stat.timer_end(key);
	}
};

/**
 * @def        __mde_calc_time(__stat, __key)
 * @brief      Creates a timer object to capture duration when going out of
 *             scope.
 *
 * @param      __stat  PerformanceStatistics object
 * @param      __key   Identifier for this duration
 */

/**
 * @def        __mde_calc_functime(__stat)
 * @brief      Creates a timer object to capture duration of the current
 *             function when going out of scope.
 *
 * @param      __stat  PerformanceStatistics object
 */

#ifdef MDE_ENABLE_PERFORMANCE_METRICS
#define __mde_calc_time(__stat, __key) \
	auto __MDE_TIMER_OBJECT__ = __CalcTime((__stat), (__key))
#define __mde_calc_functime(__stat) \
	auto __MDE_TIMER_OBJECT__ = __CalcTime((__stat), __func__)
#else
#define __mde_calc_time(__stat, __key)
#define __mde_calc_functime(__stat)
#endif

} // namespace mde

#endif