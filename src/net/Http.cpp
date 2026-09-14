#include <lexiglance/net/Http.h>

#include <lexiglance/core/Version.h>

#include <format>
#include <iterator>
#include <memory>

#ifdef LEXIGLANCE_HAVE_CURL
	#include <curl/curl.h>
#endif

namespace lexiglance::net
{

#ifdef LEXIGLANCE_HAVE_CURL

	namespace
	{

		struct Global
		{
			Global()
			{
				curl_global_init( CURL_GLOBAL_DEFAULT );
			}
			~Global()
			{
				curl_global_cleanup();
			}

			Global( const Global& )            = delete;
			Global& operator=( const Global& ) = delete;
			Global( Global&& )                 = delete;
			Global& operator=( Global&& )      = delete;
		};

		struct EasyDeleter
		{
			void operator()( CURL* handle ) const noexcept
			{
				curl_easy_cleanup( handle );
			}
		};

		struct ListDeleter
		{
			void operator()( curl_slist* list ) const noexcept
			{
				curl_slist_free_all( list );
			}
		};

		// The only variadic call; every option goes through here.
		template <typename T>
		void set( CURL* handle, CURLoption option, T value )
		{
			curl_easy_setopt( handle, option, value ); // NOLINT(cppcoreguidelines-pro-type-vararg): libcurl's C API
		}

		std::size_t append( char* data, std::size_t size, std::size_t count, void* target )
		{
			static_cast<std::string*>( target )->append( data, size * count );
			return size * count;
		}

	} // namespace

	Result<Response> fetch( const Request& request )
	{
		static const Global global;

		const std::unique_ptr<CURL, EasyDeleter> handle( curl_easy_init() );
		if ( !handle )
		{
			return fail( "cannot initialise libcurl" );
		}
		CURL* const       curl       = handle.get();
		const std::string user_agent = std::format( "Lexiglance/{}", version );
		const auto        timeout    = static_cast<long>( request.timeout.count() );

		Response response;
		set( curl, CURLOPT_URL, request.url.c_str() );
		set( curl, CURLOPT_USERAGENT, user_agent.c_str() );
		set( curl, CURLOPT_FOLLOWLOCATION, 1L );
		set( curl, CURLOPT_MAXREDIRS, 5L );
	#if LIBCURL_VERSION_NUM >= 0x075500
		// Only the web, also after redirects: no file://, scp:// or other schemes from a server or a dictionary.
		set( curl, CURLOPT_PROTOCOLS_STR, "http,https" );
		set( curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https" );
	#endif
		set( curl, CURLOPT_NOSIGNAL, 1L );
	#ifdef _WIN32
		// Windows' own certificate store: a libcurl built for MSYS2 looks for its CA bundle inside the MSYS2 tree.
		set( curl, CURLOPT_SSL_OPTIONS, static_cast<long>( CURLSSLOPT_NATIVE_CA ) );
	#endif
		set( curl, CURLOPT_TIMEOUT_MS, timeout );
		set( curl, CURLOPT_CONNECTTIMEOUT_MS, std::min( timeout, 5000L ) );
		set( curl, CURLOPT_ACCEPT_ENCODING, "" );
		set( curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>( 64 ) << 20U );
		set( curl, CURLOPT_WRITEFUNCTION, &append );
		set( curl, CURLOPT_WRITEDATA, static_cast<void*>( &response.body ) );

		std::unique_ptr<curl_slist, ListDeleter> headers;
		const std::string                        content_type = "Content-Type: " + request.content_type;
		if ( !request.body.empty() )
		{
			headers.reset( curl_slist_append( nullptr, content_type.c_str() ) );
			set( curl, CURLOPT_HTTPHEADER, headers.get() );
			set( curl, CURLOPT_POSTFIELDS, request.body.c_str() );
			set( curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>( request.body.size() ) );
		}

		if ( const CURLcode code = curl_easy_perform( curl ); code != CURLE_OK )
		{
			return fail( "{}", curl_easy_strerror( code ) );
		}
		( void )curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, &response.status ); // NOLINT(cppcoreguidelines-pro-type-vararg)
		char* type = nullptr;
		if ( curl_easy_getinfo( curl, CURLINFO_CONTENT_TYPE, &type ) == CURLE_OK && type != nullptr ) // NOLINT(cppcoreguidelines-pro-type-vararg)
		{
			response.content_type = type;
		}
		return response;
	}

#else

	Result<Response> fetch( const Request& /*request*/ )
	{
		return fail( "Lexiglance was built without libcurl" );
	}

#endif

	std::string urlEncode( std::string_view text )
	{
		std::string out;
		out.reserve( text.size() * 3 );
		for ( const char c : text )
		{
			const auto byte = static_cast<unsigned char>( c );
			if ( ( byte >= 'A' && byte <= 'Z' ) || ( byte >= 'a' && byte <= 'z' ) || ( byte >= '0' && byte <= '9' ) || c == '-' || c == '_' || c == '.' || c == '~' )
			{
				out.push_back( c );
			}
			else
			{
				std::format_to( std::back_inserter( out ), "%{:02X}", byte );
			}
		}
		return out;
	}

} // namespace lexiglance::net
