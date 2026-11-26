#include "graphics/host_gpu/renderer/pipeline/shaderQuarantine.h"

#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <charconv>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Libs::Graphics {

ShaderQuarantine::ShaderQuarantine(std::filesystem::path path): m_path(std::move(path)) {
	if (m_path.empty() || !Common::File::IsFileExisting(m_path)) {
		return;
	}

	Common::File file(m_path, Common::File::Mode::Read);
	const auto   size = file.IsInvalid() ? 0 : file.Size();
	if (file.IsInvalid() || size > 1024 * 1024) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "Warning: unable to load shader quarantine file {}\n", Common::PathToString(m_path)));
		return;
	}

	const auto bytes = file.ReadWholeBuffer();
	const std::string_view contents =
	    bytes.empty()
	        ? std::string_view {}
	        : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	size_t                 begin = 0;
	while (begin < contents.size()) {
		const auto end = contents.find('\n', begin);
		auto       line = contents.substr(begin, end == std::string_view::npos
		                                          ? contents.size() - begin
		                                          : end - begin);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		uint64_t hash = 0;
		const auto [parsed_end, error] =
		    std::from_chars(line.data(), line.data() + line.size(), hash, 16);
		if (!line.empty() && error == std::errc {} && parsed_end == line.data() + line.size() &&
		    hash != 0) {
			m_shader_hashes.insert(hash);
		}
		if (end == std::string_view::npos) {
			break;
		}
		begin = end + 1;
	}
}

bool ShaderQuarantine::Contains(uint64_t shader_hash) const {
	if (shader_hash == 0) {
		return false;
	}
	std::lock_guard lock(m_mutex);
	return m_shader_hashes.contains(shader_hash);
}

void ShaderQuarantine::Add(std::span<const uint64_t> shader_hashes) {
	std::lock_guard lock(m_mutex);
	std::vector<uint64_t> added;
	for (const auto hash: shader_hashes) {
		if (hash != 0 && m_shader_hashes.insert(hash).second) {
			added.push_back(hash);
		}
	}
	if (added.empty()) {
		return;
	}

	for (const auto hash: added) {
		Log::WriteToConsoleAndLog(
		    fmt::format("Warning: quarantining slow/hanging shader hash=0x{:016x}\n", hash));
	}
	if (!SaveLocked()) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "Warning: failed to save shader quarantine file {}\n", Common::PathToString(m_path)));
	}
}

bool ShaderQuarantine::SaveLocked() const {
	if (m_path.empty()) {
		return true;
	}
	if (!m_path.parent_path().empty() &&
	    !Common::File::CreateDirectories(m_path.parent_path())) {
		return false;
	}

	std::vector<uint64_t> hashes(m_shader_hashes.begin(), m_shader_hashes.end());
	std::ranges::sort(hashes);
	std::string contents;
	for (const auto hash: hashes) {
		contents += fmt::format("{:016x}\n", hash);
	}

	auto temporary = m_path;
	temporary += ".tmp";
	Common::File file;
	if (!file.Create(temporary)) {
		return false;
	}
	uint32_t written = 0;
	file.Write(contents.data(), static_cast<uint32_t>(contents.size()), &written);
	const bool flushed = written == contents.size() && file.Flush();
	file.Close();
	if (!flushed || !Common::File::RenameFile(temporary, m_path)) {
		Common::File::DeleteFile(temporary);
		return false;
	}
	return true;
}

} // namespace Libs::Graphics
