// @std/compress (gzip, zlib and raw deflate) and @std/zip (zip archives),
// on miniz. Everything is in memory: an archive is read whole, a new one
// is built whole and then written; files go through the runtime's own
// UTF-8 paths. Text and buffers are both accepted; what comes out is a
// string of the bytes.

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <miniz.h>

#include <algorithm>
#include <ctime>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kArchiveMeta = "cluau.Archive";
		constexpr const char* kWriterMeta = "cluau.ZipWriter";

		std::string_view Bytes(lua_State* L, int index)
		{
			if (lua_type(L, index) == LUA_TBUFFER)
			{
				size_t len = 0;
				void* data = lua_tobuffer(L, index, &len);
				return std::string_view(static_cast<const char*>(data), len);
			}
			size_t len = 0;
			const char* s = luaL_checklstring(L, index, &len);
			return std::string_view(s, len);
		}

		int Level(lua_State* L, int index, const char* fn)
		{
			if (lua_isnoneornil(L, index))
				return 6;
			luaL_checktype(L, index, LUA_TTABLE);
			lua_getfield(L, index, "level");
			const double given = lua_isnil(L, -1) ? 6 : (lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1);
			lua_pop(L, 1);
			// The range checked before converting (a number out of int's
			// range does not convert).
			if (!(given >= 0 && given <= 9) || given != std::floor(given))
				luaL_error(L, "%s: options.level is from 0 (stored) to 9 (smallest)", fn);
			return (int)given;
		}

		bool RawOption(lua_State* L, int index)
		{
			if (lua_isnoneornil(L, index))
				return false;
			luaL_checktype(L, index, LUA_TTABLE);
			lua_getfield(L, index, "raw");
			const bool raw = lua_toboolean(L, -1);
			lua_pop(L, 1);
			return raw;
		}

		// Deflate's output for `data`, zlib-wrapped or raw.
		bool Deflate(std::string_view data, int level, bool zlib, std::string& out)
		{
			mz_uint flags = tdefl_create_comp_flags_from_zip_params(level, zlib ? MZ_DEFAULT_WINDOW_BITS : -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
			size_t size = 0;
			void* compressed = tdefl_compress_mem_to_heap(data.data(), data.size(), &size, (int)flags);
			if (!compressed)
				return false;
			out.assign(static_cast<const char*>(compressed), size);
			mz_free(compressed);
			return true;
		}

		// Inflates a raw or zlib stream; `used` is how much of `data` it took.
		bool Inflate(std::string_view data, bool zlib, std::string& out, size_t* used = nullptr)
		{
			tinfl_decompressor inflator;
			tinfl_init(&inflator);
			out.clear();
			std::vector<unsigned char> window(TINFL_LZ_DICT_SIZE);
			size_t in = 0, at = 0;
			// Always "more input may follow": told it has the whole input, miniz
			// pads a stream cut short with zeros, which can decode into output
			// without end. A stream that asks for more than there is is cut short.
			const mz_uint32 flags = (zlib ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0) | TINFL_FLAG_HAS_MORE_INPUT;
			for (;;)
			{
				size_t inSize = data.size() - in;
				size_t outSize = TINFL_LZ_DICT_SIZE - at;
				const tinfl_status status = tinfl_decompress(&inflator, reinterpret_cast<const mz_uint8*>(data.data()) + in, &inSize, window.data(),
					window.data() + at, &outSize, flags);
				in += inSize;
				out.append(reinterpret_cast<const char*>(window.data()) + at, outSize);
				at = (at + outSize) & (TINFL_LZ_DICT_SIZE - 1);
				// Deflate expands at most about 1032 to 1: more is damaged data
				// that miniz decodes without end (a code table zlib refuses).
				if (out.size() > data.size() * 1040 + 65536)
					return false;
				if (status == TINFL_STATUS_DONE)
				{
					if (used)
						*used = in;
					return true;
				}
				if (status < 0 || (status == TINFL_STATUS_NEEDS_MORE_INPUT && in >= data.size()) || (inSize == 0 && outSize == 0))
					return false;
			}
		}

		// ---- @std/compress -------------------------------------------------

		int CompressDeflate(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			std::string out;
			if (!Deflate(data, Level(L, 2, "compress.deflate"), !RawOption(L, 2), out))
				luaL_error(L, "compress.deflate: the compression failed");
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		int CompressInflate(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			std::string out;
			if (!Inflate(data, !RawOption(L, 2), out))
				luaL_error(L, "compress.inflate: the data is not %s", RawOption(L, 2) ? "raw deflate" : "zlib (deflate with its header)");
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		void PutLE32(std::string& out, mz_uint32 v)
		{
			for (int i = 0; i < 4; i++)
				out += (char)((v >> (8 * i)) & 0xFF);
		}

		int CompressGzip(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			std::string body;
			const int level = Level(L, 2, "compress.gzip");
			if (!Deflate(data, level, false, body))
				luaL_error(L, "compress.gzip: the compression failed");
			std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00", 8);
			out += (char)(level >= 9 ? 2 : level <= 1 ? 4 : 0); // extra flags: the effort
			out += (char)255;                                    // the system: unknown
			out += body;
			PutLE32(out, (mz_uint32)mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size()));
			PutLE32(out, (mz_uint32)data.size());
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		mz_uint32 GetLE32(std::string_view s, size_t at)
		{
			mz_uint32 v = 0;
			for (int i = 3; i >= 0; i--)
				v = (v << 8) | (unsigned char)s[at + i];
			return v;
		}

		// gunzip(data): every member's contents, one after another (a file
		// made by appending gzip files is one too).
		int CompressGunzip(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			std::string out;
			size_t at = 0;
			do
			{
				if (data.size() - at < 18 || (unsigned char)data[at] != 0x1f || (unsigned char)data[at + 1] != 0x8b || data[at + 2] != 8)
					luaL_error(L, "compress.gunzip: the data is not gzip");
				const unsigned char flags = (unsigned char)data[at + 3];
				// The reserved flags must be clear (RFC 1952: an encrypted
				// file's, among others).
				if (flags & 0xE0)
					luaL_error(L, "compress.gunzip: the gzip header has flags this reader does not know");
				size_t p = at + 10;
				auto skipZero = [&] {
					while (p < data.size() && data[p] != 0)
						p++;
					p++;
				};
				if (flags & 4) // FEXTRA
				{
					if (p + 2 > data.size())
						luaL_error(L, "compress.gunzip: the gzip header is cut short");
					p += 2 + ((unsigned char)data[p] | ((unsigned char)data[p + 1] << 8));
				}
				if (flags & 8) // FNAME
					skipZero();
				if (flags & 16) // FCOMMENT
					skipZero();
				if (flags & 2) // FHCRC: the low half of the header's CRC-32
				{
					if (p + 2 > data.size())
						luaL_error(L, "compress.gunzip: the gzip header is cut short");
					const mz_uint32 crc = (mz_uint32)mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()) + at, p - at);
					if ((crc & 0xFFFF) != ((unsigned char)data[p] | ((unsigned)(unsigned char)data[p + 1] << 8)))
						luaL_error(L, "compress.gunzip: the gzip header's checksum does not match: the data is damaged");
					p += 2;
				}
				if (p >= data.size())
					luaL_error(L, "compress.gunzip: the gzip header is cut short");
				std::string member;
				size_t used = 0;
				if (!Inflate(data.substr(p), false, member, &used))
					luaL_error(L, "compress.gunzip: the compressed data is damaged");
				p += used;
				if (p + 8 > data.size())
					luaL_error(L, "compress.gunzip: the data ends before its checksum");
				const mz_uint32 crc = GetLE32(data, p), size = GetLE32(data, p + 4);
				if (crc != (mz_uint32)mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(member.data()), member.size()) ||
					size != (mz_uint32)member.size())
					luaL_error(L, "compress.gunzip: the checksum does not match: the data is damaged");
				out += member;
				at = p + 8;
				// Zero bytes after the last member are padding (tape blocks):
				// gzip itself passes over them.
				if (data.find_first_not_of('\0', at) == std::string_view::npos)
					break;
			} while (at < data.size());
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		// ---- @std/zip: reading -------------------------------------------

		// A path argument (a NUL byte in it would cut it short).
		std::string PathArg(lua_State* L, int index, const char* fn)
		{
			size_t len = 0;
			const char* s = luaL_checklstring(L, index, &len);
			std::string path(s, len);
			if (path.find('\0') != std::string::npos)
				luaL_error(L, "%s: the path holds a NUL byte", fn);
			return path;
		}

		bool ReadFile(const std::string& path, std::string& out)
		{
			std::error_code ec;
			if (fs::is_directory(fs::u8path(path), ec))
				return false;
			std::ifstream in(fs::u8path(path), std::ios::binary);
			if (!in)
				return false;
			std::stringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return !in.bad();
		}

		bool WriteFile(const fs::path& path, std::string_view data)
		{
			std::ofstream out(path, std::ios::binary);
			out.write(data.data(), (std::streamsize)data.size());
			return (bool)out;
		}

		// Deflate expands at most about 1032 to 1: an archive's entries may
		// claim this much from each byte, with a little over for small ones.
		constexpr double kMostExpansion = 1040;
		constexpr double kExpansionSlack = 65536;

		struct Archive
		{
			// The whole archive, which miniz reads from: in the archive's own
			// userdata (so the collector counts it), or in Owned when it is
			// too large for one.
			const char* Bytes = nullptr;
			size_t Size = 0;
			std::string Owned;
			mz_zip_archive Zip;
			bool Open = false;
			std::vector<std::string> Names;                     // each entry's whole name
			std::unordered_map<std::string_view, mz_uint> Find; // a name's entry (the last of that name)
			~Archive()
			{
				if (Open)
					mz_zip_reader_end(&Zip);
			}
		};

		// The archive's userdata: the box, then (most often) the bytes.
		struct ArchiveBox
		{
			Archive* Of = nullptr;
		};

		// The largest archive kept in its userdata (Luau's limit is just
		// under 2 GB).
		constexpr size_t kMostInline = (size_t)1 << 30;

		Archive& CheckArchive(lua_State* L, const char* fn)
		{
			ArchiveBox* box = static_cast<ArchiveBox*>(luaL_checkudata(L, 1, kArchiveMeta));
			if (!box->Of || !box->Of->Open)
				luaL_error(L, "%s: the archive is closed", fn);
			return *box->Of;
		}

		// miniz reads the archive through this (its own reader of memory
		// forms pointers past the end of a damaged archive's bytes).
		size_t ReadArchive(void* opaque, mz_uint64 offset, void* to, size_t n)
		{
			const Archive* a = static_cast<const Archive*>(opaque);
			if (offset >= a->Size)
				return 0;
			const size_t count = (size_t)std::min<mz_uint64>(n, a->Size - offset);
			std::memcpy(to, a->Bytes + offset, count);
			return count;
		}

		mz_uint32 GetLE16(const char* p) { return (mz_uint32)(unsigned char)p[0] | ((mz_uint32)(unsigned char)p[1] << 8); }

		// Pushes a new, empty archive userdata, with room for `size` bytes
		// of the archive in it when they fit; the bytes go to `Bytes`.
		Archive& NewArchive(lua_State* L, size_t size)
		{
			const bool inline_ = size <= kMostInline;
			ArchiveBox* box = static_cast<ArchiveBox*>(lua_newuserdatadtor(L, sizeof(ArchiveBox) + (inline_ ? size : 0), [](lua_State*, void* p) {
				delete static_cast<ArchiveBox*>(p)->Of;
			}));
			box->Of = new Archive();
			std::memset(&box->Of->Zip, 0, sizeof(box->Of->Zip));
			box->Of->Size = size;
			if (inline_)
				box->Of->Bytes = reinterpret_cast<const char*>(box + 1);
			else
			{
				box->Of->Owned.resize(size);
				box->Of->Bytes = box->Of->Owned.data();
			}
			return *box->Of;
		}

		// Opens the archive at the top of the stack (its bytes in place) and
		// checks how its entries lie. miniz trusts the central directory: an
		// entry there whose local header names another file, or several that
		// share one file's data, could make a small archive give out far
		// more than it holds, so either is refused; and what the entries
		// claim to hold together must fit the archive's size.
		int OpenArchive(lua_State* L, Archive& a, const char* fn, const std::string& what)
		{
			a.Zip.m_pRead = ReadArchive;
			a.Zip.m_pIO_opaque = &a;
			if (!mz_zip_reader_init(&a.Zip, a.Size, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))
				luaL_error(L, "%s: %s is not a zip archive (%s)", fn, what.c_str(), mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
			a.Open = true;
			const mz_uint n = mz_zip_reader_get_num_files(&a.Zip);
			a.Names.resize(n);
			std::vector<std::pair<mz_uint64, mz_uint64>> spans; // [local header, end of data)
			spans.reserve(n);
			double claimed = 0;
			for (mz_uint i = 0; i < n; i++)
			{
				mz_zip_archive_file_stat stat;
				if (!mz_zip_reader_file_stat(&a.Zip, i, &stat))
					luaL_error(L, "%s: %s is damaged: entry %d cannot be read (%s)", fn, what.c_str(), (int)i + 1,
						mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
				std::string& name = a.Names[i];
				name.resize(mz_zip_reader_get_filename(&a.Zip, i, nullptr, 0) - 1);
				mz_zip_reader_get_filename(&a.Zip, i, name.data(), (mz_uint)name.size() + 1);
				const mz_uint64 header = stat.m_local_header_ofs;
				if (header > a.Size || a.Size - header < 30 || std::memcmp(a.Bytes + header, "PK\x03\x04", 4) != 0)
					luaL_error(L, "%s: %s is damaged: '%s' has no local header", fn, what.c_str(), name.c_str());
				const mz_uint32 nameLength = GetLE16(a.Bytes + header + 26), extraLength = GetLE16(a.Bytes + header + 28);
				const mz_uint64 data = header + 30 + nameLength + extraLength;
				if (data > a.Size || stat.m_comp_size > a.Size - data)
					luaL_error(L, "%s: %s is damaged: '%s' ends past the archive's end", fn, what.c_str(), name.c_str());
				if (nameLength != name.size() || std::memcmp(a.Bytes + header + 30, name.data(), name.size()) != 0)
					luaL_error(L, "%s: %s is damaged: '%s' is another name in its local header", fn, what.c_str(), name.c_str());
				spans.emplace_back(header, data + stat.m_comp_size);
				claimed += (double)stat.m_uncomp_size;
				a.Find[std::string_view(name)] = i;
			}
			std::sort(spans.begin(), spans.end());
			for (size_t i = 1; i < spans.size(); i++)
				if (spans[i].first < spans[i - 1].second)
					luaL_error(L, "%s: %s is damaged: its entries overlap", fn, what.c_str());
			if (claimed > (double)a.Size * kMostExpansion + kExpansionSlack)
				luaL_error(L, "%s: %s is damaged: its entries claim %.0f bytes from %.0f", fn, what.c_str(), claimed, (double)a.Size);
			luaL_getmetatable(L, kArchiveMeta);
			lua_setmetatable(L, -2);
			return 1;
		}

		int ZipOpen(lua_State* L)
		{
			const std::string path = PathArg(L, 1, "zip.open");
			const fs::path file = fs::u8path(path);
			std::error_code ec;
			const bool regular = fs::is_regular_file(file, ec);
			const uintmax_t size = regular ? fs::file_size(file, ec) : 0;
			if (regular && !ec)
			{
				// Read straight into the archive's userdata.
				std::ifstream in(file, std::ios::binary);
				Archive& a = NewArchive(L, (size_t)size);
				if (!in || !in.read(const_cast<char*>(a.Bytes), (std::streamsize)a.Size) || in.gcount() != (std::streamsize)a.Size)
					luaL_error(L, "zip.open: cannot read '%s'", path.c_str());
				return OpenArchive(L, a, "zip.open", "'" + path + "'");
			}
			std::string data;
			if (!ReadFile(path, data))
				luaL_error(L, "zip.open: cannot read '%s'", path.c_str());
			Archive& a = NewArchive(L, data.size());
			std::memcpy(const_cast<char*>(a.Bytes), data.data(), data.size());
			return OpenArchive(L, a, "zip.open", "'" + path + "'");
		}

		int ZipFromString(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			Archive& a = NewArchive(L, data.size());
			std::memcpy(const_cast<char*>(a.Bytes), data.data(), data.size());
			return OpenArchive(L, a, "zip.fromString", "the data");
		}

		int ArchiveEntries(lua_State* L)
		{
			Archive& a = CheckArchive(L, "archive:entries");
			const mz_uint n = mz_zip_reader_get_num_files(&a.Zip);
			lua_createtable(L, (int)n, 0);
			for (mz_uint i = 0; i < n; i++)
			{
				mz_zip_archive_file_stat stat;
				if (!mz_zip_reader_file_stat(&a.Zip, i, &stat))
					continue;
				lua_createtable(L, 0, 5);
				lua_pushlstring(L, a.Names[i].data(), a.Names[i].size());
				lua_setfield(L, -2, "name");
				lua_pushnumber(L, (double)stat.m_uncomp_size);
				lua_setfield(L, -2, "size");
				lua_pushnumber(L, (double)stat.m_comp_size);
				lua_setfield(L, -2, "compressedSize");
				lua_pushboolean(L, stat.m_is_directory);
				lua_setfield(L, -2, "directory");
				lua_pushnumber(L, (double)stat.m_time);
				lua_setfield(L, -2, "modified");
				lua_rawseti(L, -2, (int)i + 1);
			}
			return 1;
		}

		// An entry's sizes must fit together before it is read: what it claims
		// to hold is allocated whole, and deflate expands at most about 1032
		// to 1 (a stored entry, not at all).
		void CheckSizes(lua_State* L, const std::string& name, const mz_zip_archive_file_stat& stat, const char* fn)
		{
			const bool stored = stat.m_method == 0;
			const double most = (double)stat.m_comp_size * kMostExpansion + kExpansionSlack;
			const bool fits = stored ? stat.m_uncomp_size == stat.m_comp_size : (double)stat.m_uncomp_size <= most;
			if (!fits)
				luaL_error(L, "%s: '%s' claims %.0f bytes from %.0f: the archive is damaged", fn, name.c_str(), (double)stat.m_uncomp_size,
					(double)stat.m_comp_size);
		}

		int ArchiveRead(lua_State* L)
		{
			Archive& a = CheckArchive(L, "archive:read");
			size_t length = 0;
			const char* name = luaL_checklstring(L, 2, &length);
			const auto found = a.Find.find(std::string_view(name, length));
			if (found == a.Find.end())
				luaL_error(L, "archive:read: no '%s' in the archive", name);
			const mz_uint index = found->second;
			mz_zip_archive_file_stat stat;
			if (!mz_zip_reader_file_stat(&a.Zip, (mz_uint)index, &stat))
				luaL_error(L, "archive:read: cannot read '%s': %s", name, mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
			CheckSizes(L, a.Names[index], stat, "archive:read");
			size_t size = 0;
			// Freed however this ends (pushing it can raise, out of memory).
			std::unique_ptr<void, void (*)(void*)> data(mz_zip_reader_extract_to_heap(&a.Zip, (mz_uint)index, &size, 0), mz_free);
			if (!data)
				luaL_error(L, "archive:read: cannot read '%s': %s", name, mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
			lua_pushlstring(L, static_cast<const char*>(data.get()), size);
			return 1;
		}

		// Whether an entry's name stays in the directory it is extracted to:
		// not absolute (nor a drive's), not through "..", no backslash (a
		// separator on Windows) and no NUL byte.
		bool StaysInside(const std::string& name)
		{
			if (name.empty() || name.find('\\') != std::string::npos || name.find('\0') != std::string::npos)
				return false;
			if (name.size() >= 2 && name[1] == ':')
				return false;
			const fs::path relative = fs::u8path(name).lexically_normal();
			return !(relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
				(!relative.empty() && *relative.begin() == ".."));
		}

		// archive:extract(dir): every entry under dir (made when missing);
		// an entry whose name would leave dir (absolute, or through "..") is
		// refused. Returns the number of files written.
		int ArchiveExtract(lua_State* L)
		{
			Archive& a = CheckArchive(L, "archive:extract");
			const fs::path root = fs::u8path(PathArg(L, 2, "archive:extract"));
			std::error_code ec;
			fs::create_directories(root, ec);
			const mz_uint n = mz_zip_reader_get_num_files(&a.Zip);
			int written = 0;
			for (mz_uint i = 0; i < n; i++)
			{
				mz_zip_archive_file_stat stat;
				if (!mz_zip_reader_file_stat(&a.Zip, i, &stat))
					continue;
				const std::string& name = a.Names[i];
				const fs::path relative = fs::u8path(name).lexically_normal();
				if (!StaysInside(name))
					luaL_error(L, "archive:extract: '%s' would be written outside the directory: nothing more extracted", name.c_str());
				const fs::path target = root / relative;
				if (stat.m_is_directory)
				{
					fs::create_directories(target, ec);
					continue;
				}
				fs::create_directories(target.parent_path(), ec);
				CheckSizes(L, name, stat, "archive:extract");
				size_t size = 0;
				std::unique_ptr<void, void (*)(void*)> data(mz_zip_reader_extract_to_heap(&a.Zip, i, &size, 0), mz_free);
				if (!data)
					luaL_error(L, "archive:extract: cannot read '%s': %s", name.c_str(), mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
				const bool ok = WriteFile(target, std::string_view(static_cast<const char*>(data.get()), size));
				data.reset();
				if (!ok)
					luaL_error(L, "archive:extract: cannot write '%s'", name.c_str());
				written++;
			}
			lua_pushinteger(L, written);
			return 1;
		}

		int ArchiveClose(lua_State* L)
		{
			ArchiveBox* box = static_cast<ArchiveBox*>(luaL_checkudata(L, 1, kArchiveMeta));
			delete box->Of;
			box->Of = nullptr;
			return 0;
		}

		// ---- @std/zip: writing -------------------------------------------

		struct Writer
		{
			mz_zip_archive Zip;
			std::string Path; // "" for an archive given back as a string
			bool Open = false;
			std::unordered_set<std::string> Names; // the entries' so far
			~Writer()
			{
				if (Open)
					mz_zip_writer_end(&Zip);
			}
		};

		struct WriterBox
		{
			Writer* Of = nullptr;
		};

		Writer& CheckWriter(lua_State* L, const char* fn)
		{
			WriterBox* box = static_cast<WriterBox*>(luaL_checkudata(L, 1, kWriterMeta));
			if (!box->Of || !box->Of->Open)
				luaL_error(L, "%s: the archive is finished", fn);
			return *box->Of;
		}

		// zip.create(path?): a new archive, written to `path` by finish() (or
		// given back by it as a string, with no path).
		int ZipCreate(lua_State* L)
		{
			auto writer = std::make_unique<Writer>();
			writer->Path = lua_isnoneornil(L, 1) ? std::string() : PathArg(L, 1, "zip.create");
			std::memset(&writer->Zip, 0, sizeof(writer->Zip));
			if (!mz_zip_writer_init_heap(&writer->Zip, 0, 0))
				luaL_error(L, "zip.create: cannot start the archive");
			writer->Open = true;
			WriterBox* box = static_cast<WriterBox*>(lua_newuserdatadtor(L, sizeof(WriterBox), [](lua_State*, void* p) {
				delete static_cast<WriterBox*>(p)->Of;
			}));
			box->Of = writer.release();
			luaL_getmetatable(L, kWriterMeta);
			lua_setmetatable(L, -2);
			return 1;
		}

		// An entry's name, from the argument at `index`.
		std::string NameArg(lua_State* L, int index)
		{
			size_t len = 0;
			const char* s = luaL_checklstring(L, index, &len);
			return std::string(s, len);
		}

		// Adds an entry. Its name must be one extract writes inside its
		// directory, and only one entry has it.
		void Add(lua_State* L, Writer& w, const std::string& name, std::string_view data, int level, const char* fn)
		{
			if (name.empty())
				luaL_error(L, "%s: an entry has a name", fn);
			if (!StaysInside(name))
				luaL_error(L, "%s: '%s' is not a name for an entry (absolute, through '..', or with '\\' or a NUL byte)", fn, name.c_str());
			// Names that extract would write to one file are one name.
			const auto normal = fs::u8path(name).lexically_normal().generic_u8string();
			const std::string key(normal.begin(), normal.end());
			if (w.Names.count(key))
				luaL_error(L, "%s: the archive has an entry '%s' already", fn, name.c_str());
			MZ_TIME_T now = std::time(nullptr);
			if (!mz_zip_writer_add_mem_ex_v2(&w.Zip, name.c_str(), data.data(), data.size(), nullptr, 0, (mz_uint)level, 0, 0, &now, nullptr, 0, nullptr, 0))
				luaL_error(L, "%s: cannot add '%s': %s", fn, name.c_str(), mz_zip_get_error_string(mz_zip_get_last_error(&w.Zip)));
			w.Names.insert(key);
		}

		int WriterAdd(lua_State* L)
		{
			Writer& w = CheckWriter(L, "writer:add");
			const std::string name = NameArg(L, 2);
			const std::string_view data = Bytes(L, 3);
			Add(L, w, name, data, Level(L, 4, "writer:add"), "writer:add");
			lua_settop(L, 1);
			return 1;
		}

		int WriterAddFile(lua_State* L)
		{
			Writer& w = CheckWriter(L, "writer:addFile");
			const std::string name = NameArg(L, 2);
			const std::string path = PathArg(L, 3, "writer:addFile");
			std::string data;
			if (!ReadFile(path, data))
				luaL_error(L, "writer:addFile: cannot read '%s'", path.c_str());
			Add(L, w, name, data, Level(L, 4, "writer:addFile"), "writer:addFile");
			lua_settop(L, 1);
			return 1;
		}

		int WriterAddDirectory(lua_State* L)
		{
			Writer& w = CheckWriter(L, "writer:addDirectory");
			std::string name = NameArg(L, 2);
			if (name.empty() || name.back() != '/')
				name += '/';
			Add(L, w, name, {}, 0, "writer:addDirectory");
			lua_settop(L, 1);
			return 1;
		}

		int WriterFinish(lua_State* L)
		{
			Writer& w = CheckWriter(L, "writer:finish");
			void* data = nullptr;
			size_t size = 0;
			if (!mz_zip_writer_finalize_heap_archive(&w.Zip, &data, &size))
				luaL_error(L, "writer:finish: %s", mz_zip_get_error_string(mz_zip_get_last_error(&w.Zip)));
			const std::string archive(static_cast<const char*>(data), size);
			mz_free(data); // the archive's buffer is the caller's to free
			mz_zip_writer_end(&w.Zip);
			w.Open = false;
			if (w.Path.empty())
			{
				lua_pushlstring(L, archive.data(), archive.size());
				return 1;
			}
			if (!WriteFile(fs::u8path(w.Path), archive))
				luaL_error(L, "writer:finish: cannot write '%s'", w.Path.c_str());
			return 0;
		}

		void Type(lua_State* L, const char* meta, const char* name, const luaL_Reg* methods)
		{
			luaL_newmetatable(L, meta);
			lua_pushstring(L, name);
			lua_setfield(L, -2, "__type");
			lua_newtable(L);
			for (const luaL_Reg* m = methods; m->name; m++)
			{
				lua_pushcfunction(L, m->func, m->name);
				lua_setfield(L, -2, m->name);
			}
			lua_setfield(L, -2, "__index");
			lua_pop(L, 1);
		}

		void Functions(lua_State* L, const luaL_Reg* functions)
		{
			lua_newtable(L);
			for (const luaL_Reg* f = functions; f->name; f++)
			{
				lua_pushcfunction(L, f->func, f->name);
				lua_setfield(L, -2, f->name);
			}
		}

	} // namespace

	void OpenCompress(lua_State* L)
	{
		const luaL_Reg functions[] = { { "gzip", CompressGzip }, { "gunzip", CompressGunzip }, { "deflate", CompressDeflate }, { "inflate", CompressInflate },
			{ nullptr, nullptr } };
		Functions(L, functions);
	}

	void OpenZip(lua_State* L)
	{
		const luaL_Reg archive[] = { { "entries", ArchiveEntries }, { "read", ArchiveRead }, { "extract", ArchiveExtract }, { "close", ArchiveClose },
			{ nullptr, nullptr } };
		const luaL_Reg writer[] = { { "add", WriterAdd }, { "addFile", WriterAddFile }, { "addDirectory", WriterAddDirectory }, { "finish", WriterFinish },
			{ nullptr, nullptr } };
		Type(L, kArchiveMeta, "Archive", archive);
		Type(L, kWriterMeta, "ZipWriter", writer);
		const luaL_Reg functions[] = { { "open", ZipOpen }, { "fromString", ZipFromString }, { "create", ZipCreate }, { nullptr, nullptr } };
		Functions(L, functions);
	}

} // namespace cluau::runtime::detail
