#pragma once

#include <cstdint>

#include <vector>

namespace Text {

constexpr const uint32_t FLAG_BOOL_RUNS = 1u << 31;
constexpr const uint32_t MASK_BOOL_RUNS = ~(1u << 31);

template <typename T>
class ValueRuns {
	public:
		constexpr ValueRuns() = default;
		template <typename U>
		constexpr ValueRuns(U&& value, int32_t limit)
				: m_values{std::forward<U>(value)}
				, m_limits{limit} {}
		constexpr ValueRuns(size_t initialCapacity) {
			m_values.reserve(initialCapacity);
			m_limits.reserve(initialCapacity);
		}

		constexpr ValueRuns(ValueRuns&&) noexcept = default;
		constexpr ValueRuns& operator=(ValueRuns&&) noexcept = default;

		ValueRuns(const ValueRuns&) = delete;
		void operator=(const ValueRuns&) = delete;

		template <typename... Args>
		constexpr void add(int32_t limit, Args&&... args) {
			m_values.emplace_back(std::forward<Args>(args)...);
			m_limits.emplace_back(limit);
		}

		constexpr void get_runs_subset(int32_t offset, int32_t length, ValueRuns& output) const {
			size_t i = 0;

			while (i < m_limits.size() && m_limits[i] < offset) {
				++i;
			}

			for (; i < m_limits.size(); ++i) {
				auto newLimit = m_limits[i] - offset;

				if (newLimit < length) {
					output.add(newLimit, m_values[i]);
				}
				else {
					output.add(length, m_values[i]);
					break;
				}
			}
		}

		constexpr T get_value(int32_t index) const {
			return m_values[get_run_index(index)];
		}

		constexpr T get_run_value(size_t runIndex) const {
			return m_values[runIndex];
		}

		constexpr int32_t get_run_limit(size_t runIndex) const {
			return m_limits[runIndex];
		}

		constexpr size_t get_run_index(int32_t index) const {
			size_t first{};
			auto count = m_limits.size();

			while (count > 0) {
				auto step = count / 2;
				auto i = first + step;

				if (m_limits[i] <= index) {
					first = i + 1;
					count -= step + 1;
				}
				else {
					count = step;
				}
			}

			return first;
		}

		constexpr void clear() {
			m_values.clear();
			m_limits.clear();
		}

		constexpr bool empty() const {
			return m_values.empty();
		}

		constexpr size_t get_run_count() const {
			return m_limits.size();
		}

		constexpr int32_t get_limit() const {
			return m_limits.back();
		}

		constexpr const T* get_values() const {
			return m_values.data();
		}

		constexpr const int32_t* get_limits() const {
			return m_limits.data();
		}
	private:
		std::vector<T> m_values;
		std::vector<int32_t> m_limits;
};

}

