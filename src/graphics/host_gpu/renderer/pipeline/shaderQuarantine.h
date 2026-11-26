#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERQUARANTINE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERQUARANTINE_H_

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <unordered_set>

namespace Libs::Graphics {

class ShaderQuarantine {
public:
	explicit ShaderQuarantine(std::filesystem::path path = {});

	[[nodiscard]] bool Contains(uint64_t shader_hash) const;
	void Add(std::span<const uint64_t> shader_hashes);

private:
	bool SaveLocked() const;

	std::filesystem::path        m_path;
	mutable std::mutex           m_mutex;
	std::unordered_set<uint64_t> m_shader_hashes;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERQUARANTINE_H_
