#include "Resolve.h"

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

		bool IsFile(const fs::path& p)
		{
			std::error_code ec;
			return fs::is_regular_file(p, ec);
		}

		bool Candidate(const fs::path& base, fs::path& found)
		{
			const fs::path candidates[] = {
				base.has_extension() ? base : fs::path(),
				fs::path(base).concat(".cluau"),
				fs::path(base).concat(".luau"),
				base / "init.cluau",
				base / "init.luau",
			};
			for (const fs::path& c : candidates)
				if (!c.empty() && IsFile(c))
				{
					found = c;
					return true;
				}
			return false;
		}

	} // namespace

	fs::path ResolveRequire(const std::string& name, const fs::path& fromDir, const fs::path& mainDir, const std::vector<fs::path>& searchDirs,
		std::vector<fs::path>* looked)
	{
		std::vector<fs::path> bases;
		const fs::path asPath = fs::u8path(name);
		if (StartsWith(name, "./") || StartsWith(name, "../") || name == "." || name == "..")
			bases.push_back(fromDir / asPath);
		else if (asPath.is_absolute())
			bases.push_back(asPath);
		else
		{
			bases.push_back(mainDir / asPath);
			// Packages: the nearest cluau_modules directory up from the
			// requiring file (a package's own dependencies sit beside it).
			std::error_code ec;
			for (fs::path dir = fromDir.lexically_normal(); !dir.empty(); dir = dir.parent_path())
			{
				const fs::path modules = dir / "cluau_modules";
				if (fs::is_directory(modules, ec))
					bases.push_back(modules / asPath);
				if (dir == dir.parent_path())
					break;
			}
			for (const fs::path& dir : searchDirs)
				bases.push_back(dir / asPath);
		}
		for (fs::path& base : bases)
		{
			base = base.lexically_normal();
			fs::path found;
			if (Candidate(base, found))
				return found;
		}
		if (looked)
			*looked = std::move(bases);
		return {};
	}

	std::string PathText(const fs::path& path)
	{
		const auto u = path.generic_u8string();
		return std::string(u.begin(), u.end());
	}

	std::string ModuleKey(const fs::path& path)
	{
		std::error_code ec;
		const fs::path canonical = fs::weakly_canonical(path, ec);
		return PathText(ec ? path : canonical);
	}

	bool IsLuauFile(const fs::path& path)
	{
		const fs::path ext = path.extension();
		return ext == ".luau" || ext == ".lua";
	}

} // namespace cluau::runtime::detail
