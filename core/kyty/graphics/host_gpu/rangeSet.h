#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// 2-level chunked sorted interval set (B-tree leaves of up to 64 disjoint, non-adjacent ranges).
// Keeps block max-ends in a compact parallel array for L1-resident binary search while capping
// middle-insert / erase shifts to <= 63 elements instead of O(N).
class RangeSet final {
public:
	static constexpr uint32_t BlockCap = 64;
	static constexpr uint32_t HalfCap  = BlockCap / 2;

	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		if (m_blocks.empty()) {
			Block blk;
			blk.count    = 1;
			blk.items[0] = {address, end};
			m_blocks.push_back(blk);
			m_block_max_end.push_back(end);
			return;
		}
		if (address > m_block_max_end.back()) {
			auto& last = m_blocks.back();
			if (last.count < BlockCap) {
				last.items[last.count++] = {address, end};
				m_block_max_end.back()   = end;
			} else {
				Block blk;
				blk.count    = 1;
				blk.items[0] = {address, end};
				m_blocks.push_back(blk);
				m_block_max_end.push_back(end);
			}
			return;
		}
		if (auto& last = m_blocks.back(); address >= last.items[last.count - 1].first) {
			if (end > last.items[last.count - 1].second) {
				last.items[last.count - 1].second = end;
				m_block_max_end.back()            = end;
			}
			return;
		}

		// First block whose max_end >= address (guaranteed < m_blocks.size()).
		const size_t b = static_cast<size_t>(
		    std::lower_bound(m_block_max_end.begin(), m_block_max_end.end(), address) - m_block_max_end.begin());
		auto& blk = m_blocks[b];
		auto* const begin_it = blk.items.data();
		auto* const end_it   = begin_it + blk.count;
		auto* it = std::lower_bound(begin_it, end_it, address,
		                            [](const auto& r, uint64_t key) { return r.first < key; });
		if (it != begin_it && (it - 1)->second >= address) {
			--it;
		}
		uint32_t pos = static_cast<uint32_t>(it - begin_it);

		if (pos == blk.count || blk.items[pos].first > address) {
			if (pos < blk.count && blk.items[pos].first <= end) {
				blk.items[pos].first = address;
				if (end > blk.items[pos].second) {
					blk.items[pos].second = end;
				}
			} else {
				// Pure disjoint insert at `pos` inside block `b` (no overlap with neighbors).
				InsertInBlock(b, pos, {address, end});
				return;
			}
		} else {
			if (blk.items[pos].second >= end) {
				return;
			}
			blk.items[pos].second = end;
		}

		// Merge any subsequent overlapping or adjacent intervals inside block `b`.
		uint32_t next      = pos + 1;
		uint32_t merge_end = next;
		while (merge_end < blk.count && blk.items[merge_end].first <= blk.items[pos].second) {
			if (blk.items[merge_end].second > blk.items[pos].second) {
				blk.items[pos].second = blk.items[merge_end].second;
			}
			++merge_end;
		}
		if (merge_end != next) {
			const uint32_t tail = blk.count - merge_end;
			if (tail > 0) {
				std::move(blk.items.begin() + merge_end, blk.items.begin() + blk.count, blk.items.begin() + next);
			}
			blk.count -= (merge_end - next);
		}

		// If the merged interval is now at the end of block `b`, absorb overlapping/adjacent subsequent blocks.
		if (pos + 1 == blk.count) {
			uint64_t     cur_end    = blk.items[pos].second;
			const size_t erase_from = b + 1;
			size_t       nb         = b + 1;
			while (nb < m_blocks.size() && m_blocks[nb].First() <= cur_end) {
				if (m_block_max_end[nb] <= cur_end) {
					++nb;
				} else {
					auto&    next_blk = m_blocks[nb];
					uint32_t k        = 0;
					while (k < next_blk.count && next_blk.items[k].first <= cur_end) {
						if (next_blk.items[k].second > cur_end) {
							cur_end = next_blk.items[k].second;
						}
						++k;
					}
					if (k == next_blk.count) {
						++nb;
					} else {
						const uint32_t rem = next_blk.count - k;
						std::move(next_blk.items.begin() + k, next_blk.items.begin() + next_blk.count,
						          next_blk.items.begin());
						next_blk.count      = rem;
						m_block_max_end[nb] = next_blk.Last();
						break;
					}
				}
			}
			blk.items[pos].second = cur_end;
			m_block_max_end[b]    = cur_end;
			if (nb > erase_from) {
				m_blocks.erase(m_blocks.begin() + static_cast<ptrdiff_t>(erase_from),
				               m_blocks.begin() + static_cast<ptrdiff_t>(nb));
				m_block_max_end.erase(m_block_max_end.begin() + static_cast<ptrdiff_t>(erase_from),
				                      m_block_max_end.begin() + static_cast<ptrdiff_t>(nb));
			}
		} else {
			m_block_max_end[b] = blk.Last();
		}
	}

	void Subtract(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		if (m_blocks.empty() || address >= m_block_max_end.back() || end <= m_blocks.front().First()) {
			return;
		}
		size_t b = static_cast<size_t>(
		    std::upper_bound(m_block_max_end.begin(), m_block_max_end.end(), address) - m_block_max_end.begin());
		while (b < m_blocks.size() && m_blocks[b].First() < end) {
			if (m_blocks[b].First() >= address && m_block_max_end[b] <= end) {
				size_t eb = b + 1;
				while (eb < m_blocks.size() && m_block_max_end[eb] <= end) {
					++eb;
				}
				m_blocks.erase(m_blocks.begin() + static_cast<ptrdiff_t>(b),
				               m_blocks.begin() + static_cast<ptrdiff_t>(eb));
				m_block_max_end.erase(m_block_max_end.begin() + static_cast<ptrdiff_t>(b),
				                      m_block_max_end.begin() + static_cast<ptrdiff_t>(eb));
				continue;
			}
			auto& blk = m_blocks[b];
			auto* const begin_it = blk.items.data();
			auto* const end_it   = begin_it + blk.count;
			auto* it = std::lower_bound(begin_it, end_it, address,
			                            [](const auto& r, uint64_t key) { return r.first < key; });
			if (it != begin_it && (it - 1)->second > address) {
				--it;
			}
			uint32_t pos = static_cast<uint32_t>(it - begin_it);
			if (pos == blk.count || blk.items[pos].first >= end) {
				break;
			}
			if (blk.items[pos].first < address && blk.items[pos].second > end) {
				const auto tail       = blk.items[pos].second;
				blk.items[pos].second = address;
				InsertInBlock(b, pos + 1, {end, tail});
				return;
			}
			if (blk.items[pos].first < address) {
				blk.items[pos].second = address;
				++pos;
			}
			uint32_t del = pos;
			while (del < blk.count && blk.items[del].second <= end) {
				++del;
			}
			if (del < blk.count && blk.items[del].first < end) {
				blk.items[del].first = end;
			}
			if (del != pos) {
				const uint32_t tail = blk.count - del;
				if (tail > 0) {
					std::move(blk.items.begin() + del, blk.items.begin() + blk.count, blk.items.begin() + pos);
				}
				blk.count -= (del - pos);
			}
			if (blk.count == 0) {
				m_blocks.erase(m_blocks.begin() + static_cast<ptrdiff_t>(b));
				m_block_max_end.erase(m_block_max_end.begin() + static_cast<ptrdiff_t>(b));
			} else {
				m_block_max_end[b] = blk.Last();
				++b;
			}
		}
	}

	void Clear() {
		m_blocks.clear();
		m_block_max_end.clear();
	}

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& blk: m_blocks) {
			for (uint32_t i = 0; i < blk.count; ++i) {
				func(blk.items[i].first, blk.items[i].second);
			}
		}
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		if (m_blocks.empty() || address >= m_block_max_end.back() || end <= m_blocks.front().First()) {
			return false;
		}
		const size_t b = static_cast<size_t>(
		    std::upper_bound(m_block_max_end.begin(), m_block_max_end.end(), address) - m_block_max_end.begin());
		if (b >= m_blocks.size()) {
			return false;
		}
		const auto& blk = m_blocks[b];
		if (blk.First() >= end) {
			return false;
		}
		const auto* const begin_it = blk.items.data();
		const auto* const end_it   = begin_it + blk.count;
		const auto* const it       = std::lower_bound(begin_it, end_it, address,
		                                              [](const auto& r, uint64_t key) { return r.first < key; });
		if (it != begin_it && (it - 1)->second > address) {
			return true;
		}
		return it != end_it && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		if (m_blocks.empty() || address < m_blocks.front().First() || end > m_block_max_end.back()) {
			return false;
		}
		const size_t b = static_cast<size_t>(
		    std::upper_bound(m_block_max_end.begin(), m_block_max_end.end(), address) - m_block_max_end.begin());
		if (b >= m_blocks.size()) {
			return false;
		}
		const auto& blk = m_blocks[b];
		if (blk.First() > address) {
			return false;
		}
		const auto* const begin_it = blk.items.data();
		const auto* const end_it   = begin_it + blk.count;
		const auto* it             = std::upper_bound(begin_it, end_it, address,
		                                              [](uint64_t key, const auto& r) { return key < r.first; });
		if (it == begin_it) {
			return false;
		}
		--it;
		return it->first <= address && it->second >= end;
	}

	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		if (m_blocks.empty() || address >= m_block_max_end.back() || end <= m_blocks.front().First()) {
			return;
		}
		size_t b = static_cast<size_t>(
		    std::upper_bound(m_block_max_end.begin(), m_block_max_end.end(), address) - m_block_max_end.begin());
		for (bool first_block = true; b < m_blocks.size() && m_blocks[b].First() < end; ++b, first_block = false) {
			const auto&       blk      = m_blocks[b];
			const auto* const begin_it = blk.items.data();
			const auto* const end_it   = begin_it + blk.count;
			const auto*       it       = begin_it;
			if (first_block) {
				it = std::upper_bound(begin_it, end_it, address,
				                      [](uint64_t key, const auto& r) { return key < r.first; });
				if (it != begin_it) {
					--it;
				}
			}
			for (; it != end_it && it->first < end; ++it) {
				const auto begin = std::max(address, it->first);
				const auto last  = std::min(end, it->second);
				if (begin < last) {
					func(begin, last);
				}
			}
		}
	}

	[[nodiscard]] bool Empty() const { return m_blocks.empty(); }

private:
	struct Block {
		uint32_t                                             count = 0;
		std::array<std::pair<uint64_t, uint64_t>, BlockCap> items {};

		[[nodiscard]] uint64_t First() const noexcept { return items[0].first; }
		[[nodiscard]] uint64_t Last() const noexcept { return items[count - 1].second; }
	};

	void InsertInBlock(size_t b, uint32_t pos, std::pair<uint64_t, uint64_t> item) {
		if (m_blocks[b].count < BlockCap) {
			auto& blk = m_blocks[b];
			if (pos < blk.count) {
				std::move_backward(blk.items.begin() + pos, blk.items.begin() + blk.count,
				                   blk.items.begin() + blk.count + 1);
			}
			blk.items[pos] = item;
			++blk.count;
			m_block_max_end[b] = blk.Last();
			return;
		}
		// Split full block into two blocks of HalfCap before inserting.
		Block right;
		right.count = BlockCap - HalfCap;
		std::copy(m_blocks[b].items.begin() + HalfCap, m_blocks[b].items.begin() + BlockCap, right.items.begin());
		m_blocks[b].count = HalfCap;
		if (pos <= HalfCap) {
			auto& left = m_blocks[b];
			if (pos < left.count) {
				std::move_backward(left.items.begin() + pos, left.items.begin() + left.count,
				                   left.items.begin() + left.count + 1);
			}
			left.items[pos] = item;
			++left.count;
		} else {
			const uint32_t rpos = pos - HalfCap;
			if (rpos < right.count) {
				std::move_backward(right.items.begin() + rpos, right.items.begin() + right.count,
				                   right.items.begin() + right.count + 1);
			}
			right.items[rpos] = item;
			++right.count;
		}
		m_block_max_end[b] = m_blocks[b].Last();
		const uint64_t right_max = right.Last();
		m_blocks.insert(m_blocks.begin() + static_cast<ptrdiff_t>(b + 1), right);
		m_block_max_end.insert(m_block_max_end.begin() + static_cast<ptrdiff_t>(b + 1), right_max);
	}

	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::vector<uint64_t> m_block_max_end;
	std::vector<Block>    m_blocks;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
