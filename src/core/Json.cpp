#include <lexiglance/core/Json.h>
#include <lexiglance/core/Utf8.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory_resource>
#ifndef __cpp_lib_to_chars
	#include <locale>
	#include <sstream>
#endif

namespace lexiglance::json
{

	namespace
	{

		const Value null_value{};

		constexpr int max_depth = 512;

		constexpr bool isDigit( char c ) noexcept
		{
			return c >= '0' && c <= '9';
		}

		constexpr int hexValue( char c ) noexcept
		{
			if ( c >= '0' && c <= '9' )
			{
				return c - '0';
			}
			if ( c >= 'a' && c <= 'f' )
			{
				return c - 'a' + 10;
			}
			if ( c >= 'A' && c <= 'F' )
			{
				return c - 'A' + 10;
			}
			return -1;
		}

	} // namespace

	// ---------------------------------------------------------------------------------------------------------------------
	// Value
	// ---------------------------------------------------------------------------------------------------------------------

	bool Value::asBool( bool fallback ) const noexcept
	{
		return type_ == Type::Bool ? boolean_ : fallback;
	}

	double Value::asDouble( double fallback ) const noexcept
	{
		return type_ == Type::Number ? data_.number : fallback;
	}

	std::int64_t Value::asInt( std::int64_t fallback ) const noexcept
	{
		if ( type_ != Type::Number )
		{
			return fallback;
		}
		constexpr double limit = 9.2e18;
		const double     n     = data_.number;
		if ( std::isnan( n ) || n <= -limit || n >= limit )
		{
			return fallback;
		}
		return static_cast<std::int64_t>( n );
	}

	std::string_view Value::asString( std::string_view fallback ) const noexcept
	{
		return type_ == Type::String ? std::string_view( data_.string, size_ ) : fallback;
	}

	std::span<const Value> Value::items() const noexcept
	{
		return type_ == Type::Array ? std::span<const Value>( data_.array, size_ ) : std::span<const Value>{};
	}

	std::span<const Member> Value::members() const noexcept
	{
		return type_ == Type::Object ? std::span<const Member>( data_.object, size_ ) : std::span<const Member>{};
	}

	std::size_t Value::size() const noexcept
	{
		return type_ == Type::Array || type_ == Type::Object ? size_ : 0;
	}

	const Value* Value::find( std::string_view key ) const noexcept
	{
		for ( const Member& member : members() )
		{
			if ( member.key == key )
			{
				return &member.value;
			}
		}
		return nullptr;
	}

	const Value& Value::operator[]( std::string_view key ) const noexcept
	{
		const Value* found = find( key );
		return found != nullptr ? *found : null_value;
	}

	const Value& Value::operator[]( std::size_t index ) const noexcept
	{
		const auto values = items();
		return index < values.size() ? values[index] : null_value;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Parser
	// ---------------------------------------------------------------------------------------------------------------------

	// Recursive descent parser decoding strings in place. Relies on the NUL terminator std::string guarantees after the
	// last character, which acts as a sentinel and removes most bounds checks.
	class Parser
	{
	public:
		Parser( std::string& text, std::pmr::memory_resource& arena ) noexcept :
			begin_( text.data() ),
			cursor_( text.data() ),
			end_( text.data() + text.size() ),
			arena_( &arena )
		{
		}

		Result<Value> run()
		{
			Value root;
			skipWhitespace();
			if ( !parseValue( root, 0 ) )
			{
				return error();
			}
			skipWhitespace();
			if ( cursor_ != end_ )
			{
				setError( "unexpected trailing characters" );
				return error();
			}
			return root;
		}

	private:
		bool setError( const char* message ) noexcept
		{
			if ( error_ == nullptr )
			{
				error_        = message;
				error_offset_ = static_cast<std::size_t>( cursor_ - begin_ );
			}
			return false;
		}

		[[nodiscard]] std::unexpected<Error> error() const
		{
			std::size_t line   = 1;
			std::size_t column = 1;
			for ( const char* p = begin_; p < begin_ + error_offset_; ++p )
			{
				if ( *p == '\n' )
				{
					++line;
					column = 1;
				}
				else
				{
					++column;
				}
			}
			return fail( "{} at line {}, column {}", error_ != nullptr ? error_ : "invalid JSON", line, column );
		}

		void skipWhitespace() noexcept
		{
			while ( cursor_ < end_ && ( *cursor_ == ' ' || *cursor_ == '\n' || *cursor_ == '\r' || *cursor_ == '\t' ) )
			{
				++cursor_;
			}
		}

		bool parseValue( Value& out, int depth )
		{
			switch ( *cursor_ )
			{
				case '{':
					return parseObject( out, depth );
				case '[':
					return parseArray( out, depth );
				case '"':
				{
					std::string_view text;
					if ( !parseString( text ) )
					{
						return false;
					}
					out.type_        = Type::String;
					out.size_        = static_cast<std::uint32_t>( text.size() );
					out.data_.string = text.data();
					return true;
				}
				case 't':
					out.type_    = Type::Bool;
					out.boolean_ = true;
					return literal( "true" );
				case 'f':
					out.type_    = Type::Bool;
					out.boolean_ = false;
					return literal( "false" );
				case 'n':
					out.type_ = Type::Null;
					return literal( "null" );
				default:
					if ( *cursor_ == '-' || isDigit( *cursor_ ) )
					{
						return parseNumber( out );
					}
					return setError( cursor_ == end_ ? "unexpected end of input" : "unexpected character" );
			}
		}

		bool literal( std::string_view word ) noexcept
		{
			if ( std::string_view( cursor_, static_cast<std::size_t>( end_ - cursor_ ) ).starts_with( word ) )
			{
				cursor_ += word.size();
				return true;
			}
			return setError( "invalid literal" );
		}

		bool parseNumber( Value& out ) noexcept
		{
			const char* start = cursor_;
			if ( *cursor_ == '-' )
			{
				++cursor_;
			}
			if ( *cursor_ == '0' )
			{
				++cursor_;
			}
			else if ( isDigit( *cursor_ ) )
			{
				while ( isDigit( *cursor_ ) )
				{
					++cursor_;
				}
			}
			else
			{
				return setError( "invalid number" );
			}

			bool integral = true;
			if ( *cursor_ == '.' )
			{
				integral = false;
				++cursor_;
				if ( !isDigit( *cursor_ ) )
				{
					return setError( "invalid number" );
				}
				while ( isDigit( *cursor_ ) )
				{
					++cursor_;
				}
			}
			if ( *cursor_ == 'e' || *cursor_ == 'E' )
			{
				integral = false;
				++cursor_;
				if ( *cursor_ == '+' || *cursor_ == '-' )
				{
					++cursor_;
				}
				if ( !isDigit( *cursor_ ) )
				{
					return setError( "invalid number" );
				}
				while ( isDigit( *cursor_ ) )
				{
					++cursor_;
				}
			}

			out.type_ = Type::Number;

			// Fast path: integers below 2^53 convert exactly.
			const bool negative = *start == '-';
			const auto digits   = cursor_ - start - ( negative ? 1 : 0 );
			if ( integral && digits <= 15 )
			{
				std::int64_t value = 0;
				for ( const char* p = start + ( negative ? 1 : 0 ); p < cursor_; ++p )
				{
					value = ( value * 10 ) + ( *p - '0' );
				}
				out.data_.number = static_cast<double>( negative ? -value : value );
				return true;
			}

#ifdef __cpp_lib_to_chars
			double     value  = 0.0;
			const auto result = std::from_chars( start, cursor_, value );
			if ( result.ec != std::errc{} && result.ec != std::errc::result_out_of_range )
			{
				return setError( "invalid number" );
			}
#else
			// Apple's libc++ has no floating-point from_chars. The number's syntax is checked above, and the classic
			// locale keeps '.' the decimal point.
			std::istringstream stream( std::string( start, static_cast<std::size_t>( cursor_ - start ) ) );
			stream.imbue( std::locale::classic() );
			double value = 0.0;
			stream >> value;
#endif
			out.data_.number = value;
			return true;
		}

		bool parseString( std::string_view& out ) noexcept
		{
			char* const start = ++cursor_;

			// Fast path: no escapes, the string is referenced where it is.
			while ( true )
			{
				const char c = *cursor_;
				if ( c == '"' )
				{
					out = { start, static_cast<std::size_t>( cursor_ - start ) };
					++cursor_;
					return true;
				}
				if ( c == '\\' )
				{
					break;
				}
				if ( static_cast<unsigned char>( c ) < 0x20 )
				{
					return setError( cursor_ >= end_ ? "unterminated string" : "control character in string" );
				}
				++cursor_;
			}

			// Slow path: decode escapes in place. The output never outgrows the input.
			char* write = cursor_;
			while ( true )
			{
				const char c = *cursor_;
				if ( c == '"' )
				{
					out = { start, static_cast<std::size_t>( write - start ) };
					++cursor_;
					return true;
				}
				if ( static_cast<unsigned char>( c ) < 0x20 )
				{
					return setError( cursor_ >= end_ ? "unterminated string" : "control character in string" );
				}
				if ( c != '\\' )
				{
					*write++ = c;
					++cursor_;
					continue;
				}

				const char escaped = cursor_[1];
				cursor_ += 2;
				switch ( escaped )
				{
					case '"':
					case '\\':
					case '/':
						*write++ = escaped;
						break;
					case 'b':
						*write++ = '\b';
						break;
					case 'f':
						*write++ = '\f';
						break;
					case 'n':
						*write++ = '\n';
						break;
					case 'r':
						*write++ = '\r';
						break;
					case 't':
						*write++ = '\t';
						break;
					case 'u':
					{
						char32_t cp = 0;
						if ( !parseHex4( cp ) )
						{
							return false;
						}
						if ( cp >= 0xD800 && cp <= 0xDBFF && cursor_[0] == '\\' && cursor_[1] == 'u' )
						{
							char32_t low  = 0;
							char*    save = cursor_;
							cursor_ += 2;
							if ( parseHex4( low ) && low >= 0xDC00 && low <= 0xDFFF )
							{
								cp = 0x10000 + ( ( cp - 0xD800 ) << 10U ) + ( low - 0xDC00 );
							}
							else
							{
								cursor_ = save;
								error_  = nullptr;
							}
						}
						utf8::EncodeBuffer buffer{};
						const std::size_t  n = utf8::encode( cp, buffer );
						std::copy_n( buffer.data(), n, write );
						write += n;
						break;
					}
					default:
						cursor_ -= 2;
						return setError( "invalid escape sequence" );
				}
			}
		}

		bool parseHex4( char32_t& out ) noexcept
		{
			char32_t value = 0;
			for ( int i = 0; i < 4; ++i )
			{
				const int digit = hexValue( *cursor_ );
				if ( digit < 0 )
				{
					return setError( "invalid unicode escape" );
				}
				value = ( value << 4U ) | static_cast<char32_t>( digit );
				++cursor_;
			}
			out = value;
			return true;
		}

		bool parseArray( Value& out, int depth )
		{
			if ( depth >= max_depth )
			{
				return setError( "nesting too deep" );
			}
			++cursor_;
			skipWhitespace();

			const std::size_t base = values_.size();
			if ( *cursor_ != ']' )
			{
				while ( true )
				{
					Value item;
					if ( !parseValue( item, depth + 1 ) )
					{
						return false;
					}
					values_.push_back( item );

					skipWhitespace();
					if ( *cursor_ == ',' )
					{
						++cursor_;
						skipWhitespace();
						continue;
					}
					if ( *cursor_ == ']' )
					{
						break;
					}
					return setError( "expected ',' or ']'" );
				}
			}
			++cursor_;

			const std::size_t count = values_.size() - base;
			out.type_               = Type::Array;
			out.size_               = static_cast<std::uint32_t>( count );
			out.data_.array         = commit( values_, base );
			return true;
		}

		bool parseObject( Value& out, int depth )
		{
			if ( depth >= max_depth )
			{
				return setError( "nesting too deep" );
			}
			++cursor_;
			skipWhitespace();

			const std::size_t base = members_.size();
			if ( *cursor_ != '}' )
			{
				while ( true )
				{
					if ( *cursor_ != '"' )
					{
						return setError( "expected object key" );
					}
					Member member;
					if ( !parseString( member.key ) )
					{
						return false;
					}
					skipWhitespace();
					if ( *cursor_ != ':' )
					{
						return setError( "expected ':'" );
					}
					++cursor_;
					skipWhitespace();
					if ( !parseValue( member.value, depth + 1 ) )
					{
						return false;
					}
					members_.push_back( member );

					skipWhitespace();
					if ( *cursor_ == ',' )
					{
						++cursor_;
						skipWhitespace();
						continue;
					}
					if ( *cursor_ == '}' )
					{
						break;
					}
					return setError( "expected ',' or '}'" );
				}
			}
			++cursor_;

			const std::size_t count = members_.size() - base;
			out.type_               = Type::Object;
			out.size_               = static_cast<std::uint32_t>( count );
			out.data_.object        = commit( members_, base );
			return true;
		}

		// Moves the elements above `base` of a scratch stack into the arena.
		template <typename T>
		const T* commit( std::vector<T>& stack, std::size_t base )
		{
			const std::size_t count = stack.size() - base;
			if ( count == 0 )
			{
				return nullptr;
			}
			auto* block = static_cast<T*>( arena_->allocate( count * sizeof( T ), alignof( T ) ) );
			std::uninitialized_copy( stack.begin() + static_cast<std::ptrdiff_t>( base ), stack.end(), block );
			stack.resize( base );
			return block;
		}

		char*                      begin_;
		char*                      cursor_;
		char*                      end_;
		std::pmr::memory_resource* arena_;
		std::vector<Value>         values_;
		std::vector<Member>        members_;
		const char*                error_        = nullptr;
		std::size_t                error_offset_ = 0;
	};

	// ---------------------------------------------------------------------------------------------------------------------
	// Document
	// ---------------------------------------------------------------------------------------------------------------------

	struct Document::Storage
	{
		explicit Storage( std::string input ) :
			text( std::move( input ) ),
			arena( std::max<std::size_t>( 4096, text.size() / 2 ) )
		{
		}

		std::string                         text;
		std::pmr::monotonic_buffer_resource arena;
		Value                               root;
	};

	Document::Document( std::unique_ptr<Storage> storage ) noexcept :
		storage_( std::move( storage ) )
	{
	}

	Document::Document( Document&& other ) noexcept = default;

	Document& Document::operator=( Document&& other ) noexcept = default;

	Document::~Document() = default;

	Result<Document> Document::parse( std::string text )
	{
		auto   storage = std::make_unique<Storage>( std::move( text ) );
		Parser parser( storage->text, storage->arena );
		auto   root = parser.run();
		if ( !root )
		{
			return std::unexpected( root.error() );
		}
		storage->root = *root;
		return Document( std::move( storage ) );
	}

	const Value& Document::root() const noexcept
	{
		return storage_ ? storage_->root : null_value;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Serialisation
	// ---------------------------------------------------------------------------------------------------------------------

	void escape( std::string& out, std::string_view text )
	{
		static constexpr std::array<char, 16> hex{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

		out.push_back( '"' );
		std::size_t run = 0;
		for ( std::size_t i = 0; i < text.size(); ++i )
		{
			const auto c = static_cast<unsigned char>( text[i] );
			if ( c >= 0x20 && c != '"' && c != '\\' )
			{
				continue;
			}
			out.append( text.substr( run, i - run ) );
			run = i + 1;
			switch ( c )
			{
				case '"':
					out.append( "\\\"" );
					break;
				case '\\':
					out.append( "\\\\" );
					break;
				case '\n':
					out.append( "\\n" );
					break;
				case '\r':
					out.append( "\\r" );
					break;
				case '\t':
					out.append( "\\t" );
					break;
				default:
					out.append( "\\u00" );
					out.push_back( hex[c >> 4U] );
					out.push_back( hex[c & 0xFU] );
					break;
			}
		}
		out.append( text.substr( run ) );
		out.push_back( '"' );
	}

	namespace
	{

		void appendNumber( std::string& out, double number )
		{
			if ( !std::isfinite( number ) )
			{
				out.append( "null" );
				return;
			}
			std::array<char, 32> buffer{};
			const auto           result = std::to_chars( buffer.data(), buffer.data() + buffer.size(), number );
			out.append( buffer.data(), result.ptr );
		}

	} // namespace

	void serialize( std::string& out, const Value& value )
	{
		switch ( value.type() )
		{
			case Type::Null:
				out.append( "null" );
				break;
			case Type::Bool:
				out.append( value.asBool() ? "true" : "false" );
				break;
			case Type::Number:
				appendNumber( out, value.asDouble() );
				break;
			case Type::String:
				escape( out, value.asString() );
				break;
			case Type::Array:
			{
				out.push_back( '[' );
				bool first = true;
				for ( const Value& item : value.items() )
				{
					if ( !first )
					{
						out.push_back( ',' );
					}
					first = false;
					serialize( out, item );
				}
				out.push_back( ']' );
				break;
			}
			case Type::Object:
			{
				out.push_back( '{' );
				bool first = true;
				for ( const Member& member : value.members() )
				{
					if ( !first )
					{
						out.push_back( ',' );
					}
					first = false;
					escape( out, member.key );
					out.push_back( ':' );
					serialize( out, member.value );
				}
				out.push_back( '}' );
				break;
			}
		}
	}

	std::string serialize( const Value& value )
	{
		std::string out;
		serialize( out, value );
		return out;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Writer
	// ---------------------------------------------------------------------------------------------------------------------

	void Writer::indent()
	{
		if ( pretty_ )
		{
			out_.push_back( '\n' );
			out_.append( has_items_.size() * 4, ' ' );
		}
	}

	void Writer::prefix()
	{
		if ( after_key_ )
		{
			after_key_ = false;
			return;
		}
		if ( !has_items_.empty() )
		{
			if ( has_items_.back() )
			{
				out_.push_back( ',' );
			}
			has_items_.back() = true;
			indent();
		}
	}

	Writer& Writer::beginObject()
	{
		prefix();
		out_.push_back( '{' );
		has_items_.push_back( false );
		return *this;
	}

	Writer& Writer::endObject()
	{
		const bool had_items = has_items_.back();
		has_items_.pop_back();
		if ( had_items )
		{
			indent();
		}
		out_.push_back( '}' );
		return *this;
	}

	Writer& Writer::beginArray()
	{
		prefix();
		out_.push_back( '[' );
		has_items_.push_back( false );
		return *this;
	}

	Writer& Writer::endArray()
	{
		const bool had_items = has_items_.back();
		has_items_.pop_back();
		if ( had_items )
		{
			indent();
		}
		out_.push_back( ']' );
		return *this;
	}

	Writer& Writer::key( std::string_view name )
	{
		prefix();
		escape( out_, name );
		out_.append( pretty_ ? ": " : ":" );
		after_key_ = true;
		return *this;
	}

	Writer& Writer::value( std::string_view text )
	{
		prefix();
		escape( out_, text );
		return *this;
	}

	Writer& Writer::value( const char* text )
	{
		return value( std::string_view( text != nullptr ? text : "" ) );
	}

	Writer& Writer::value( bool boolean )
	{
		prefix();
		out_.append( boolean ? "true" : "false" );
		return *this;
	}

	Writer& Writer::value( double number )
	{
		prefix();
		appendNumber( out_, number );
		return *this;
	}

	Writer& Writer::null()
	{
		prefix();
		out_.append( "null" );
		return *this;
	}

	Writer& Writer::raw( std::string_view json )
	{
		prefix();
		out_.append( json );
		return *this;
	}

} // namespace lexiglance::json
