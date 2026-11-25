#pragma once

// JSON values for the language server's messages: parsed from text, built
// in code, written back out. Objects keep their keys in the order they were
// set. Reading a missing key or the wrong type gives a null value (or the
// accessor's default), so a message's optional parts need no checks.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cluau::runtime::detail {

	class Json
	{
	public:
		enum class Kind { Null, Bool, Number, String, Array, Object };

		Json() = default;
		Json(std::nullptr_t) {}
		Json(bool value)
			: m_Kind(Kind::Bool)
			, m_Bool(value)
		{
		}
		Json(double value)
			: m_Kind(Kind::Number)
			, m_Number(value)
		{
		}
		Json(int value)
			: Json((double)value)
		{
		}
		Json(size_t value)
			: Json((double)value)
		{
		}
		Json(const char* value)
			: m_Kind(Kind::String)
			, m_String(value)
		{
		}
		Json(std::string value)
			: m_Kind(Kind::String)
			, m_String(std::move(value))
		{
		}

		static Json Array();
		static Json Object();
		// The value `text` holds; nullopt when it is not JSON.
		static std::optional<Json> Parse(std::string_view text);

		Kind Type() const { return m_Kind; }
		bool IsNull() const { return m_Kind == Kind::Null; }
		bool IsString() const { return m_Kind == Kind::String; }
		bool IsNumber() const { return m_Kind == Kind::Number; }
		bool IsArray() const { return m_Kind == Kind::Array; }
		bool IsObject() const { return m_Kind == Kind::Object; }

		bool Bool(bool otherwise = false) const { return m_Kind == Kind::Bool ? m_Bool : otherwise; }
		double Number(double otherwise = 0) const { return m_Kind == Kind::Number ? m_Number : otherwise; }
		int Int(int otherwise = 0) const { return m_Kind == Kind::Number ? (int)m_Number : otherwise; }
		const std::string& String() const;

		// An object's member (null when absent); an array's item.
		const Json& operator[](std::string_view key) const;
		const Json& operator[](size_t index) const;
		bool Has(std::string_view key) const;
		size_t Size() const { return m_Kind == Kind::Array ? m_Items.size() : m_Kind == Kind::Object ? m_Members.size() : 0; }
		const std::vector<Json>& Items() const { return m_Items; }
		const std::vector<std::pair<std::string, Json>>& Members() const { return m_Members; }

		// An object's member set (replaced when present); returns *this.
		Json& Set(std::string key, Json value);
		// An array's item appended; returns *this.
		Json& Push(Json value);

		std::string Dump() const;

	private:
		void DumpTo(std::string& out) const;

		Kind m_Kind = Kind::Null;
		bool m_Bool = false;
		double m_Number = 0;
		std::string m_String;
		std::vector<Json> m_Items;
		std::vector<std::pair<std::string, Json>> m_Members;
	};

} // namespace cluau::runtime::detail
