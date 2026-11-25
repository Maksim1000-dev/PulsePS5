#include "common/archiveReader.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Common {
namespace {

constexpr uint64_t kPfsMagic             = 20130315;
constexpr uint64_t kMaxPfsInodes         = 4096;
constexpr uint64_t kMaxPfscBlocks        = 8ull * 1024 * 1024;
constexpr uint64_t kMaxExfatFatBytes     = 256ull * 1024 * 1024;
constexpr uint64_t kMaxExfatDirectory    = 64ull * 1024 * 1024;
constexpr uint64_t kMaxExfatEntries      = 1ull * 1024 * 1024;
constexpr uint64_t kMaxExfatDepth        = 128;
constexpr uint32_t kPfscBlockSize        = 0x10000;
constexpr uint32_t kPfsInodeSize         = 0xA8;
constexpr uint32_t kPfscOffsetTable      = 0x400;
constexpr uint32_t kPfscInitialData      = 0x10000;
constexpr uint32_t kExfatFatEndMinimum   = 0xFFFFFFF8;
constexpr uint8_t  kExfatFile            = 0x85;
constexpr uint8_t  kExfatStream           = 0xC0;
constexpr uint8_t  kExfatFileName         = 0xC1;
constexpr uint16_t kExfatDirectoryAttrib = 0x10;
constexpr uint8_t  kExfatNoFatChain       = 0x02;

uint16_t Load16(const uint8_t* data) {
	return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8u));
}

uint32_t Load32(const uint8_t* data) {
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
	       (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

uint64_t Load64(const uint8_t* data) {
	return static_cast<uint64_t>(Load32(data)) |
	       (static_cast<uint64_t>(Load32(data + 4)) << 32u);
}

bool IsRangeValid(uint64_t offset, uint64_t size, uint64_t limit) {
	return offset <= limit && size <= limit - offset;
}

std::string FoldAscii(std::string_view value) {
	std::string folded(value);
	for (auto& ch: folded) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = static_cast<char>(ch + ('a' - 'A'));
		}
	}
	return folded;
}

bool Utf16ToUtf8(const std::vector<uint16_t>& input, std::string& output) {
	output.clear();
	for (size_t i = 0; i < input.size(); ++i) {
		uint32_t codepoint = input[i];
		if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
			if (i + 1 >= input.size() || input[i + 1] < 0xDC00 || input[i + 1] > 0xDFFF) {
				return false;
			}
			codepoint = 0x10000 + ((codepoint - 0xD800) << 10u) + (input[++i] - 0xDC00);
		} else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
			return false;
		}

		if (codepoint <= 0x7F) {
			output.push_back(static_cast<char>(codepoint));
		} else if (codepoint <= 0x7FF) {
			output.push_back(static_cast<char>(0xC0 | (codepoint >> 6u)));
			output.push_back(static_cast<char>(0x80 | (codepoint & 0x3Fu)));
		} else if (codepoint <= 0xFFFF) {
			output.push_back(static_cast<char>(0xE0 | (codepoint >> 12u)));
			output.push_back(static_cast<char>(0x80 | ((codepoint >> 6u) & 0x3Fu)));
			output.push_back(static_cast<char>(0x80 | (codepoint & 0x3Fu)));
		} else {
			output.push_back(static_cast<char>(0xF0 | (codepoint >> 18u)));
			output.push_back(static_cast<char>(0x80 | ((codepoint >> 12u) & 0x3Fu)));
			output.push_back(static_cast<char>(0x80 | ((codepoint >> 6u) & 0x3Fu)));
			output.push_back(static_cast<char>(0x80 | (codepoint & 0x3Fu)));
		}
	}
	return true;
}

struct PfsInode {
	uint16_t mode            = 0;
	uint32_t flags           = 0;
	uint64_t size            = 0;
	uint64_t logical_size    = 0;
	uint32_t block_count     = 0;
	int32_t  first_block     = -1;
};

struct PfsDirent {
	uint32_t inode = 0;
	uint32_t type  = 0;
	std::string name;
};

class FFPFSCReader final: public ArchiveReader {
public:
	static std::shared_ptr<ArchiveReader> Open(const std::filesystem::path& path) {
		auto reader = std::shared_ptr<FFPFSCReader>(new FFPFSCReader(path));
		return reader->Initialize() ? reader : nullptr;
	}

	std::optional<Entry> Find(std::string_view member) override {
		const auto key = NormalizeMember(member);
		const auto it  = m_paths.find(key);
		if (it == m_paths.end()) {
			return {};
		}
		const auto& node = m_nodes[it->second];
		return Entry {it->second, node.size, node.is_file};
	}

	std::vector<File::DirEntry> List(std::string_view member) override {
		const auto entry = Find(member);
		if (!entry || entry->is_file) {
			return {};
		}

		std::vector<File::DirEntry> result;
		const auto& node = m_nodes[entry->id];
		result.reserve(node.children.size());
		for (const auto child: node.children) {
			result.push_back({m_nodes[child].name, m_nodes[child].is_file});
		}
		return result;
	}

	uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) override {
		if (data == nullptr || id >= m_nodes.size() || !m_nodes[id].is_file) {
			return 0;
		}
		const auto& node = m_nodes[id];
		if (offset >= node.size) {
			return 0;
		}

		const auto requested = static_cast<size_t>(std::min<uint64_t>(size, node.size - offset));
		size_t     done      = 0;
		std::lock_guard lock(m_mutex);
		if (node.from_pfs) {
			return ReadLogical(node.image_offset + offset, requested, data) ? requested : 0;
		}
		while (done < requested) {
			const uint64_t file_offset = offset + done;
			const uint64_t cluster_idx = file_offset / m_cluster_size;
			const uint64_t in_cluster  = file_offset % m_cluster_size;
			if (cluster_idx >= node.clusters.size()) {
				break;
			}

			const auto step = static_cast<size_t>(
			    std::min<uint64_t>(requested - done, m_cluster_size - in_cluster));
			const auto image_offset =
			    m_heap_offset + static_cast<uint64_t>(node.clusters[cluster_idx] - 2) *
			                        m_cluster_size +
			    in_cluster;
			if (!ReadLogical(image_offset, step, static_cast<uint8_t*>(data) + done)) {
				break;
			}
			done += step;
		}
		return done;
	}

private:
	struct Node {
		std::string name;
		std::string path;
		uint64_t    size      = 0;
		uint64_t    image_offset = 0;
		bool        is_file   = false;
		bool        from_pfs  = false;
		std::vector<uint64_t> children;
		std::vector<uint32_t> clusters;
	};

	struct PfscCacheBlock {
		uint64_t                 index = std::numeric_limits<uint64_t>::max();
		uint64_t                 age   = 0;
		std::array<uint8_t, kPfscBlockSize> data {};
	};

	explicit FFPFSCReader(const std::filesystem::path& path): m_file(path, File::Mode::Read) {}

	bool ReadHost(uint64_t offset, size_t size, void* data) {
		if (size > std::numeric_limits<uint32_t>::max() || !IsRangeValid(offset, size, m_file.Size()) ||
		    !m_file.Seek(offset)) {
			return false;
		}
		uint32_t bytes_read = 0;
		m_file.Read(data, static_cast<uint32_t>(size), &bytes_read);
		return bytes_read == size;
	}

	bool ReadLogical(uint64_t offset, size_t size, void* output) {
		if (!IsRangeValid(offset, size, m_image_size)) {
			return false;
		}
		auto* out = static_cast<uint8_t*>(output);
		size_t done = 0;
		while (done < size) {
			const auto block_index = (offset + done) / kPfscBlockSize;
			const auto block_offset = (offset + done) % kPfscBlockSize;
			const auto step = static_cast<size_t>(
			    std::min<uint64_t>(size - done, kPfscBlockSize - block_offset));
			const auto* block = GetPfscBlock(block_index);
			if (block == nullptr) {
				return false;
			}
			std::memcpy(out + done, block + block_offset, step);
			done += step;
		}
		return true;
	}

	const uint8_t* GetPfscBlock(uint64_t index) {
		for (auto& cache: m_cache) {
			if (cache.index == index) {
				cache.age = ++m_cache_age;
				return cache.data.data();
			}
		}
		if (index >= m_block_offsets.size() - 1) {
			return nullptr;
		}

		auto* slot = &m_cache.front();
		for (auto& cache: m_cache) {
			if (cache.index == std::numeric_limits<uint64_t>::max() || cache.age < slot->age) {
				slot = &cache;
			}
		}

		const auto start = m_block_offsets[index];
		const auto end   = m_block_offsets[index + 1];
		if (end < start || end - start == 0 || end - start > kPfscBlockSize) {
			return nullptr;
		}
		const auto stored_size = static_cast<size_t>(end - start);
		if (!ReadHost(m_payload_offset + start, stored_size, m_compressed.data())) {
			return nullptr;
		}
		if (stored_size == kPfscBlockSize) {
			std::memcpy(slot->data.data(), m_compressed.data(), kPfscBlockSize);
		} else {
			z_stream stream {};
			stream.next_in   = m_compressed.data();
			stream.avail_in  = static_cast<uInt>(stored_size);
			stream.next_out  = slot->data.data();
			stream.avail_out = kPfscBlockSize;
			if (inflateInit(&stream) != Z_OK) {
				return nullptr;
			}
			const int inflate_result = inflate(&stream, Z_FINISH);
			const bool valid = inflate_result == Z_STREAM_END && stream.total_out == kPfscBlockSize &&
			                   stream.avail_in == 0;
			inflateEnd(&stream);
			if (!valid) {
				return nullptr;
			}
		}
		slot->index = index;
		slot->age   = ++m_cache_age;
		return slot->data.data();
	}

	bool Initialize() {
		if (m_file.IsInvalid() || !ReadPfs() || !ReadPfsc() || !ReadImage()) {
			return false;
		}
		return true;
	}

	bool ReadPfsDirectory(const PfsInode& inode, uint32_t block_size,
	                      std::vector<PfsDirent>& entries, bool from_image = false) {
		if (inode.size > kMaxExfatDirectory || inode.first_block <= 0 ||
		    inode.block_count == 0) {
			return false;
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(inode.size));
		const auto offset = static_cast<uint64_t>(inode.first_block) * block_size;
		const auto inode_count = from_image ? m_inner_inodes.size() : m_inodes.size();
		const bool read_ok =
		    from_image ? IsRangeValid(offset, inode.size, m_image_size) &&
		                     ReadLogical(offset, bytes.size(), bytes.data())
		               : IsRangeValid(offset, inode.size, m_file.Size()) &&
		                     ReadHost(offset, bytes.size(), bytes.data());
		if (!read_ok) {
			return false;
		}

		for (size_t at = 0; at + 16 <= bytes.size();) {
			const auto inode_id = Load32(bytes.data() + at);
			const auto type      = Load32(bytes.data() + at + 4);
			const auto name_size = Load32(bytes.data() + at + 8);
			const auto entry_size = Load32(bytes.data() + at + 12);
			if (inode_id == 0 && type == 0 && name_size == 0 && entry_size == 0) {
				break;
			}
			if (entry_size < 16 || (entry_size & 7u) != 0 || name_size > entry_size - 16 ||
			    entry_size > bytes.size() - at || inode_id >= inode_count) {
				return false;
			}
			std::string name(reinterpret_cast<const char*>(bytes.data() + at + 16), name_size);
			if (name.find('\0') != std::string::npos) {
				return false;
			}
			entries.push_back({inode_id, type, std::move(name)});
			at += entry_size;
		}
		return true;
	}

	bool ReadPfs() {
		std::array<uint8_t, 0x100> header {};
		if (m_file.Size() < header.size() || !ReadHost(0, header.size(), header.data()) ||
		    Load64(header.data()) != 2 || Load64(header.data() + 8) != kPfsMagic) {
			return false;
		}
		const auto mode = Load16(header.data() + 0x1C);
		if ((mode & ~0x8u) != 0) {
			return false;
		}
		m_pfs_block_size = Load32(header.data() + 0x20);
		const auto inode_count = Load64(header.data() + 0x30);
		const auto block_count = Load64(header.data() + 0x38);
		const auto inode_blocks = Load64(header.data() + 0x40);
		if (m_pfs_block_size < 0x1000 || m_pfs_block_size > 0x100000 ||
		    (m_pfs_block_size & (m_pfs_block_size - 1)) != 0 || inode_count != 4 ||
		    inode_blocks == 0 || inode_count > kMaxPfsInodes ||
		    inode_blocks > std::numeric_limits<uint64_t>::max() / m_pfs_block_size ||
		    block_count > std::numeric_limits<uint64_t>::max() / m_pfs_block_size ||
		    block_count * m_pfs_block_size > m_file.Size() ||
		    inode_blocks >= block_count ||
		    inode_count > inode_blocks * (m_pfs_block_size / kPfsInodeSize)) {
			return false;
		}

		std::vector<uint8_t> inode_data(static_cast<size_t>(inode_count) * kPfsInodeSize);
		if (!ReadHost(m_pfs_block_size, inode_data.size(), inode_data.data())) {
			return false;
		}
		m_inodes.resize(static_cast<size_t>(inode_count));
		for (size_t i = 0; i < m_inodes.size(); ++i) {
			const auto* record = inode_data.data() + i * kPfsInodeSize;
			auto& inode = m_inodes[i];
			inode.mode         = Load16(record);
			inode.flags        = Load32(record + 4);
			inode.size         = Load64(record + 8);
			inode.logical_size = Load64(record + 0x10);
			inode.block_count  = Load32(record + 0x60);
			inode.first_block  = static_cast<int32_t>(Load32(record + 0x64));
			if (inode.first_block <= 0 || inode.block_count == 0 ||
			    static_cast<uint64_t>(inode.first_block) >= block_count ||
			    static_cast<uint64_t>(inode.first_block) < 1 + inode_blocks ||
			    inode.block_count > block_count - static_cast<uint64_t>(inode.first_block) ||
			    inode.size > static_cast<uint64_t>(inode.block_count) * m_pfs_block_size) {
				return false;
			}
		}

		if ((m_inodes[0].mode & 0xF000u) != 0x4000u ||
		    (m_inodes[2].mode & 0xF000u) != 0x4000u ||
		    (m_inodes[3].mode & 0xF000u) != 0x8000u ||
		    (m_inodes[3].flags & 0x1u) == 0 || m_inodes[3].logical_size == 0) {
			return false;
		}

		std::vector<PfsDirent> super_root;
		if (!ReadPfsDirectory(m_inodes[0], m_pfs_block_size, super_root)) {
			return false;
		}
		bool has_path_table = false;
		bool has_user_root  = false;
		for (const auto& entry: super_root) {
			has_path_table |= entry.inode == 1 && entry.type == 2 && entry.name == "flat_path_table";
			has_user_root |= entry.inode == 2 && entry.type == 3 && entry.name == "uroot";
		}
		if (!has_path_table || !has_user_root) {
			return false;
		}

		std::vector<PfsDirent> user_root;
		if (!ReadPfsDirectory(m_inodes[2], m_pfs_block_size, user_root)) {
			return false;
		}
		size_t payload_entries = 0;
		for (const auto& entry: user_root) {
			if (entry.inode == 2 && entry.type == 4 && entry.name == ".") {
				continue;
			}
			if ((entry.inode == 0 || entry.inode == 2) && entry.type == 5 &&
			    entry.name == "..") {
				continue;
			}
			if (entry.inode != 3 || entry.type != 2 || entry.name.empty()) {
				return false;
			}
			++payload_entries;
		}
		if (payload_entries != 1) {
			return false;
		}

		m_payload_offset = static_cast<uint64_t>(m_inodes[3].first_block) * m_pfs_block_size;
		m_payload_size   = m_inodes[3].size;
		m_image_size     = m_inodes[3].logical_size;
		return IsRangeValid(m_payload_offset, m_payload_size, m_file.Size());
	}

	bool ReadPfsc() {
		std::array<uint8_t, 0x30> header {};
		if (m_payload_size < header.size() || !ReadHost(m_payload_offset, header.size(), header.data()) ||
		    Load32(header.data()) != 0x43534650u || Load32(header.data() + 4) != 0 ||
		    Load32(header.data() + 8) != 6 || Load32(header.data() + 0x0C) != kPfscBlockSize ||
		    Load64(header.data() + 0x10) != kPfscBlockSize) {
			return false;
		}
		const auto table_offset = Load64(header.data() + 0x18);
		const auto data_offset  = Load64(header.data() + 0x20);
		const auto logical_size = Load64(header.data() + 0x28);
		if (table_offset != kPfscOffsetTable || table_offset > data_offset ||
		    data_offset < kPfscInitialData || data_offset % kPfscBlockSize != 0 ||
		    logical_size == 0 || logical_size % kPfscBlockSize != 0 ||
		    m_image_size > logical_size || logical_size - m_image_size >= kPfscBlockSize) {
			return false;
		}
		const auto block_count = logical_size / kPfscBlockSize;
		if (block_count == 0 || block_count > kMaxPfscBlocks ||
		    block_count + 1 > std::numeric_limits<uint64_t>::max() / 8) {
			return false;
		}
		const auto table_size = (block_count + 1) * 8;
		if (!IsRangeValid(table_offset, table_size, data_offset) ||
		    !IsRangeValid(data_offset, 0, m_payload_size)) {
			return false;
		}

		std::vector<uint8_t> table(static_cast<size_t>(table_size));
		if (!ReadHost(m_payload_offset + table_offset, table.size(), table.data())) {
			return false;
		}
		m_block_offsets.resize(static_cast<size_t>(block_count) + 1);
		for (size_t i = 0; i < m_block_offsets.size(); ++i) {
			const auto block_offset = Load64(table.data() + i * 8);
			if (block_offset < data_offset || block_offset > m_payload_size ||
			    (i != 0 && block_offset <= m_block_offsets[i - 1]) ||
			    (i != 0 && block_offset - m_block_offsets[i - 1] > kPfscBlockSize)) {
				return false;
			}
			m_block_offsets[i] = block_offset;
		}
		return m_block_offsets.front() == data_offset &&
		       m_block_offsets.back() <= m_payload_size;
	}

	bool ReadImage() {
		std::array<uint8_t, 0x10> header {};
		if (!ReadLogical(0, header.size(), header.data())) {
			return false;
		}
		if (Load64(header.data()) == 2 && Load64(header.data() + 8) == kPfsMagic) {
			return ReadInnerPfs();
		}
		return ReadExfat();
	}

	bool ReadInnerPfs() {
		std::array<uint8_t, 0x100> header {};
		if (!ReadLogical(0, header.size(), header.data()) ||
		    Load64(header.data()) != 2 || Load64(header.data() + 8) != kPfsMagic) {
			return false;
		}
		const auto mode = Load16(header.data() + 0x1C);
		if ((mode & ~0x8u) != 0) {
			return false;
		}
		m_inner_pfs_block_size = Load32(header.data() + 0x20);
		const auto inode_count  = Load64(header.data() + 0x30);
		const auto block_count  = Load64(header.data() + 0x38);
		const auto inode_blocks = Load64(header.data() + 0x40);
		if (m_inner_pfs_block_size < 0x1000 || m_inner_pfs_block_size > 0x100000 ||
		    (m_inner_pfs_block_size & (m_inner_pfs_block_size - 1)) != 0 ||
		    inode_count == 0 || inode_count > kMaxPfsInodes || inode_blocks == 0 ||
		    block_count > std::numeric_limits<uint64_t>::max() / m_inner_pfs_block_size ||
		    block_count * m_inner_pfs_block_size > m_image_size || inode_blocks >= block_count ||
		    inode_count > inode_blocks * (m_inner_pfs_block_size / kPfsInodeSize)) {
			return false;
		}

		std::vector<uint8_t> inode_data(static_cast<size_t>(inode_count) * kPfsInodeSize);
		if (!ReadLogical(m_inner_pfs_block_size, inode_data.size(), inode_data.data())) {
			return false;
		}
		m_inner_inodes.resize(static_cast<size_t>(inode_count));
		for (size_t i = 0; i < m_inner_inodes.size(); ++i) {
			const auto* record = inode_data.data() + i * kPfsInodeSize;
			auto&       inode  = m_inner_inodes[i];
			inode.mode         = Load16(record);
			inode.flags        = Load32(record + 4);
			inode.size         = Load64(record + 8);
			inode.logical_size = Load64(record + 0x10);
			inode.block_count  = Load32(record + 0x60);
			inode.first_block  = static_cast<int32_t>(Load32(record + 0x64));

			const auto file_type = inode.mode & 0xF000u;
			if ((file_type != 0x4000u && file_type != 0x8000u) ||
			    inode.size > static_cast<uint64_t>(inode.block_count) *
			                     m_inner_pfs_block_size ||
			    (inode.size != 0 &&
			     (inode.first_block <= 0 || inode.block_count == 0 ||
			      static_cast<uint64_t>(inode.first_block) < 1 + inode_blocks ||
			      static_cast<uint64_t>(inode.first_block) >= block_count ||
			      inode.block_count > block_count - static_cast<uint64_t>(inode.first_block)))) {
				return false;
			}
		}

		std::vector<PfsDirent> super_root;
		if (!ReadPfsDirectory(m_inner_inodes[0], m_inner_pfs_block_size, super_root, true)) {
			return false;
		}
		bool     has_path_table = false;
		uint32_t user_root      = 0;
		for (const auto& entry: super_root) {
			has_path_table |= entry.inode == 1 && entry.type == 2 &&
			                  entry.name == "flat_path_table";
			if (entry.type == 3 && entry.name == "uroot") {
				if (user_root != 0 || entry.inode >= m_inner_inodes.size()) {
					return false;
				}
				user_root = entry.inode;
			}
		}
		if (!has_path_table || user_root == 0 ||
		    (m_inner_inodes[user_root].mode & 0xF000u) != 0x4000u) {
			return false;
		}

		m_nodes.clear();
		m_paths.clear();
		m_nodes.emplace_back();
		m_paths.emplace(std::string {}, 0);
		std::unordered_set<uint32_t> visiting;
		return ParseInnerPfsDirectory(user_root, 0, 0, visiting);
	}

	bool ParseInnerPfsDirectory(uint32_t inode_id, uint64_t parent_id, uint64_t depth,
	                            std::unordered_set<uint32_t>& visiting) {
		if (depth > kMaxExfatDepth || inode_id >= m_inner_inodes.size() ||
		    m_nodes.size() > kMaxExfatEntries || !visiting.insert(inode_id).second) {
			return false;
		}
		const auto& inode = m_inner_inodes[inode_id];
		std::vector<PfsDirent> entries;
		if (!ReadPfsDirectory(inode, m_inner_pfs_block_size, entries, true)) {
			return false;
		}

		for (const auto& entry: entries) {
			if (entry.name == "." || entry.name == "..") {
				continue;
			}
			if (entry.name.empty() || entry.name.find_first_of("/\\") != std::string::npos ||
			    entry.inode >= m_inner_inodes.size() ||
			    (entry.type != 2 && entry.type != 3)) {
				return false;
			}
			const auto& child_inode = m_inner_inodes[entry.inode];
			const auto  file_type   = child_inode.mode & 0xF000u;
			const bool  is_directory = entry.type == 3;
			if ((is_directory && file_type != 0x4000u) ||
			    (!is_directory && file_type != 0x8000u)) {
				return false;
			}

			const auto& parent = m_nodes[parent_id];
			const auto  path   = parent.path.empty() ? entry.name : parent.path + "/" + entry.name;
			const auto  key    = FoldAscii(path);
			if (m_paths.contains(key) || m_nodes.size() >= kMaxExfatEntries) {
				return false;
			}

			Node child;
			child.name          = entry.name;
			child.path          = path;
			child.size          = is_directory ? 0 : child_inode.size;
			child.image_offset  = static_cast<uint64_t>(child_inode.first_block) *
			                     m_inner_pfs_block_size;
			child.is_file       = !is_directory;
			child.from_pfs      = true;
			const auto child_id = static_cast<uint64_t>(m_nodes.size());
			m_nodes.push_back(std::move(child));
			m_paths.emplace(key, child_id);
			m_nodes[parent_id].children.push_back(child_id);

			if (is_directory &&
			    (child_inode.size == 0 ||
			     !ParseInnerPfsDirectory(entry.inode, child_id, depth + 1, visiting))) {
				return false;
			}
		}
		visiting.erase(inode_id);
		return true;
	}

	uint32_t FatEntry(uint32_t cluster) const {
		return m_fat[cluster];
	}

	bool CollectClusters(uint32_t first, uint64_t size, bool contiguous, bool until_end,
	                     std::vector<uint32_t>& clusters) const {
		clusters.clear();
		const auto cluster_count = static_cast<uint64_t>(m_cluster_count) + 2;
		if (!until_end && size == 0) {
			return true;
		}
		if (first < 2 || first >= cluster_count) {
			return false;
		}
		const auto wanted = until_end ? uint64_t {0} : (size + m_cluster_size - 1) / m_cluster_size;
		if (!until_end && wanted > m_cluster_count) {
			return false;
		}
		const auto max_directory_clusters =
		    std::max<uint64_t>(1, kMaxExfatDirectory / m_cluster_size);
		if (contiguous && !until_end) {
			if (wanted != 0 && static_cast<uint64_t>(first) + wanted > cluster_count) {
				return false;
			}
			clusters.reserve(static_cast<size_t>(wanted));
			for (uint64_t i = 0; i < wanted; ++i) {
				clusters.push_back(static_cast<uint32_t>(first + i));
			}
			return true;
		}

		std::unordered_set<uint32_t> seen;
		auto cluster = first;
		while (cluster < kExfatFatEndMinimum) {
			if (cluster < 2 || cluster >= cluster_count || !seen.insert(cluster).second ||
			    clusters.size() >= m_cluster_count ||
			    (until_end && clusters.size() >= max_directory_clusters)) {
				return false;
			}
			clusters.push_back(cluster);
			if (!until_end && clusters.size() == wanted) {
				return true;
			}
			cluster = FatEntry(cluster);
		}
		return until_end ? !clusters.empty() : clusters.size() == wanted;
	}

	bool ReadClusters(const std::vector<uint32_t>& clusters, uint64_t size,
	                  std::vector<uint8_t>& output) {
		if (size > kMaxExfatDirectory || size > std::numeric_limits<size_t>::max()) {
			return false;
		}
		output.resize(static_cast<size_t>(size));
		size_t done = 0;
		for (const auto cluster: clusters) {
			if (done == output.size()) {
				break;
			}
			const auto step = std::min<size_t>(output.size() - done, m_cluster_size);
			const auto offset =
			    m_heap_offset + static_cast<uint64_t>(cluster - 2) * m_cluster_size;
			if (!ReadLogical(offset, step, output.data() + done)) {
				return false;
			}
			done += step;
		}
		return done == output.size();
	}

	bool ParseDirectory(uint64_t parent_id, uint32_t first_cluster, uint64_t size,
	                    bool contiguous, bool root, uint64_t depth,
	                    std::unordered_set<uint32_t>& visited) {
		if (depth > kMaxExfatDepth || m_nodes.size() > kMaxExfatEntries ||
		    !visited.insert(first_cluster).second) {
			return false;
		}
		std::vector<uint32_t> clusters;
		if (!CollectClusters(first_cluster, size, contiguous, root, clusters)) {
			return false;
		}
		uint64_t directory_size = root ? static_cast<uint64_t>(clusters.size()) * m_cluster_size : size;
		if (directory_size > kMaxExfatDirectory) {
			return false;
		}
		std::vector<uint8_t> data;
		if (!ReadClusters(clusters, directory_size, data)) {
			return false;
		}

		for (size_t offset = 0; offset + 32 <= data.size();) {
			if (data[offset] == 0) {
				break;
			}
			if (data[offset] != kExfatFile) {
				offset += 32;
				continue;
			}
			const auto secondary_count = data[offset + 1];
			const auto set_size        = (static_cast<size_t>(secondary_count) + 1) * 32;
			if (secondary_count < 2 || set_size > data.size() - offset) {
				return false;
			}
			const auto* primary = data.data() + offset;
			const auto* stream  = primary + 32;
			if (stream[0] != kExfatStream) {
				return false;
			}
			const auto name_length = stream[3];
			const auto name_entries = (static_cast<size_t>(name_length) + 14) / 15;
			if (name_length == 0 ||
			    name_entries > static_cast<size_t>(secondary_count - 1)) {
				return false;
			}
			std::vector<uint16_t> utf16_name;
			utf16_name.reserve(name_length);
			for (size_t entry_index = 0; entry_index < name_entries; ++entry_index) {
				const auto* name_entry = primary + (entry_index + 2) * 32;
				if (name_entry[0] != kExfatFileName) {
					return false;
				}
				for (size_t char_index = 0; char_index < 15 && utf16_name.size() < name_length;
				     ++char_index) {
					utf16_name.push_back(Load16(name_entry + 2 + char_index * 2));
				}
			}
			std::string name;
			if (utf16_name.size() != name_length || !Utf16ToUtf8(utf16_name, name) ||
			    name.empty() || name == "." || name == ".." ||
			    name.find_first_of("/\\") != std::string::npos || name.find('\0') != std::string::npos) {
				return false;
			}
			const bool is_directory = (Load16(primary + 4) & kExfatDirectoryAttrib) != 0;
			const bool no_fat_chain = (stream[1] & kExfatNoFatChain) != 0;
			const auto child_cluster = Load32(stream + 0x14);
			const auto child_size    = Load64(stream + 0x18);
			if (child_size > std::numeric_limits<int64_t>::max()) {
				return false;
			}

			const auto& parent = m_nodes[parent_id];
			const auto path    = parent.path.empty() ? name : parent.path + "/" + name;
			const auto key     = FoldAscii(path);
			if (m_paths.contains(key) || m_nodes.size() >= kMaxExfatEntries) {
				return false;
			}

			Node child;
			child.name    = name;
			child.path    = path;
			child.size    = is_directory ? 0 : child_size;
			child.is_file = !is_directory;
			if (!CollectClusters(child_cluster, child_size, no_fat_chain, false,
			                     child.clusters)) {
				return false;
			}
			const auto child_id = static_cast<uint64_t>(m_nodes.size());
			m_nodes.push_back(std::move(child));
			m_paths.emplace(key, child_id);
			m_nodes[parent_id].children.push_back(child_id);

			if (is_directory) {
				if (child_size == 0 ||
				    !ParseDirectory(child_id, child_cluster, child_size, no_fat_chain, false,
				                    depth + 1, visited)) {
					return false;
				}
				visited.erase(child_cluster);
			}
			offset += set_size;
		}
		return true;
	}

	bool ReadExfat() {
		std::array<uint8_t, 512> boot {};
		if (m_image_size < boot.size() || !ReadLogical(0, boot.size(), boot.data()) ||
		    std::memcmp(boot.data() + 3, "EXFAT   ", 8) != 0 ||
		    Load16(boot.data() + 0x1FE) != 0xAA55) {
			return false;
		}
		const auto sector_shift  = boot[0x6C];
		const auto cluster_shift = boot[0x6D];
		if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 25 ||
		    static_cast<unsigned>(sector_shift) + cluster_shift > 31 || boot[0x6E] != 1) {
			return false;
		}
		const auto sector_size = 1u << sector_shift;
		m_cluster_size = sector_size << cluster_shift;
		const auto volume_sectors = Load64(boot.data() + 0x48);
		const auto fat_sector     = Load32(boot.data() + 0x50);
		const auto fat_sectors    = Load32(boot.data() + 0x54);
		const auto heap_sector    = Load32(boot.data() + 0x58);
		m_cluster_count           = Load32(boot.data() + 0x5C);
		const auto root_cluster   = Load32(boot.data() + 0x60);
		if (volume_sectors == 0 || volume_sectors > m_image_size / sector_size ||
		    m_cluster_count == 0 || root_cluster < 2 ||
		    root_cluster >= static_cast<uint64_t>(m_cluster_count) + 2) {
			return false;
		}
		const auto volume_bytes = volume_sectors * sector_size;
		const auto fat_offset   = static_cast<uint64_t>(fat_sector) * sector_size;
		const auto fat_bytes    = static_cast<uint64_t>(fat_sectors) * sector_size;
		m_heap_offset           = static_cast<uint64_t>(heap_sector) * sector_size;
		const auto required_fat = (static_cast<uint64_t>(m_cluster_count) + 2) * sizeof(uint32_t);
		const auto heap_bytes   = static_cast<uint64_t>(m_cluster_count) * m_cluster_size;
		if (!IsRangeValid(fat_offset, fat_bytes, volume_bytes) ||
		    fat_bytes < required_fat || required_fat > kMaxExfatFatBytes ||
		    !IsRangeValid(m_heap_offset, heap_bytes, volume_bytes)) {
			return false;
		}

		std::vector<uint8_t> fat_data(static_cast<size_t>(required_fat));
		if (!ReadLogical(fat_offset, fat_data.size(), fat_data.data())) {
			return false;
		}
		m_fat.resize(static_cast<size_t>(m_cluster_count) + 2);
		for (size_t i = 0; i < m_fat.size(); ++i) {
			m_fat[i] = Load32(fat_data.data() + i * sizeof(uint32_t));
		}

		m_nodes.push_back(Node {});
		m_paths.emplace(std::string {}, 0);
		std::unordered_set<uint32_t> visited;
		return ParseDirectory(0, root_cluster, 0, false, true, 0, visited);
	}

	static std::string NormalizeMember(std::string_view member) {
		while (!member.empty() && (member.front() == '/' || member.front() == '\\')) {
			member.remove_prefix(1);
		}
		std::string normalized(member);
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		return FoldAscii(normalized);
	}

	File m_file;
	std::mutex m_mutex;
	uint32_t m_pfs_block_size = 0;
	uint32_t m_inner_pfs_block_size = 0;
	uint64_t m_payload_offset = 0;
	uint64_t m_payload_size = 0;
	uint64_t m_image_size = 0;
	std::vector<PfsInode> m_inodes;
	std::vector<PfsInode> m_inner_inodes;
	std::vector<uint64_t> m_block_offsets;
	std::vector<uint8_t> m_compressed = std::vector<uint8_t>(kPfscBlockSize);
	std::array<PfscCacheBlock, 4> m_cache {};
	uint64_t m_cache_age = 0;
	uint32_t m_cluster_size = 0;
	uint32_t m_cluster_count = 0;
	uint64_t m_heap_offset = 0;
	std::vector<uint32_t> m_fat;
	std::vector<Node> m_nodes;
	std::unordered_map<std::string, uint64_t> m_paths;
};

} // namespace

std::shared_ptr<ArchiveReader> OpenFFPFSCArchive(const std::filesystem::path& path) {
	return FFPFSCReader::Open(path);
}

} // namespace Common
