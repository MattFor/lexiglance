#include <lexiglance/language/Language.h>

#include "Defined.h"
#include "Variants.h"
#include "ja/Japanese.h"
#include "ru/Russian.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/dictionary/SearchKey.h>
#include <lexiglance/language/BuiltinLanguages.h>
#include <lexiglance/language/Deinflector.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace lexiglance::lang
{

	namespace
	{

		struct Registry
		{
			std::vector<std::unique_ptr<Language>> owned;
			// How many come as code (Japanese and Russian): their files only add dictionaries.
			std::size_t                  coded = 0;
			std::vector<const Language*> all;
		};

		// A language file: the definition of a language, or the dictionaries of one that comes as code. A file for a
		// language already there replaces it, so the user's files can change the built-in ones.
		void addFile( Registry& registry, std::string text, std::string_view origin )
		{
			auto document = json::Document::parse( std::move( text ) );
			if ( !document )
			{
				log::warn( "language file {}: {}", origin, document.error().message );
				return;
			}
			const json::Value& root = document->root();
			const auto         code = root["code"].asString();
			const auto         it   = std::ranges::find_if( registry.owned, [&]( const auto& language ) { return language->code() == code; } );
			if ( it != registry.owned.end() && std::cmp_less( it - registry.owned.begin(), registry.coded ) )
			{
				( *it )->recommend( recommendationsOf( root ) );
				return;
			}
			auto defined = defineLanguage( root );
			if ( !defined )
			{
				log::warn( "language file {}: {}", origin, defined.error().message );
				return;
			}
			if ( it != registry.owned.end() )
			{
				*it = std::move( *defined );
			}
			else
			{
				registry.owned.push_back( std::move( *defined ) );
			}
		}

		Registry load()
		{
			Registry registry;
			registry.owned.push_back( std::make_unique<ja::Japanese>() );
			registry.owned.push_back( std::make_unique<ru::Russian>() );
			registry.coded = registry.owned.size();
			for ( const std::string_view text : builtin_languages )
			{
				addFile( registry, std::string( text ), "(built in)" );
			}

			std::vector<std::filesystem::path> files;
			std::error_code                    ec;
			for ( const auto& entry : std::filesystem::directory_iterator( languagesDirectory(), ec ) )
			{
				if ( entry.path().extension() == ".json" )
				{
					files.push_back( entry.path() );
				}
			}
			std::ranges::sort( files );
			for ( const auto& file : files )
			{
				const std::ifstream in( file, std::ios::binary );
				std::ostringstream  text;
				text << in.rdbuf();
				addFile( registry, text.str(), file.string() );
			}

			for ( const auto& language : registry.owned )
			{
				registry.all.push_back( language.get() );
			}
			return registry;
		}

	} // namespace

	std::span<const Language* const> languages()
	{
		static const Registry registry = load();
		return registry.all;
	}

	std::filesystem::path languagesDirectory()
	{
		return paths::dataDir() / "languages";
	}

	Result<std::unique_ptr<Language>> parseLanguage( std::string_view json )
	{
		auto document = json::Document::parse( std::string( json ) );
		if ( !document )
		{
			return std::unexpected( document.error() );
		}
		return defineLanguage( document->root() );
	}

	const Language* findLanguage( std::string_view code )
	{
		for ( const Language* language : languages() )
		{
			if ( language->code() == code )
			{
				return language;
			}
		}
		return nullptr;
	}

	std::vector<const Language*> enabledLanguages( std::span<const std::string> disabled )
	{
		std::vector<const Language*> enabled;
		std::ranges::copy_if( languages(), std::back_inserter( enabled ), [&]( const Language* language ) { return !std::ranges::contains( disabled, language->code() ); } );
		if ( enabled.empty() )
		{
			std::ranges::copy( languages(), std::back_inserter( enabled ) );
		}
		return enabled;
	}

	const Language& languageOf( std::string_view text, const Language* preferred, std::span<const Language* const> among )
	{
		if ( among.empty() )
		{
			among = languages();
		}
		if ( !std::ranges::contains( among, preferred ) )
		{
			preferred = nullptr;
		}
		const Language* found = nullptr;
		if ( !text.empty() )
		{
			const char32_t c = utf8::first( text );
			for ( const Language* language : among )
			{
				if ( language->isScriptCharacter( c ) )
				{
					if ( language == preferred )
					{
						return *language;
					}
					found = found != nullptr ? found : language;
				}
			}
		}
		if ( found != nullptr )
		{
			return *found;
		}
		return preferred != nullptr ? *preferred : *among.front();
	}

	bool startsInKnownScript( std::string_view text, std::span<const Language* const> among )
	{
		if ( text.empty() )
		{
			return false;
		}
		const char32_t c = utf8::first( text );
		return std::ranges::any_of( among.empty() ? languages() : among, [c]( const Language* language ) { return language->isScriptCharacter( c ); } );
	}

	bool Language::isLookupCharacter( char32_t c ) const noexcept
	{
		return isScriptCharacter( c );
	}

	void Language::variants( std::u32string_view source, std::vector<TextVariant>& out ) const
	{
		Units units;
		units.reserve( source.size() );
		for ( std::size_t i = 0; i < source.size(); ++i )
		{
			units.emplace_back( source[i], static_cast<std::uint32_t>( i + 1 ) );
		}
		addVariant( out, units );
	}

	const Deinflector& Language::deinflector() const noexcept
	{
		static const Deinflector none;
		return none;
	}

	std::vector<RubySegment> Language::headword( std::string_view expression, std::string_view reading ) const
	{
		if ( dict::onlyAddsStress( reading, expression ) )
		{
			return { { .text = std::string( reading ) } };
		}
		if ( reading.empty() || reading == expression )
		{
			return { { .text = std::string( expression ) } };
		}
		return { { .text = std::string( expression ), .reading = std::string( reading ) } };
	}

	TextVariant toVariant( const Units& units )
	{
		TextVariant variant;
		variant.text.reserve( units.size() * 3 );
		variant.offsets.reserve( units.size() + 1 );
		variant.source_length.reserve( units.size() + 1 );
		variant.source_length.push_back( 0 );
		for ( const auto& [c, end] : units )
		{
			variant.offsets.push_back( static_cast<std::uint32_t>( variant.text.size() ) );
			utf8::append( variant.text, c );
			variant.source_length.push_back( end );
		}
		variant.offsets.push_back( static_cast<std::uint32_t>( variant.text.size() ) );
		return variant;
	}

	void addVariant( std::vector<TextVariant>& out, const Units& units )
	{
		if ( TextVariant variant = toVariant( units ); std::ranges::none_of( out, [&]( const TextVariant& v ) { return v.text == variant.text; } ) )
		{
			out.push_back( std::move( variant ) );
		}
	}

} // namespace lexiglance::lang
