#ifndef LEXIGLANCE_CORE_JSON_H
#define LEXIGLANCE_CORE_JSON_H

#include <lexiglance/core/Error.h>

#include <charconv>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lexiglance::json
{

	enum class Type : std::uint8_t
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	struct Member;

	// Immutable DOM node. Nodes live in their Document's arena and reference its in-situ decoded text.
	class Value
	{
	public:
		[[nodiscard]] Type type() const noexcept
		{
			return type_;
		}

		[[nodiscard]] bool isNull() const noexcept
		{
			return type_ == Type::Null;
		}

		[[nodiscard]] bool isBool() const noexcept
		{
			return type_ == Type::Bool;
		}

		[[nodiscard]] bool isNumber() const noexcept
		{
			return type_ == Type::Number;
		}

		[[nodiscard]] bool isString() const noexcept
		{
			return type_ == Type::String;
		}

		[[nodiscard]] bool isArray() const noexcept
		{
			return type_ == Type::Array;
		}

		[[nodiscard]] bool isObject() const noexcept
		{
			return type_ == Type::Object;
		}

		[[nodiscard]] bool             asBool( bool fallback = false ) const noexcept;
		[[nodiscard]] double           asDouble( double fallback = 0.0 ) const noexcept;
		[[nodiscard]] std::int64_t     asInt( std::int64_t fallback = 0 ) const noexcept;
		[[nodiscard]] std::string_view asString( std::string_view fallback = {} ) const noexcept;

		[[nodiscard]] std::span<const Value>  items() const noexcept;
		[[nodiscard]] std::span<const Member> members() const noexcept;
		[[nodiscard]] std::size_t             size() const noexcept;

		[[nodiscard]] const Value* find( std::string_view key ) const noexcept;

		// Missing keys and out of range indices yield a null value.
		[[nodiscard]] const Value& operator[]( std::string_view key ) const noexcept;
		[[nodiscard]] const Value& operator[]( std::size_t index ) const noexcept;

	private:
		friend class Parser;

		union Payload
		{
			double        number;
			const char*   string;
			const Value*  array;
			const Member* object;
		};

		Type          type_    = Type::Null;
		bool          boolean_ = false;
		std::uint32_t size_    = 0;
		Payload       data_{ .number = 0.0 };
	};

	struct Member
	{
		std::string_view key;
		Value            value;
	};

	class Document
	{
	public:
		Document( Document&& other ) noexcept;
		Document& operator=( Document&& other ) noexcept;
		Document( const Document& )            = delete;
		Document& operator=( const Document& ) = delete;
		~Document();

		[[nodiscard]] static Result<Document> parse( std::string text );

		[[nodiscard]] const Value& root() const noexcept;

	private:
		struct Storage;

		explicit Document( std::unique_ptr<Storage> storage ) noexcept;

		std::unique_ptr<Storage> storage_;
	};

	void escape( std::string& out, std::string_view text );

	void serialize( std::string& out, const Value& value );

	[[nodiscard]] std::string serialize( const Value& value );

	// Streaming writer producing compact (or indented) JSON.
	class Writer
	{
	public:
		explicit Writer( bool pretty = false ) :
			pretty_( pretty )
		{
		}

		Writer& beginObject();
		Writer& endObject();
		Writer& beginArray();
		Writer& endArray();
		Writer& key( std::string_view name );
		Writer& value( std::string_view text );
		Writer& value( const char* text );
		Writer& value( bool boolean );
		Writer& value( double number );
		Writer& null();

		// Inserts pre-serialised JSON verbatim.
		Writer& raw( std::string_view json );

		template <std::integral T>
			requires( !std::same_as<T, bool> )
		Writer& value( T number )
		{
			prefix();
			std::array<char, 24> buffer{};
			const auto           result = std::to_chars( buffer.data(), buffer.data() + buffer.size(), number );
			out_.append( buffer.data(), result.ptr );
			return *this;
		}

		Writer& field( std::string_view name, std::string_view text )
		{
			key( name );
			return value( text );
		}

		template <typename T>
			requires( !std::is_convertible_v<const T&, std::string_view> )
		Writer& field( std::string_view name, const T& v )
		{
			key( name );
			return value( v );
		}

		[[nodiscard]] const std::string& str() const noexcept
		{
			return out_;
		}

		[[nodiscard]] std::string take() noexcept
		{
			return std::move( out_ );
		}

	private:
		void prefix();
		void indent();

		std::string       out_;
		std::vector<bool> has_items_;
		bool              after_key_ = false;
		bool              pretty_    = false;
	};

} // namespace lexiglance::json

#endif // LEXIGLANCE_CORE_JSON_H
