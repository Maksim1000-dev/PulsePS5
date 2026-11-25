#ifndef KYTY_COMMON_ARCHIVE_READER_H_
#define KYTY_COMMON_ARCHIVE_READER_H_

#include "common/file.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Common {

// Backends expose immutable metadata and offset-based reads; path routing and cursor management
// are shared by every archive format.
class ArchiveReader {
public:
	struct Entry {
		uint64_t id;
		uint64_t size;
		bool     is_file;
	};

	virtual ~ArchiveReader()                                                       = default;
	virtual std::optional<Entry>        Find(std::string_view member)              = 0;
	virtual std::vector<File::DirEntry> List(std::string_view member)              = 0;
	virtual uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) = 0;
};

std::shared_ptr<ArchiveReader> OpenFFPFSCArchive(const std::filesystem::path& path);

} // namespace Common

#endif /* KYTY_COMMON_ARCHIVE_READER_H_ */
