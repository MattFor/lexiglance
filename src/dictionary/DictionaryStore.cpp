#include <lexiglance/dictionary/DictionaryStore.h>

#include <lexiglance/core/Hash.h>
#include <lexiglance/core/MappedFile.h>

#include <algorithm>
#include <format>
#include <string>

namespace lexiglance::dict
{

	namespace fs = std::filesystem;

	fs::path DictionaryStore::fileFor( std::string_view title ) const
	{
		std::string slug;
		for ( const char c : title )
		{
			const bool alnum = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' );
			if ( alnum )
			{
				slug.push_back( static_cast<char>( c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c ) );
			}
			else if ( !slug.empty() && slug.back() != '-' )
			{
				slug.push_back( '-' );
			}
		}
		while ( !slug.empty() && slug.back() == '-' )
		{
			slug.pop_back();
		}
		slug.resize( std::min<std::size_t>( slug.size(), 48 ) );
		if ( slug.empty() )
		{
			slug = "dictionary";
		}

		const auto id = static_cast<std::uint32_t>( hash64( title ) );
		return directory_ / std::format( "{}-{:08x}{}", slug, id, format::extension );
	}

	std::vector<std::shared_ptr<const Dictionary>> DictionaryStore::loadAll( std::vector<Error>* errors ) const
	{
		// Removed or replaced dictionaries Windows kept while they were mapped.
		sweepRemoved( directory_ );

		std::vector<std::shared_ptr<const Dictionary>> out;
		std::error_code                                ec;
		for ( const auto& entry : fs::directory_iterator( directory_, ec ) )
		{
			if ( !entry.is_regular_file( ec ) || entry.path().extension() != format::extension )
			{
				continue;
			}
			auto dictionary = Dictionary::open( entry.path() );
			if ( dictionary )
			{
				out.push_back( std::move( *dictionary ) );
			}
			else if ( errors != nullptr )
			{
				errors->push_back( dictionary.error() );
			}
		}
		std::ranges::sort( out, {}, []( const auto& d ) -> const std::string& { return d->info().title; } );
		return out;
	}

	Result<ImportSummary> DictionaryStore::install( const fs::path& source, const ImportOptions& options, bool replace ) const
	{
		const auto title = readTitle( source );
		if ( !title )
		{
			return std::unexpected( title.error() );
		}

		const auto      target = fileFor( *title );
		std::error_code ec;
		if ( !replace && fs::exists( target, ec ) )
		{
			return fail( "\"{}\" is already installed", *title );
		}
		return compile( source, target, options );
	}

	Result<> DictionaryStore::remove( std::string_view title ) const
	{
		const auto      file = fileFor( title );
		std::error_code ec;
		if ( !fs::exists( file, ec ) )
		{
			return fail( "cannot remove \"{}\": not installed", title );
		}
		// The daemon may still have it mapped.
		if ( auto removed = removeMapped( file ); !removed )
		{
			return fail( "cannot remove \"{}\": {}", title, removed.error().message );
		}
		return {};
	}

} // namespace lexiglance::dict
