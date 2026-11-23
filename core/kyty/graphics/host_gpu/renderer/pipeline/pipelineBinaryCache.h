#pragma once
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics::PipelineBinaryCache {
inline constexpr uint32_t kMagic = 0x3142504Bu; // "KPB1"

inline std::filesystem::path FilePath(const vk::PipelineBinaryKeyKHR& key) {
	static std::once_flag once;
	static const std::filesystem::path dir = std::filesystem::path("_PipelineCache") / "binaries";
	std::call_once(once, [] { std::error_code ec; std::filesystem::create_directories(dir, ec); });
	static constexpr char hex[] = "0123456789abcdef";
	const size_t n = std::min<size_t>(key.keySize, VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
	std::string name;
	name.reserve(n * 2 + 6);
	for (size_t i = 0; i < n; ++i) {
		name.push_back(hex[key.key[i] >> 4]);
		name.push_back(hex[key.key[i] & 0xFu]);
	}
	name += ".vkbin";
	return dir / name;
}

inline bool TryLoadCompute(GraphicContext& graphics, const vk::ComputePipelineCreateInfo& base_info,
                           const vk::PipelineBinaryKeyKHR& key, vk::Pipeline& out_pipe) {
	const auto path = FilePath(key);
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) return false;
	std::FILE* f = std::fopen(path.string().c_str(), "rb");
	if (!f) return false;
	uint32_t magic = 0, count = 0;
	bool ok = std::fread(&magic, 4, 1, f) == 1 && magic == kMagic &&
	          std::fread(&count, 4, 1, f) == 1 && count > 0 && count <= 64;
	std::vector<vk::PipelineBinaryKeyKHR> keys;
	std::vector<std::vector<uint8_t>> blobs;
	std::vector<vk::PipelineBinaryDataKHR> descs;
	if (ok) {
		keys.resize(count); blobs.resize(count); descs.resize(count);
		for (uint32_t i = 0; i < count; ++i) {
			uint32_t ks = 0; uint64_t ds = 0;
			if (std::fread(&ks, 4, 1, f) != 1 || ks > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) { ok = false; break; }
			keys[i].keySize = ks;
			if (ks > 0 && std::fread(keys[i].key.data(), 1, ks, f) != ks) { ok = false; break; }
			if (std::fread(&ds, 8, 1, f) != 1 || ds == 0 || ds > (256ull << 20)) { ok = false; break; }
			blobs[i].resize(static_cast<size_t>(ds));
			if (std::fread(blobs[i].data(), 1, blobs[i].size(), f) != blobs[i].size()) { ok = false; break; }
			descs[i].dataSize = blobs[i].size(); descs[i].pData = blobs[i].data();
		}
	}
	std::fclose(f);
	if (!ok) { std::filesystem::remove(path, ec); return false; }
	vk::PipelineBinaryKeysAndDataKHR kd {}; kd.binaryCount = count; kd.pPipelineBinaryKeys = keys.data(); kd.pPipelineBinaryData = descs.data();
	vk::PipelineBinaryCreateInfoKHR bci {}; bci.pKeysAndDataInfo = &kd;
	std::vector<vk::PipelineBinaryKHR> handles(count, nullptr);
	vk::PipelineBinaryHandlesInfoKHR hi {}; hi.pipelineBinaryCount = count; hi.pPipelineBinaries = handles.data();
	if (graphics.device.createPipelineBinariesKHR(&bci, nullptr, &hi) != vk::Result::eSuccess) {
		for (auto h: handles) if (h) graphics.device.destroyPipelineBinaryKHR(h, nullptr);
		std::filesystem::remove(path, ec);
		return false;
	}
	vk::PipelineBinaryInfoKHR bi {};
	bi.binaryCount = hi.pipelineBinaryCount;
	bi.pPipelineBinaries = handles.data();
	bi.pNext = const_cast<void*>(base_info.pNext);
	vk::ComputePipelineCreateInfo li = base_info;
	li.pNext = &bi;
	auto res = graphics.device.createComputePipelines(nullptr, 1, &li, nullptr, &out_pipe);
	for (auto h: handles) if (h) graphics.device.destroyPipelineBinaryKHR(h, nullptr);
	if (res == vk::Result::eSuccess && out_pipe != nullptr) return true;
	out_pipe = nullptr;
	std::filesystem::remove(path, ec);
	return false;
}

inline void SaveCompute(GraphicContext& graphics, vk::Pipeline pipe,
                        const vk::PipelineBinaryKeyKHR& key, uint64_t hash, double ms) {
	vk::PipelineBinaryCreateInfoKHR bci {}; bci.pipeline = pipe;
	vk::PipelineBinaryHandlesInfoKHR hi {};
	auto res = graphics.device.createPipelineBinariesKHR(&bci, nullptr, &hi);
	if (res != vk::Result::eSuccess || hi.pipelineBinaryCount == 0) {
		vk::ReleaseCapturedPipelineDataInfoKHR rel {}; rel.pipeline = pipe;
		(void)graphics.device.releaseCapturedPipelineDataKHR(&rel, nullptr);
		return;
	}
	std::vector<vk::PipelineBinaryKHR> handles(hi.pipelineBinaryCount, nullptr);
	hi.pPipelineBinaries = handles.data();
	res = graphics.device.createPipelineBinariesKHR(&bci, nullptr, &hi);
	vk::ReleaseCapturedPipelineDataInfoKHR rel {}; rel.pipeline = pipe;
	(void)graphics.device.releaseCapturedPipelineDataKHR(&rel, nullptr);
	if (res != vk::Result::eSuccess) {
		for (auto h: handles) if (h) graphics.device.destroyPipelineBinaryKHR(h, nullptr);
		return;
	}
	const auto path = FilePath(key);
	const auto tmp = path.string() + ".tmp." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	std::FILE* f = std::fopen(tmp.c_str(), "wb");
	bool ok = (f != nullptr);
	size_t bytes = 0;
	if (ok) {
		const uint32_t magic = kMagic, count = hi.pipelineBinaryCount;
		ok = std::fwrite(&magic, 4, 1, f) == 1 && std::fwrite(&count, 4, 1, f) == 1;
		for (uint32_t i = 0; ok && i < count; ++i) {
			vk::PipelineBinaryDataInfoKHR di {}; di.pipelineBinary = handles[i];
			vk::PipelineBinaryKeyKHR bk {}; size_t ds = 0;
			if (graphics.device.getPipelineBinaryDataKHR(&di, &bk, &ds, nullptr) != vk::Result::eSuccess || ds == 0) { ok = false; break; }
			std::vector<uint8_t> buf(ds);
			if (graphics.device.getPipelineBinaryDataKHR(&di, &bk, &ds, buf.data()) != vk::Result::eSuccess) { ok = false; break; }
			const uint32_t ks = bk.keySize; const uint64_t ds64 = ds;
			ok = std::fwrite(&ks, 4, 1, f) == 1 && (ks == 0 || std::fwrite(bk.key.data(), 1, ks, f) == ks) &&
			     std::fwrite(&ds64, 8, 1, f) == 1 && std::fwrite(buf.data(), 1, ds, f) == ds;
			bytes += ds;
		}
		std::fclose(f);
	}
	for (auto h: handles) if (h) graphics.device.destroyPipelineBinaryKHR(h, nullptr);
	std::error_code ec;
	if (ok) {
		std::filesystem::rename(tmp, path, ec);
		if (ec) std::filesystem::remove(tmp, ec);
		else if (ms >= 200.0) {
			std::fprintf(stderr, "PIPELINE_BINARY saved cs=0x%016llx bytes=%zu compile_ms=%.0f\n",
			             static_cast<unsigned long long>(hash), bytes, ms);
			std::fflush(stderr);
		}
	} else { std::filesystem::remove(tmp, ec); }
}

} // namespace Libs::Graphics::PipelineBinaryCache
