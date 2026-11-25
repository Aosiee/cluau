#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cluau::runtime::detail {

	namespace {

		const Json kNull;
		const std::string kEmpty;

		struct Reader
		{
			std::string_view Text;
			size_t At = 0;
			int Depth = 0;

			void Space()
			{
				while (At < Text.size() && (Text[At] == ' ' || Text[At] == '\t' || Text[At] == '\n' || Text[At] == '\r'))
					At++;
			}

			bool Literal(std::string_view word)
			{
				if (Text.substr(At, word.size()) != word)
					return false;
				At += word.size();
				return true;
			}

			static void Utf8(unsigned code, std::string& out)
			{
				if (code < 0x80)
					out += (char)code;
				else if (code < 0x800)
				{
					out += (char)(0xC0 | (code >> 6));
					out += (char)(0x80 | (code & 0x3F));
				}
				else if (code < 0x10000)
				{
					out += (char)(0xE0 | (code >> 12));
					out += (char)(0x80 | ((code >> 6) & 0x3F));
					out += (char)(0x80 | (code & 0x3F));
				}
				else
				{
					out += (char)(0xF0 | (code >> 18));
					out += (char)(0x80 | ((code >> 12) & 0x3F));
					out += (char)(0x80 | ((code >> 6) & 0x3F));
					out += (char)(0x80 | (code & 0x3F));
				}
			}

			bool Hex4(unsigned& code)
			{
				if (At + 4 > Text.size())
					return false;
				code = 0;
				for (int i = 0; i < 4; i++)
				{
					const char c = Text[At++];
					code <<= 4;
					if (c >= '0' && c <= '9')
						code |= (unsigned)(c - '0');
					else if (c >= 'a' && c <= 'f')
						code |= (unsigned)(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F')
						code |= (unsigned)(c - 'A' + 10);
					else
						return false;
				}
				return true;
			}

			bool String(std::string& out)
			{
				At++; // the opening quote
				while (At < Text.size())
				{
					const char c = Text[At++];
					if (c == '"')
						return true;
					if (c != '\\')
					{
						out += c;
						continue;
					}
					if (At >= Text.size())
						return false;
					switch (Text[At++])
					{
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u':
					{
						unsigned code = 0;
						if (!Hex4(code))
							return false;
						// A surrogate pair: one character.
						if (code >= 0xD800 && code < 0xDC00 && Text.substr(At, 2) == "\\u")
						{
							const size_t back = At;
							At += 2;
							unsigned low = 0;
							if (Hex4(low) && low >= 0xDC00 && low < 0xE000)
								code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
							else
								At = back;
						}
						Utf8(code, out);
						break;
					}
					default: return false;
					}
				}
				return false;
			}

			std::optional<Json> Value()
			{
				Space();
				if (At >= Text.size() || ++Depth > 200)
					return std::nullopt;
				struct Leave
				{
					int& D;
					~Leave() { D--; }
				} leave{ Depth };
				const char c = Text[At];
				if (c == '{')
				{
					At++;
					Json object = Json::Object();
					Space();
					if (At < Text.size() && Text[At] == '}')
					{
						At++;
						return object;
					}
					for (;;)
					{
						Space();
						std::string key;
						if (At >= Text.size() || Text[At] != '"' || !String(key))
							return std::nullopt;
						Space();
						if (At >= Text.size() || Text[At++] != ':')
							return std::nullopt;
						std::optional<Json> value = Value();
						if (!value)
							return std::nullopt;
						object.Set(std::move(key), std::move(*value));
						Space();
						if (At < Text.size() && Text[At] == ',')
						{
							At++;
							continue;
						}
						if (At < Text.size() && Text[At] == '}')
						{
							At++;
							return object;
						}
						return std::nullopt;
					}
				}
				if (c == '[')
				{
					At++;
					Json array = Json::Array();
					Space();
					if (At < Text.size() && Text[At] == ']')
					{
						At++;
						return array;
					}
					for (;;)
					{
						std::optional<Json> value = Value();
						if (!value)
							return std::nullopt;
						array.Push(std::move(*value));
						Space();
						if (At < Text.size() && Text[At] == ',')
						{
							At++;
							continue;
						}
						if (At < Text.size() && Text[At] == ']')
						{
							At++;
							return array;
						}
						return std::nullopt;
					}
				}
				if (c == '"')
				{
					std::string text;
					if (!String(text))
						return std::nullopt;
					return Json(std::move(text));
				}
				if (Literal("true"))
					return Json(true);
				if (Literal("false"))
					return Json(false);
				if (Literal("null"))
					return Json();
				const size_t start = At;
				while (At < Text.size() && std::string_view("+-0123456789.eE").find(Text[At]) != std::string_view::npos)
					At++;
				if (At == start)
					return std::nullopt;
				const std::string number(Text.substr(start, At - start));
				char* end = nullptr;
				const double value = std::strtod(number.c_str(), &end);
				if (end != number.c_str() + number.size())
					return std::nullopt;
				return Json(value);
			}
		};

		void Quote(const std::string& text, std::string& out)
		{
			out += '"';
			for (const char ch : text)
			{
				const unsigned char c = (unsigned char)ch;
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20)
					{
						char buffer[8];
						std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
						out += buffer;
					}
					else
						out += ch;
				}
			}
			out += '"';
		}

	} // namespace

	Json Json::Array()
	{
		Json value;
		value.m_Kind = Kind::Array;
		return value;
	}

	Json Json::Object()
	{
		Json value;
		value.m_Kind = Kind::Object;
		return value;
	}

	std::optional<Json> Json::Parse(std::string_view text)
	{
		Reader reader{ text };
		std::optional<Json> value = reader.Value();
		reader.Space();
		if (!value || reader.At != text.size())
			return std::nullopt;
		return value;
	}

	const std::string& Json::String() const { return m_Kind == Kind::String ? m_String : kEmpty; }

	const Json& Json::operator[](std::string_view key) const
	{
		for (const auto& [name, value] : m_Members)
			if (name == key)
				return value;
		return kNull;
	}

	const Json& Json::operator[](size_t index) const { return index < m_Items.size() ? m_Items[index] : kNull; }

	bool Json::Has(std::string_view key) const
	{
		for (const auto& member : m_Members)
			if (member.first == key)
				return true;
		return false;
	}

	Json& Json::Set(std::string key, Json value)
	{
		m_Kind = Kind::Object;
		for (auto& member : m_Members)
			if (member.first == key)
			{
				member.second = std::move(value);
				return *this;
			}
		m_Members.emplace_back(std::move(key), std::move(value));
		return *this;
	}

	Json& Json::Push(Json value)
	{
		m_Kind = Kind::Array;
		m_Items.push_back(std::move(value));
		return *this;
	}

	std::string Json::Dump() const
	{
		std::string out;
		DumpTo(out);
		return out;
	}

	void Json::DumpTo(std::string& out) const
	{
		switch (m_Kind)
		{
		case Kind::Null: out += "null"; break;
		case Kind::Bool: out += m_Bool ? "true" : "false"; break;
		case Kind::Number:
		{
			if (!std::isfinite(m_Number))
			{
				out += "null";
				break;
			}
			char buffer[32];
			if (m_Number == std::floor(m_Number) && std::fabs(m_Number) < 1e15)
				std::snprintf(buffer, sizeof(buffer), "%.0f", m_Number);
			else
				std::snprintf(buffer, sizeof(buffer), "%.17g", m_Number);
			out += buffer;
			break;
		}
		case Kind::String: Quote(m_String, out); break;
		case Kind::Array:
		{
			out += '[';
			for (size_t i = 0; i < m_Items.size(); i++)
			{
				if (i)
					out += ',';
				m_Items[i].DumpTo(out);
			}
			out += ']';
			break;
		}
		case Kind::Object:
		{
			out += '{';
			for (size_t i = 0; i < m_Members.size(); i++)
			{
				if (i)
					out += ',';
				Quote(m_Members[i].first, out);
				out += ':';
				m_Members[i].second.DumpTo(out);
			}
			out += '}';
			break;
		}
		}
	}

} // namespace cluau::runtime::detail
