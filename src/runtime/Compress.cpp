// @std/compress (gzip, zlib and raw deflate) and @std/zip (zip archives),
// on miniz. Everything is in memory: an archive is read whole, a new one
// is built whole and then written; files go through the runtime's own
// UTF-8 paths. Text and buffers are both accepted; what comes out is a
// string of the bytes.

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <miniz.h>

#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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
			const int level = lua_isnil(L, -1) ? 6 : (int)lua_tointeger(L, -1);
			lua_pop(L, 1);
			if (level < 0 || level > 9)
				luaL_error(L, "%s: options.level is from 0 (stored) to 9 (smallest)", fn);
			return level;
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
			if (!compressed && !data.empty())
				return false;
			out.assign(static_cast<const char*>(compressed), size);
			mz_free(compressed);
			if (data.empty() && out.empty())
			{
				// miniz gives nothing for nothing: the empty stream, by hand.
				out = zlib ? std::string("\x78\x9c\x03\x00\x00\x00\x00\x01", 8) : std::string("\x03\x00", 2);
			}
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
				if (flags & 2) // FHCRC
					p += 2;
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
			} while (at < data.size());
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		// ---- @std/zip: reading -------------------------------------------

		bool ReadFile(const std::string& path, std::string& out)
		{
			std::ifstream in(fs::u8path(path), std::ios::binary);
			if (!in)
				return false;
			std::stringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return true;
		}

		bool WriteFile(const fs::path& path, std::string_view data)
		{
			std::ofstream out(path, std::ios::binary);
			out.write(data.data(), (std::streamsize)data.size());
			return (bool)out;
		}

		struct Archive
		{
			std::string Data; // the whole archive (miniz reads from it)
			mz_zip_archive Zip;
			bool Open = false;
			~Archive()
			{
				if (Open)
					mz_zip_reader_end(&Zip);
			}
		};

		struct ArchiveBox
		{
			Archive* Of = nullptr;
		};

		Archive& CheckArchive(lua_State* L, const char* fn)
		{
			ArchiveBox* box = static_cast<ArchiveBox*>(luaL_checkudata(L, 1, kArchiveMeta));
			if (!box->Of || !box->Of->Open)
				luaL_error(L, "%s: the archive is closed", fn);
			return *box->Of;
		}

		int PushArchive(lua_State* L, std::string data, const char* fn, const std::string& what)
		{
			auto archive = std::make_unique<Archive>();
			archive->Data = std::move(data);
			std::memset(&archive->Zip, 0, sizeof(archive->Zip));
			if (!mz_zip_reader_init_mem(&archive->Zip, archive->Data.data(), archive->Data.size(), 0))
				luaL_error(L, "%s: %s is not a zip archive (%s)", fn, what.c_str(), mz_zip_get_error_string(mz_zip_get_last_error(&archive->Zip)));
			archive->Open = true;
			ArchiveBox* box = static_cast<ArchiveBox*>(lua_newuserdatadtor(L, sizeof(ArchiveBox), [](lua_State*, void* p) {
				delete static_cast<ArchiveBox*>(p)->Of;
			}));
			box->Of = archive.release();
			luaL_getmetatable(L, kArchiveMeta);
			lua_setmetatable(L, -2);
			return 1;
		}

		int ZipOpen(lua_State* L)
		{
			const std::string path = luaL_checkstring(L, 1);
			std::string data;
			if (!ReadFile(path, data))
				luaL_error(L, "zip.open: cannot read '%s'", path.c_str());
			return PushArchive(L, std::move(data), "zip.open", "'" + path + "'");
		}

		int ZipFromString(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			return PushArchive(L, std::string(data), "zip.fromString", "the data");
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
				lua_pushstring(L, stat.m_filename);
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
		void CheckSizes(lua_State* L, const mz_zip_archive_file_stat& stat, const char* fn)
		{
			const bool stored = stat.m_method == 0;
			const bool fits = stored ? stat.m_uncomp_size == stat.m_comp_size : stat.m_uncomp_size <= stat.m_comp_size * 1040 + 65536;
			if (!fits)
				luaL_error(L, "%s: '%s' claims %.0f bytes from %.0f: the archive is damaged", fn, stat.m_filename, (double)stat.m_uncomp_size,
					(double)stat.m_comp_size);
		}

		int ArchiveRead(lua_State* L)
		{
			Archive& a = CheckArchive(L, "archive:read");
			const char* name = luaL_checkstring(L, 2);
			const int index = mz_zip_reader_locate_file(&a.Zip, name, nullptr, 0);
			if (index < 0)
				luaL_error(L, "archive:read: no '%s' in the archive", name);
			mz_zip_archive_file_stat stat;
			if (!mz_zip_reader_file_stat(&a.Zip, (mz_uint)index, &stat))
				luaL_error(L, "archive:read: cannot read '%s': %s", name, mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
			CheckSizes(L, stat, "archive:read");
			size_t size = 0;
			void* data = mz_zip_reader_extract_to_heap(&a.Zip, (mz_uint)index, &size, 0);
			if (!data)
				luaL_error(L, "archive:read: cannot read '%s': %s", name, mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
			lua_pushlstring(L, static_cast<const char*>(data), size);
			mz_free(data);
			return 1;
		}

		// archive:extract(dir): every entry under dir (made when missing);
		// an entry whose name would leave dir (absolute, or through "..") is
		// refused. Returns the number of files written.
		int ArchiveExtract(lua_State* L)
		{
			Archive& a = CheckArchive(L, "archive:extract");
			const fs::path root = fs::u8path(luaL_checkstring(L, 2));
			std::error_code ec;
			fs::create_directories(root, ec);
			const mz_uint n = mz_zip_reader_get_num_files(&a.Zip);
			int written = 0;
			for (mz_uint i = 0; i < n; i++)
			{
				mz_zip_archive_file_stat stat;
				if (!mz_zip_reader_file_stat(&a.Zip, i, &stat))
					continue;
				const std::string name = stat.m_filename;
				const fs::path relative = fs::u8path(name).lexically_normal();
				const bool escapes = relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
									 (!relative.empty() && *relative.begin() == "..") || name.find('\\') != std::string::npos;
				if (escapes)
					luaL_error(L, "archive:extract: '%s' would be written outside the directory: nothing more extracted", name.c_str());
				const fs::path target = root / relative;
				if (stat.m_is_directory)
				{
					fs::create_directories(target, ec);
					continue;
				}
				fs::create_directories(target.parent_path(), ec);
				CheckSizes(L, stat, "archive:extract");
				size_t size = 0;
				void* data = mz_zip_reader_extract_to_heap(&a.Zip, i, &size, 0);
				if (!data)
					luaL_error(L, "archive:extract: cannot read '%s': %s", name.c_str(), mz_zip_get_error_string(mz_zip_get_last_error(&a.Zip)));
				const bool ok = WriteFile(target, std::string_view(static_cast<const char*>(data), size));
				mz_free(data);
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
			writer->Path = luaL_optstring(L, 1, "");
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

		void Add(lua_State* L, Writer& w, const char* name, std::string_view data, int level, const char* fn)
		{
			if (!*name)
				luaL_error(L, "%s: an entry has a name", fn);
			MZ_TIME_T now = std::time(nullptr);
			if (!mz_zip_writer_add_mem_ex_v2(&w.Zip, name, data.data(), data.size(), nullptr, 0, (mz_uint)level, 0, 0, &now, nullptr, 0, nullptr, 0))
				luaL_error(L, "%s: cannot add '%s': %s", fn, name, mz_zip_get_error_string(mz_zip_get_last_error(&w.Zip)));
		}

		int WriterAdd(lua_State* L)
		{
			Writer& w = CheckWriter(L, "writer:add");
			const char* name = luaL_checkstring(L, 2);
			const std::string_view data = Bytes(L, 3);
			Add(L, w, name, data, Level(L, 4, "writer:add"), "writer:add");
			lua_settop(L, 1);
			return 1;
		}

		int WriterAddFile(lua_State* L)
		{
			Writer& w = CheckWriter(L, "writer:addFile");
			const char* name = luaL_checkstring(L, 2);
			const std::string path = luaL_checkstring(L, 3);
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
			std::string name = luaL_checkstring(L, 2);
			if (name.empty() || name.back() != '/')
				name += '/';
			Add(L, w, name.c_str(), {}, 0, "writer:addDirectory");
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
