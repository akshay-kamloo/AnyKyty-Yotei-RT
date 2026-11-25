namespace AsyncHostCopy {
namespace {

struct CopyItem {
	uint8_t* dst       = nullptr;
	uint64_t src_vaddr = 0;
	uint64_t size      = 0;
};

struct Worker {
	static constexpr size_t RingSize = 1024;
	static constexpr size_t RingMask = RingSize - 1;

	struct Job {
		MemoryTracker*              tracker = nullptr;
		std::vector<CopyItem>       copies;
		std::vector<RegionManager*> regions;
		uint64_t                    epoch = 0;
	};

	std::array<Job, RingSize> slots {};
	alignas(64) std::atomic<uint64_t> head {0};
	alignas(64) std::atomic<uint64_t> tail {0};
	alignas(64) std::atomic<uint64_t> published_epoch {0};
	alignas(64) std::atomic<uint64_t> completed_epoch {0};
	alignas(64) std::atomic<bool>     worker_sleeping {false};
	alignas(64) std::atomic<uint32_t> waiters {0};

	std::atomic<uint64_t> stat_jobs {0};
	std::atomic<uint64_t> stat_copies {0};
	std::atomic<uint64_t> stat_bytes {0};
	std::atomic<uint64_t> stat_regions {0};
	std::atomic<uint64_t> stat_worker_ns {0};
	std::atomic<uint64_t> stat_wait_calls {0};
	std::atomic<uint64_t> stat_wait_ns {0};

	std::thread thread;

	Worker() : thread([this] { Run(); }) { thread.detach(); }

	void Run() {
		Common::Thread::SetCurrentName("AsyncHostCopy");
		for (;;) {
			const uint64_t t = tail.load(std::memory_order_relaxed);
			uint64_t       h = head.load(std::memory_order_acquire);
			if (h == t) {
				const auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
				while (h == t && std::chrono::steady_clock::now() < spin_until) {
					_mm_pause();
					h = head.load(std::memory_order_acquire);
				}
				while (h == t) {
					worker_sleeping.store(true, std::memory_order_seq_cst);
					h = head.load(std::memory_order_seq_cst);
					if (h != t) {
						worker_sleeping.store(false, std::memory_order_relaxed);
						break;
					}
					head.wait(t, std::memory_order_acquire);
					worker_sleeping.store(false, std::memory_order_relaxed);
					h = head.load(std::memory_order_acquire);
				}
			}

			auto&      job   = slots[t & RingMask];
			const bool timed = PerfEnabled();
			const auto t0    = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};

			uint32_t reg_updated = 0;
			if (job.tracker != nullptr && !job.regions.empty()) {
				reg_updated = job.tracker->CommitDeferredProtections(job.regions);
			}
			uint64_t bytes = 0;
			for (const auto& c: job.copies) {
				LibKernel::Memory::ReadGuestPlain(c.dst, c.src_vaddr, c.size);
				bytes += c.size;
			}
			const uint64_t job_epoch = job.epoch;
			const uint64_t n_copies  = job.copies.size();
			job.copies.clear();
			job.regions.clear();
			job.tracker = nullptr;

			tail.store(t + 1, std::memory_order_release);
			tail.notify_one();
			completed_epoch.store(job_epoch, std::memory_order_seq_cst);
			if (waiters.load(std::memory_order_seq_cst) != 0) {
				completed_epoch.notify_all();
			}

			if (timed) {
				const auto ns = static_cast<uint64_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
				const uint64_t jobs = stat_jobs.fetch_add(1, std::memory_order_relaxed) + 1;
				stat_copies.fetch_add(n_copies, std::memory_order_relaxed);
				stat_bytes.fetch_add(bytes, std::memory_order_relaxed);
				stat_regions.fetch_add(reg_updated, std::memory_order_relaxed);
				stat_worker_ns.fetch_add(ns, std::memory_order_relaxed);
				if ((jobs & 4095u) == 0) {
					std::fprintf(stderr,
					             "PERF async_copy jobs=%llu copies=%llu kb=%llu regions=%llu worker_ms=%llu wait_calls=%llu wait_ms=%llu\n",
					             static_cast<unsigned long long>(jobs),
					             static_cast<unsigned long long>(stat_copies.load(std::memory_order_relaxed)),
					             static_cast<unsigned long long>(stat_bytes.load(std::memory_order_relaxed) / 1024u),
					             static_cast<unsigned long long>(stat_regions.load(std::memory_order_relaxed)),
					             static_cast<unsigned long long>(stat_worker_ns.load(std::memory_order_relaxed) / 1'000'000u),
					             static_cast<unsigned long long>(stat_wait_calls.load(std::memory_order_relaxed)),
					             static_cast<unsigned long long>(stat_wait_ns.load(std::memory_order_relaxed) / 1'000'000u));
				}
			}
		}
	}

	bool EnqueueImpl(MemoryTracker* tracker, std::vector<CopyItem>& copies, std::vector<RegionManager*>& regions,
	                 bool allow_wait) noexcept {
		const uint64_t h = head.load(std::memory_order_relaxed);
		uint64_t       t = tail.load(std::memory_order_acquire);
		if (h - t >= RingSize) {
			if (!allow_wait) return false;
			while (h - t >= RingSize) {
				tail.wait(t, std::memory_order_acquire);
				t = tail.load(std::memory_order_acquire);
			}
		}
		auto&          slot  = slots[h & RingMask];
		const uint64_t epoch = published_epoch.load(std::memory_order_relaxed) + 1;
		slot.tracker         = tracker;
		slot.copies.swap(copies);
		copies.clear();
		slot.regions.swap(regions);
		regions.clear();
		slot.epoch = epoch;
		published_epoch.store(epoch, std::memory_order_release);
		head.store(h + 1, std::memory_order_seq_cst);
		if (worker_sleeping.load(std::memory_order_seq_cst)) {
			head.notify_one();
		}
		return true;
	}

	void WaitEpoch(uint64_t target) noexcept {
		if (target == 0 || completed_epoch.load(std::memory_order_acquire) >= target) return;
		const bool timed = PerfEnabled();
		const auto t0    = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
		waiters.fetch_add(1, std::memory_order_seq_cst);
		for (;;) {
			const uint64_t cur = completed_epoch.load(std::memory_order_acquire);
			if (cur >= target) break;
			completed_epoch.wait(cur, std::memory_order_acquire);
		}
		waiters.fetch_sub(1, std::memory_order_release);
		if (timed) {
			const auto ns = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
			stat_wait_calls.fetch_add(1, std::memory_order_relaxed);
			stat_wait_ns.fetch_add(ns, std::memory_order_relaxed);
		}
	}
};

std::atomic<Worker*> g_worker {nullptr};

Worker& GetWorker() noexcept {
	static Worker* w = [] {
		auto* ptr = new Worker();
		g_worker.store(ptr, std::memory_order_release);
		return ptr;
	}();
	return *w;
}

} // namespace

bool Enabled() noexcept {
	static const bool enabled = [] {
		if (const char* v = std::getenv("KYTY_ASYNC_HOST_COPY"); v != nullptr && v[0] == '0' && v[1] == '\0') {
			return false;
		}
		return true;
	}();
	return enabled;
}

uint64_t PublishedEpoch() noexcept {
	auto* w = g_worker.load(std::memory_order_acquire);
	return w != nullptr ? w->published_epoch.load(std::memory_order_acquire) : 0;
}

void WaitForEpoch(uint64_t epoch) noexcept {
	if (epoch == 0) return;
	if (auto* w = g_worker.load(std::memory_order_acquire); w != nullptr) {
		w->WaitEpoch(epoch);
	}
}

void Drain() noexcept {
	if (auto* w = g_worker.load(std::memory_order_acquire); w != nullptr) {
		w->WaitEpoch(w->published_epoch.load(std::memory_order_acquire));
	}
}

} // namespace AsyncHostCopy