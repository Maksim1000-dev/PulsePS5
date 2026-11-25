#include "common/archive.h"
#include "common/file.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kBlockSize = 0x10000;
constexpr uint32_t kInodeSize = 0xA8;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FFPFSCArchiveTests: failed: %s\n", message);
		std::abort();
	}
}

void Store16(uint8_t* data, uint16_t value) {
	data[0] = static_cast<uint8_t>(value);
	data[1] = static_cast<uint8_t>(value >> 8u);
}

void Store32(uint8_t* data, uint32_t value) {
	for (unsigned i = 0; i < 4; ++i) {
		data[i] = static_cast<uint8_t>(value >> (i * 8u));
	}
}

void Store64(uint8_t* data, uint64_t value) {
	Store32(data, static_cast<uint32_t>(value));
	Store32(data + 4, static_cast<uint32_t>(value >> 32u));
}

class TemporaryDirectory {
public:
	TemporaryDirectory() {
		m_path = std::filesystem::temp_directory_path() /
		         ("kyty_ffpfsc_test_" +
		          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Check(std::filesystem::create_directories(m_path), "create temporary directory");
	}

	~TemporaryDirectory() {
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}

	const std::filesystem::path& Path() const { return m_path; }

private:
	std::filesystem::path m_path;
};

std::vector<uint8_t> MakeDirectoryEntry(const std::string& name, bool directory,
                                        uint32_t first_cluster, uint64_t size) {
	std::vector<uint8_t> entry(96, 0);
	entry[0] = 0x85;
	entry[1] = 2;
	Store16(entry.data() + 4, directory ? 0x10 : 0x20);

	entry[32] = 0xC0;
	entry[33] = 0x02; // contiguous clusters
	entry[35] = static_cast<uint8_t>(name.size());
	Store32(entry.data() + 32 + 0x14, first_cluster);
	Store64(entry.data() + 32 + 0x18, size);

	entry[64] = 0xC1;
	for (size_t i = 0; i < name.size(); ++i) {
		Store16(entry.data() + 66 + i * 2, static_cast<uint8_t>(name[i]));
	}
	return entry;
}

std::vector<uint8_t> MakeExfatImage(const std::vector<uint8_t>& eboot,
                                    const std::string& param,
                                    const std::vector<uint8_t>& large) {
	constexpr uint32_t sector_size     = 512;
	constexpr uint32_t sector_count    = 160;
	constexpr uint32_t fat_sector      = 24;
	constexpr uint32_t fat_sector_count = 2;
	constexpr uint32_t heap_sector     = 26;
	constexpr uint32_t cluster_count   = 134;

	std::vector<uint8_t> image(static_cast<size_t>(sector_count) * sector_size, 0);
	auto* boot = image.data();
	std::memcpy(boot + 3, "EXFAT   ", 8);
	Store64(boot + 0x48, sector_count);
	Store32(boot + 0x50, fat_sector);
	Store32(boot + 0x54, fat_sector_count);
	Store32(boot + 0x58, heap_sector);
	Store32(boot + 0x5C, cluster_count);
	Store32(boot + 0x60, 2);
	boot[0x6C] = 9;
	boot[0x6D] = 0;
	boot[0x6E] = 1;
	Store16(boot + 0x1FE, 0xAA55);

	const auto fat_offset = static_cast<size_t>(fat_sector) * sector_size;
	for (uint32_t cluster: {2u, 3u, 4u, 8u, 9u, 10u, 100u, 101u, 102u, 103u,
	                        104u, 105u, 106u, 107u}) {
		Store32(image.data() + fat_offset + static_cast<size_t>(cluster) * 4, 0xFFFFFFFFu);
	}

	const auto cluster_offset = [&](uint32_t cluster) {
		return (static_cast<size_t>(heap_sector) + cluster - 2) * sector_size;
	};
	auto* root = image.data() + cluster_offset(2);
	auto sce_sys = MakeDirectoryEntry("sce_sys", true, 3, sector_size);
	auto eboot_entry = MakeDirectoryEntry("eboot.bin", false, 8, eboot.size());
	auto large_entry = MakeDirectoryEntry("large.bin", false, 100, large.size());
	std::copy(sce_sys.begin(), sce_sys.end(), root);
	std::copy(eboot_entry.begin(), eboot_entry.end(), root + sce_sys.size());
	std::copy(large_entry.begin(), large_entry.end(), root + sce_sys.size() + eboot_entry.size());

	auto* sys_dir = image.data() + cluster_offset(3);
	auto param_entry = MakeDirectoryEntry("param.json", false, 4, param.size());
	std::copy(param_entry.begin(), param_entry.end(), sys_dir);

	std::copy(eboot.begin(), eboot.end(), image.begin() + cluster_offset(8));
	std::copy(param.begin(), param.end(), image.begin() + cluster_offset(4));
	std::copy(large.begin(), large.end(), image.begin() + cluster_offset(100));
	return image;
}

void AddDirent(std::vector<uint8_t>& block, uint32_t inode, uint32_t type,
               const std::string& name) {
	const auto entry_size = (name.size() + 17 + 7) & ~size_t {7};
	const auto offset = block.size();
	block.resize(offset + entry_size, 0);
	Store32(block.data() + offset, inode);
	Store32(block.data() + offset + 4, type);
	Store32(block.data() + offset + 8, static_cast<uint32_t>(name.size()));
	Store32(block.data() + offset + 12, static_cast<uint32_t>(entry_size));
	std::memcpy(block.data() + offset + 16, name.data(), name.size());
}

void AddInode(std::vector<uint8_t>& pfs, uint32_t id, uint16_t mode, uint32_t flags,
              uint64_t size, uint64_t logical_size, uint32_t blocks, int32_t first_block) {
	auto* inode = pfs.data() + kBlockSize + static_cast<size_t>(id) * kInodeSize;
	Store16(inode, mode);
	Store16(inode + 2, 1);
	Store32(inode + 4, flags);
	Store64(inode + 8, size);
	Store64(inode + 0x10, logical_size);
	Store32(inode + 0x60, blocks);
	Store32(inode + 0x64, static_cast<uint32_t>(first_block));
}

std::vector<uint8_t> MakeFFPFSC(const std::vector<uint8_t>& exfat, bool compress) {
	const auto block_count = (exfat.size() + kBlockSize - 1) / kBlockSize;
	std::vector<uint8_t> pfsc(kBlockSize, 0);
	std::vector<uint64_t> offsets(block_count + 1);
	for (size_t block_index = 0; block_index < block_count; ++block_index) {
		std::vector<uint8_t> logical(kBlockSize, 0);
		const auto source_offset = block_index * kBlockSize;
		const auto source_size = std::min<size_t>(kBlockSize, exfat.size() - source_offset);
		std::copy_n(exfat.data() + source_offset, source_size, logical.data());

		std::vector<uint8_t> stored(kBlockSize);
		if (compress) {
			uLongf compressed_size = static_cast<uLongf>(stored.size());
			const auto result = compress2(stored.data(), &compressed_size, logical.data(),
			                              static_cast<uLong>(logical.size()), Z_BEST_COMPRESSION);
			Check(result == Z_OK, "compress PFSC fixture block");
			stored.resize(compressed_size);
		} else {
			stored = std::move(logical);
		}

		offsets[block_index] = pfsc.size();
		pfsc.insert(pfsc.end(), stored.begin(), stored.end());
	}
	offsets.back() = pfsc.size();
	const auto logical_length = block_count * kBlockSize;
	for (size_t i = 0; i < offsets.size(); ++i) {
		Store64(pfsc.data() + 0x400 + i * 8, offsets[i]);
	}

	Store32(pfsc.data(), 0x43534650u);
	Store32(pfsc.data() + 8, 6);
	Store32(pfsc.data() + 0x0C, kBlockSize);
	Store64(pfsc.data() + 0x10, kBlockSize);
	Store64(pfsc.data() + 0x18, 0x400);
	Store64(pfsc.data() + 0x20, kBlockSize);
	Store64(pfsc.data() + 0x28, logical_length);

	const auto payload_blocks = static_cast<uint32_t>((pfsc.size() + kBlockSize - 1) / kBlockSize);
	const auto pfs_blocks = 6 + payload_blocks;
	std::vector<uint8_t> pfs(static_cast<size_t>(pfs_blocks) * kBlockSize, 0);
	Store64(pfs.data(), 2);
	Store64(pfs.data() + 8, 20130315);
	Store16(pfs.data() + 0x1C, 0x8);
	Store32(pfs.data() + 0x20, kBlockSize);
	Store64(pfs.data() + 0x28, 1);
	Store64(pfs.data() + 0x30, 4);
	Store64(pfs.data() + 0x38, pfs_blocks);
	Store64(pfs.data() + 0x40, 1);

	AddInode(pfs, 0, 0x416D, 0x20010, kBlockSize, kBlockSize, 1, 2);
	AddInode(pfs, 1, 0x816D, 0x20010, 8, 8, 1, 3);
	AddInode(pfs, 2, 0x416D, 0x10, kBlockSize, kBlockSize, 1, 5);
	AddInode(pfs, 3, 0x816D, 0x11, pfsc.size(), exfat.size(), payload_blocks, 6);

	std::vector<uint8_t> root;
	AddDirent(root, 1, 2, "flat_path_table");
	AddDirent(root, 2, 3, "uroot");
	std::copy(root.begin(), root.end(), pfs.begin() + 2 * kBlockSize);

	std::vector<uint8_t> user_root;
	AddDirent(user_root, 2, 4, ".");
	AddDirent(user_root, 0, 5, "..");
	AddDirent(user_root, 3, 2, "game.exfat");
	std::copy(user_root.begin(), user_root.end(), pfs.begin() + 5 * kBlockSize);

	std::copy(pfsc.begin(), pfsc.end(), pfs.begin() + 6 * kBlockSize);
	return pfs;
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	std::ofstream output(path, std::ios::binary);
	output.write(reinterpret_cast<const char*>(bytes.data()),
	             static_cast<std::streamsize>(bytes.size()));
	Check(output.good(), "write FFPFSC fixture");
}

void CheckValidImage(const std::filesystem::path& path, bool compressed) {
	std::vector<uint8_t> eboot(1300);
	for (size_t i = 0; i < eboot.size(); ++i) {
		eboot[i] = static_cast<uint8_t>((i * 37 + i / 7) % 251);
	}
	const std::string param = R"({"titleId":"TEST00001"})";
	std::vector<uint8_t> large(4096);
	for (size_t i = 0; i < large.size(); ++i) {
		large[i] = static_cast<uint8_t>((i * 19 + i / 3) % 253);
	}
	const auto image = MakeFFPFSC(MakeExfatImage(eboot, param, large), compressed);
	WriteFile(path, image);

	Check(Common::IsSupportedArchive(path), "recognize FFPFSC by extension");
	const auto root = Common::MakeArchivePath(path);
	Check(Common::File::IsDirectoryExisting(root), "open FFPFSC virtual root");
	Check(Common::File::IsFileExisting(root / "EBOOT.BIN"), "find eboot case-insensitively");

	const auto entries = Common::File::GetDirEntries(root);
	Check(std::ranges::any_of(entries, [](const auto& entry) {
		      return !entry.is_file && entry.name == "sce_sys";
	      }),
	      "enumerate exFAT directory");

	Common::File eboot_file(root / "eboot.bin", Common::File::Mode::Read);
	const auto actual_eboot = eboot_file.ReadWholeBuffer();
	Check(actual_eboot.size() == eboot.size() &&
	          std::memcmp(actual_eboot.data(), eboot.data(), eboot.size()) == 0,
	      "read decompressed eboot.bin");

	Common::File metadata(root / "sce_sys" / "param.json", Common::File::Mode::Read);
	const auto actual_param = metadata.ReadWholeBuffer();
	Check(std::string(reinterpret_cast<const char*>(actual_param.data()), actual_param.size()) == param,
	      "read nested exFAT metadata");

	Common::File cross_block_file(root / "large.bin", Common::File::Mode::Read);
	const auto actual_large = cross_block_file.ReadWholeBuffer();
	Check(actual_large.size() == large.size() &&
	          std::memcmp(actual_large.data(), large.data(), large.size()) == 0,
	      "read a file spanning PFSC block boundaries");
}

void CheckCorruptPfsc(const std::filesystem::path& path) {
	const std::vector<uint8_t> eboot {1, 2, 3, 4};
	const auto image = MakeFFPFSC(MakeExfatImage(eboot, "{}", {}), true);
	auto damaged = image;
	Store64(damaged.data() + 6 * kBlockSize + 0x400, 0xFFFFFFFFFFFFFFFFu);
	WriteFile(path, damaged);
	Check(!Common::OpenArchive(path), "reject PFSC offsets outside the payload");
}

} // namespace

int main() {
	TemporaryDirectory temporary;
	CheckValidImage(temporary.Path() / "compressed.ffpfsc", true);
	CheckValidImage(temporary.Path() / "raw.ffpfsc", false);
	CheckCorruptPfsc(temporary.Path() / "corrupt.ffpfsc");

	std::vector<uint8_t> invalid(0x10000, 0);
	const auto invalid_path = temporary.Path() / "invalid.ffpfsc";
	WriteFile(invalid_path, invalid);
	Check(!Common::OpenArchive(invalid_path), "reject malformed PFS header");
	Check(!Common::IsSupportedArchive("game.fpkg"), "do not classify FPKG as an archive");

	std::puts("FFPFSC archive tests passed");
	return 0;
}
