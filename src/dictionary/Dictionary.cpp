#include <lexiglance/dictionary/Dictionary.h>

#include <lexiglance/core/Json.h>

#include <algorithm>
#include <cstring>

namespace lexiglance::dict
{

	namespace
	{

		using namespace format;

		template <typename T>
		Result<std::span<const T>> section( const MappedFile& file, const Section& s, std::string_view name )
		{
			if ( s.count == 0 )
			{
				return std::span<const T>{};
			}
			const bool aligned = s.offset % alignof( T ) == 0;
			const bool fits    = s.offset <= file.size() && s.count <= ( file.size() - s.offset ) / sizeof( T );
			if ( !aligned || !fits )
			{
				return fail( "corrupt {} section", name );
			}
			return std::span<const T>( reinterpret_cast<const T*>( file.data() + s.offset ), static_cast<std::size_t>( s.count ) );
		}

		DictionaryInfo parseInfo( std::string_view index_json )
		{
			DictionaryInfo info;
			auto           doc = json::Document::parse( std::string( index_json ) );
			if ( !doc )
			{
				return info;
			}

			const auto& root     = doc->root();
			const auto  text     = [&]( std::string_view key ) { return std::string( root[key].asString() ); };
			info.title           = text( "title" );
			info.revision        = text( "revision" );
			info.author          = text( "author" );
			info.url             = text( "url" );
			info.description     = text( "description" );
			info.attribution     = text( "attribution" );
			info.source_language = text( "sourceLanguage" );
			info.target_language = text( "targetLanguage" );
			info.frequency_mode  = text( "frequencyMode" );
			info.index_url       = text( "indexUrl" );
			info.download_url    = text( "downloadUrl" );
			info.format          = static_cast<int>( root["format"].asInt( root["version"].asInt( 3 ) ) );
			info.sequenced       = root["sequenced"].asBool();
			info.updatable       = root["isUpdatable"].asBool();
			return info;
		}

	} // namespace

	Result<std::shared_ptr<const Dictionary>> Dictionary::open( const std::filesystem::path& path )
	{
		auto file = MappedFile::open( path );
		if ( !file )
		{
			return std::unexpected( file.error() );
		}
		if ( file->size() < sizeof( Header ) )
		{
			return fail( "{}: not a compiled dictionary", path.string() );
		}

		std::shared_ptr<Dictionary> dict( new Dictionary() );
		dict->file_   = std::move( *file );
		dict->path_   = path;
		dict->header_ = reinterpret_cast<const Header*>( dict->file_.data() );

		const Header& h = *dict->header_;
		if ( h.magic != magic )
		{
			return fail( "{}: not a compiled dictionary", path.string() );
		}
		if ( h.version != version || h.header_size != sizeof( Header ) )
		{
			return fail( "{}: unsupported dictionary format version {} (expected {}), re-import it", path.string(), h.version, version );
		}
		if ( h.file_size != dict->file_.size() || h.pool.offset > h.file_size || h.pool.count > h.file_size - h.pool.offset )
		{
			return fail( "{}: truncated dictionary", path.string() );
		}

		dict->pool_ = std::string_view( dict->file_.data() + h.pool.offset, static_cast<std::size_t>( h.pool.count ) );

		auto error = [&]( const Error& e ) { return failWith( path.string(), e ); };

		const auto terms         = section<TermRecord>( dict->file_, h.terms, "terms" );
		const auto glosses       = section<GlossRecord>( dict->file_, h.glosses, "glosses" );
		const auto term_index    = section<IndexSlot>( dict->file_, h.term_index, "term index" );
		const auto term_postings = section<std::uint32_t>( dict->file_, h.term_postings, "term postings" );
		const auto meta          = section<MetaRecord>( dict->file_, h.meta, "meta" );
		const auto meta_index    = section<IndexSlot>( dict->file_, h.meta_index, "meta index" );
		const auto meta_postings = section<std::uint32_t>( dict->file_, h.meta_postings, "meta postings" );
		const auto kanji         = section<KanjiRecord>( dict->file_, h.kanji, "kanji" );
		const auto tags          = section<TagRecord>( dict->file_, h.tags, "tags" );
		const auto string_lists  = section<StrRef>( dict->file_, h.string_lists, "string lists" );

		if ( !terms )
		{
			return error( terms.error() );
		}
		if ( !glosses )
		{
			return error( glosses.error() );
		}
		if ( !term_index || !std::has_single_bit( std::max<std::size_t>( term_index->size(), 1 ) ) )
		{
			return fail( "{}: corrupt term index", path.string() );
		}
		if ( !term_postings )
		{
			return error( term_postings.error() );
		}
		if ( !meta )
		{
			return error( meta.error() );
		}
		if ( !meta_index || !std::has_single_bit( std::max<std::size_t>( meta_index->size(), 1 ) ) )
		{
			return fail( "{}: corrupt meta index", path.string() );
		}
		if ( !meta_postings )
		{
			return error( meta_postings.error() );
		}
		if ( !kanji )
		{
			return error( kanji.error() );
		}
		if ( !tags )
		{
			return error( tags.error() );
		}
		if ( !string_lists )
		{
			return error( string_lists.error() );
		}

		dict->terms_         = *terms;
		dict->glosses_       = *glosses;
		dict->term_index_    = *term_index;
		dict->term_postings_ = *term_postings;
		dict->meta_          = *meta;
		dict->meta_index_    = *meta_index;
		dict->meta_postings_ = *meta_postings;
		dict->kanji_         = *kanji;
		dict->tags_          = *tags;
		dict->string_lists_  = *string_lists;
		dict->info_          = parseInfo( dict->indexJson() );

		dict->file_.adviseRandom();
		return dict;
	}

	std::string_view Dictionary::string( StrRef ref ) const noexcept
	{
		if ( ref.offset > pool_.size() || ref.length > pool_.size() - ref.offset )
		{
			return {};
		}
		return pool_.substr( ref.offset, ref.length );
	}

	std::span<const std::uint32_t> Dictionary::probe( std::span<const IndexSlot> index, std::span<const std::uint32_t> postings, std::string_view key ) noexcept
	{
		if ( index.empty() )
		{
			return {};
		}

		const std::uint64_t hash = indexHash( key );
		const std::size_t   mask = index.size() - 1;
		for ( std::size_t pos = hash & mask, probes = 0; probes < index.size(); pos = ( pos + 1 ) & mask, ++probes )
		{
			const IndexSlot& slot = index[pos];
			if ( slot.hash == 0 )
			{
				return {};
			}
			if ( slot.hash == hash )
			{
				if ( slot.begin > postings.size() || slot.count > postings.size() - slot.begin )
				{
					return {};
				}
				return postings.subspan( slot.begin, slot.count );
			}
		}
		return {};
	}

	std::span<const std::uint32_t> Dictionary::findTerms( std::string_view key ) const noexcept
	{
		return probe( term_index_, term_postings_, key );
	}

	std::span<const std::uint32_t> Dictionary::findMeta( std::string_view expression ) const noexcept
	{
		return probe( meta_index_, meta_postings_, expression );
	}

	std::span<const GlossRecord> Dictionary::glossary( const TermRecord& term ) const noexcept
	{
		if ( term.gloss_begin > glosses_.size() || term.gloss_count > glosses_.size() - term.gloss_begin )
		{
			return {};
		}
		return glosses_.subspan( term.gloss_begin, term.gloss_count );
	}

	const KanjiRecord* Dictionary::findKanji( char32_t codepoint ) const noexcept
	{
		const auto it = std::ranges::lower_bound( kanji_, static_cast<std::uint32_t>( codepoint ), {}, &KanjiRecord::codepoint );
		return it != kanji_.end() && it->codepoint == codepoint ? &*it : nullptr;
	}

	const TagRecord* Dictionary::findTag( std::string_view name ) const noexcept
	{
		const auto it = std::ranges::lower_bound( tags_, name, {}, [this]( const TagRecord& tag ) { return string( tag.name ); } );
		return it != tags_.end() && string( it->name ) == name ? &*it : nullptr;
	}

	std::span<const StrRef> Dictionary::stringList( std::uint32_t begin, std::uint32_t count ) const noexcept
	{
		if ( begin > string_lists_.size() || count > string_lists_.size() - begin )
		{
			return {};
		}
		return string_lists_.subspan( begin, count );
	}

	void Dictionary::prefetchIndexes() const noexcept
	{
		const Header& h = *header_;
		file_.prefetch( h.term_index.offset, h.term_index.count * sizeof( IndexSlot ) );
		file_.prefetch( h.term_postings.offset, h.term_postings.count * sizeof( std::uint32_t ) );
		file_.prefetch( h.terms.offset, h.terms.count * sizeof( TermRecord ) );
		file_.prefetch( h.meta_index.offset, h.meta_index.count * sizeof( IndexSlot ) );
		file_.prefetch( h.meta_postings.offset, h.meta_postings.count * sizeof( std::uint32_t ) );
		file_.prefetch( h.meta.offset, h.meta.count * sizeof( MetaRecord ) );
	}

} // namespace lexiglance::dict
