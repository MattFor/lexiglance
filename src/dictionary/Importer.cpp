#include <lexiglance/dictionary/Importer.h>

#include <lexiglance/core/Json.h>
#include <lexiglance/core/MappedFile.h>
#include <lexiglance/core/Thread.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/core/Zip.h>
#include <lexiglance/dictionary/Format.h>
#include <lexiglance/dictionary/SearchKey.h>
#include <lexiglance/dictionary/WordRules.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <deque>
#include <fstream>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace lexiglance::dict
{

	namespace
	{

		namespace fs = std::filesystem;
		using namespace format;

		// ---------------------------------------------------------------------------------------------------------------------
		// Sources: a zip archive or an extracted directory
		// ---------------------------------------------------------------------------------------------------------------------

		class Source
		{
		public:
			virtual ~Source() = default;

			[[nodiscard]] virtual std::vector<std::string> names() const                         = 0;
			[[nodiscard]] virtual Result<std::string>      read( const std::string& name ) const = 0;
		};

		class ZipSource final : public Source
		{
		public:
			ZipSource( ZipArchive archive, std::string prefix ) :
				archive_( std::move( archive ) ),
				prefix_( std::move( prefix ) )
			{
			}

			[[nodiscard]] std::vector<std::string> names() const override
			{
				std::vector<std::string> out;
				for ( const auto& entry : archive_.entries() )
				{
					if ( entry.name.starts_with( prefix_ ) && entry.name.size() > prefix_.size() )
					{
						out.push_back( entry.name.substr( prefix_.size() ) );
					}
				}
				return out;
			}

			[[nodiscard]] Result<std::string> read( const std::string& name ) const override
			{
				const auto* entry = archive_.find( prefix_ + name );
				if ( entry == nullptr )
				{
					return fail( "{} not found in archive", name );
				}
				return archive_.read( *entry );
			}

		private:
			ZipArchive  archive_;
			std::string prefix_;
		};

		Result<std::string> readFile( const fs::path& path )
		{
			std::ifstream in( path, std::ios::binary );
			if ( !in )
			{
				return fail( "cannot open {}", path.string() );
			}
			in.seekg( 0, std::ios::end );
			const auto size = static_cast<std::size_t>( in.tellg() );
			in.seekg( 0, std::ios::beg );

			std::string data( size, '\0' );
			in.read( data.data(), static_cast<std::streamsize>( size ) );
			if ( !in )
			{
				return fail( "cannot read {}", path.string() );
			}
			return data;
		}

		class DirectorySource final : public Source
		{
		public:
			explicit DirectorySource( fs::path root ) :
				root_( std::move( root ) )
			{
			}

			[[nodiscard]] std::vector<std::string> names() const override
			{
				std::vector<std::string> out;
				std::error_code          ec;
				for ( const auto& entry : fs::directory_iterator( root_, ec ) )
				{
					if ( entry.is_regular_file( ec ) )
					{
						out.push_back( entry.path().filename().string() );
					}
				}
				return out;
			}

			[[nodiscard]] Result<std::string> read( const std::string& name ) const override
			{
				return readFile( root_ / name );
			}

		private:
			fs::path root_;
		};

		Result<std::unique_ptr<Source>> openSource( const fs::path& path )
		{
			std::error_code ec;
			if ( fs::is_directory( path, ec ) )
			{
				if ( !fs::exists( path / "index.json", ec ) )
				{
					return fail( "{}: not a Yomitan dictionary (index.json missing)", path.string() );
				}
				return std::make_unique<DirectorySource>( path );
			}

			auto archive = ZipArchive::open( path );
			if ( !archive )
			{
				return std::unexpected( archive.error() );
			}

			// Tolerate archives that wrap all files in a single top-level folder.
			std::string prefix;
			if ( archive->find( "index.json" ) == nullptr )
			{
				const auto entries = archive->entries();
				const auto it      = std::ranges::find_if( entries, []( const ZipArchive::Entry& entry ) {
                    const auto slash = entry.name.find( '/' );
                    return slash != std::string::npos && std::string_view( entry.name ).substr( slash + 1 ) == "index.json";
                } );
				if ( it == entries.end() )
				{
					return fail( "{}: not a Yomitan dictionary (index.json missing)", path.string() );
				}
				prefix = it->name.substr( 0, it->name.find( '/' ) + 1 );
			}
			return std::make_unique<ZipSource>( std::move( *archive ), std::move( prefix ) );
		}

		// ---------------------------------------------------------------------------------------------------------------------
		// Bank files
		// ---------------------------------------------------------------------------------------------------------------------

		enum class BankKind : std::uint8_t
		{
			Tag,
			Term,
			TermMeta,
			Kanji,
			KanjiMeta
		};

		struct BankFile
		{
			BankKind    kind   = BankKind::Term;
			unsigned    number = 0;
			std::string name;
		};

		std::optional<BankFile> classify( const std::string& name )
		{
			static constexpr std::array<std::pair<std::string_view, BankKind>, 5> prefixes{ {
					{ "tag_bank_", BankKind::Tag },
					{ "term_bank_", BankKind::Term },
					{ "term_meta_bank_", BankKind::TermMeta },
					{ "kanji_bank_", BankKind::Kanji },
					{ "kanji_meta_bank_", BankKind::KanjiMeta },
			} };

			constexpr std::string_view suffix = ".json";
			for ( const auto& [prefix, kind] : prefixes )
			{
				std::string_view rest = name;
				if ( !rest.starts_with( prefix ) || !rest.ends_with( suffix ) )
				{
					continue;
				}
				rest = rest.substr( prefix.size(), rest.size() - prefix.size() - suffix.size() );

				unsigned   number = 0;
				const auto parsed = std::from_chars( rest.data(), rest.data() + rest.size(), number );
				if ( parsed.ec == std::errc{} && parsed.ptr == rest.data() + rest.size() )
				{
					return BankFile{ .kind = kind, .number = number, .name = name };
				}
			}
			return std::nullopt;
		}

		// Text either viewed inside the parsed document or serialised into the bank's scratch buffer.
		struct Text
		{
			std::string_view view;
			std::uint32_t    offset  = 0;
			std::uint32_t    length  = 0;
			bool             scratch = false;
		};

		struct ParsedTerm
		{
			std::string_view expression;
			std::string_view reading;
			std::string_view definition_tags;
			std::string_view term_tags;
			std::string_view rules;
			std::int32_t     score       = 0;
			std::uint32_t    sequence    = 0;
			std::uint32_t    gloss_begin = 0;
			std::uint32_t    gloss_count = 0;
		};

		struct ParsedGloss
		{
			GlossKind kind = GlossKind::Text;
			Text      data;
		};

		struct ParsedMeta
		{
			std::string_view expression;
			std::string_view reading;
			std::string_view display;
			Text             extra;
			std::int32_t     value = 0;
			MetaKind         kind  = MetaKind::Frequency;
		};

		struct ParsedKanji
		{
			std::string_view character;
			std::string_view onyomi;
			std::string_view kunyomi;
			std::string_view tags;
			std::uint32_t    meanings_begin = 0;
			std::uint32_t    meanings_count = 0;
			std::uint32_t    stats_begin    = 0;
			std::uint32_t    stats_count    = 0;
		};

		struct ParsedTag
		{
			std::string_view name;
			std::string_view category;
			std::string_view notes;
			std::int32_t     order = 0;
			std::int32_t     score = 0;
		};

		struct Bank
		{
			std::optional<json::Document>                  document;
			std::string                                    scratch;
			std::vector<ParsedTerm>                        terms;
			std::vector<ParsedGloss>                       glosses;
			std::vector<ParsedMeta>                        meta;
			std::vector<ParsedKanji>                       kanji;
			std::vector<std::string_view>                  kanji_meanings;
			std::vector<std::pair<std::string_view, Text>> kanji_stats;
			std::vector<ParsedTag>                         tags;
			std::uint64_t                                  images  = 0;
			std::uint64_t                                  skipped = 0;

			Text store( const json::Value& value )
			{
				Text text;
				text.scratch = true;
				text.offset  = static_cast<std::uint32_t>( scratch.size() );
				json::serialize( scratch, value );
				text.length = static_cast<std::uint32_t>( scratch.size() - text.offset );
				return text;
			}

			[[nodiscard]] std::string_view resolve( const Text& text ) const noexcept
			{
				return text.scratch ? std::string_view( scratch ).substr( text.offset, text.length ) : text.view;
			}
		};

		std::int32_t toInt32( std::int64_t value ) noexcept
		{
			return static_cast<std::int32_t>( std::clamp<std::int64_t>( value, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max() ) );
		}

		std::int32_t leadingInteger( std::string_view text ) noexcept
		{
			const auto   start = text.find_first_of( "-0123456789" );
			std::int64_t value = 0;
			if ( start != std::string_view::npos )
			{
				( void )std::from_chars( text.data() + start, text.data() + text.size(), value );
			}
			return toInt32( value );
		}

		void addGloss( Bank& bank, const json::Value& gloss )
		{
			if ( gloss.isString() )
			{
				if ( !gloss.asString().empty() )
				{
					bank.glosses.push_back( { .kind = GlossKind::Text, .data = { .view = gloss.asString() } } );
				}
				return;
			}
			if ( gloss.isArray() )
			{
				bank.glosses.push_back( { .kind = GlossKind::Deinflection, .data = bank.store( gloss ) } );
				return;
			}

			const auto type = gloss["type"].asString();
			if ( type == "text" )
			{
				bank.glosses.push_back( { .kind = GlossKind::Text, .data = { .view = gloss["text"].asString() } } );
			}
			else if ( type == "structured-content" )
			{
				bank.glosses.push_back( { .kind = GlossKind::StructuredContent, .data = bank.store( gloss["content"] ) } );
			}
			else if ( type == "image" )
			{
				++bank.images;
				bank.glosses.push_back( { .kind = GlossKind::Image, .data = bank.store( gloss ) } );
			}
		}

		void parseTerms( const json::Value& root, int version, Bank& bank )
		{
			bank.terms.reserve( root.size() );
			for ( const json::Value& row : root.items() )
			{
				if ( !row.isArray() || row.size() < 5 || row[0].asString().empty() )
				{
					++bank.skipped;
					continue;
				}

				ParsedTerm term;
				term.expression      = row[0].asString();
				term.reading         = row[1].asString( term.expression );
				term.reading         = term.reading.empty() ? term.expression : term.reading;
				term.definition_tags = row[2].asString();
				term.rules           = row[3].asString();
				term.score           = toInt32( row[4].asInt() );
				term.gloss_begin     = static_cast<std::uint32_t>( bank.glosses.size() );

				if ( version == 1 )
				{
					for ( std::size_t i = 5; i < row.size(); ++i )
					{
						addGloss( bank, row[i] );
					}
				}
				else
				{
					for ( const json::Value& gloss : row[5].items() )
					{
						addGloss( bank, gloss );
					}
					term.sequence  = static_cast<std::uint32_t>( std::max<std::int64_t>( 0, row[6].asInt() ) );
					term.term_tags = row[7].asString();
				}

				term.gloss_count = static_cast<std::uint32_t>( bank.glosses.size() ) - term.gloss_begin;
				bank.terms.push_back( term );
			}
		}

		void parseFrequency( const json::Value& data, ParsedMeta& meta )
		{
			const json::Value* frequency = &data;
			if ( data.isObject() && data.find( "frequency" ) != nullptr )
			{
				meta.reading = data["reading"].asString();
				frequency    = &data["frequency"];
			}

			if ( frequency->isNumber() )
			{
				meta.value = toInt32( frequency->asInt() );
			}
			else if ( frequency->isString() )
			{
				meta.display = frequency->asString();
				meta.value   = leadingInteger( meta.display );
			}
			else if ( frequency->isObject() )
			{
				meta.value   = toInt32( ( *frequency )["value"].asInt() );
				meta.display = ( *frequency )["displayValue"].asString();
			}
		}

		void parseMeta( const json::Value& root, bool kanji, Bank& bank )
		{
			bank.meta.reserve( root.size() );
			for ( const json::Value& row : root.items() )
			{
				if ( !row.isArray() || row.size() < 3 )
				{
					++bank.skipped;
					continue;
				}

				const auto         expression = row[0].asString();
				const auto         mode       = row[1].asString();
				const json::Value& data       = row[2];

				if ( mode == "freq" )
				{
					ParsedMeta meta{ .expression = expression, .kind = kanji ? MetaKind::KanjiFrequency : MetaKind::Frequency };
					parseFrequency( data, meta );
					bank.meta.push_back( meta );
				}
				else if ( mode == "pitch" )
				{
					for ( const json::Value& pitch : data["pitches"].items() )
					{
						ParsedMeta  meta{ .expression = expression, .reading = data["reading"].asString(), .kind = MetaKind::Pitch };
						const auto& position = pitch["position"];
						meta.value           = position.isNumber() ? toInt32( position.asInt() ) : -1;
						meta.display         = position.asString();
						meta.extra           = bank.store( pitch );
						bank.meta.push_back( meta );
					}
				}
				else if ( mode == "ipa" )
				{
					for ( const json::Value& transcription : data["transcriptions"].items() )
					{
						ParsedMeta meta{ .expression = expression, .reading = data["reading"].asString(), .kind = MetaKind::Ipa };
						meta.display = transcription["ipa"].asString();
						meta.extra   = bank.store( transcription["tags"] );
						bank.meta.push_back( meta );
					}
				}
				else
				{
					++bank.skipped;
				}
			}
		}

		void parseKanji( const json::Value& root, int version, Bank& bank )
		{
			bank.kanji.reserve( root.size() );
			for ( const json::Value& row : root.items() )
			{
				if ( !row.isArray() || row.size() < 4 || row[0].asString().empty() )
				{
					++bank.skipped;
					continue;
				}

				ParsedKanji kanji{
					.character      = row[0].asString(),
					.onyomi         = row[1].asString(),
					.kunyomi        = row[2].asString(),
					.tags           = row[3].asString(),
					.meanings_begin = static_cast<std::uint32_t>( bank.kanji_meanings.size() ),
					.stats_begin    = static_cast<std::uint32_t>( bank.kanji_stats.size() ),
				};

				if ( version == 1 )
				{
					for ( std::size_t i = 4; i < row.size(); ++i )
					{
						bank.kanji_meanings.push_back( row[i].asString() );
					}
				}
				else
				{
					for ( const json::Value& meaning : row[4].items() )
					{
						bank.kanji_meanings.push_back( meaning.asString() );
					}
					for ( const json::Member& stat : row[5].members() )
					{
						const Text value = stat.value.isString() ? Text{ .view = stat.value.asString() } : bank.store( stat.value );
						bank.kanji_stats.emplace_back( stat.key, value );
					}
				}

				kanji.meanings_count = static_cast<std::uint32_t>( bank.kanji_meanings.size() ) - kanji.meanings_begin;
				kanji.stats_count    = static_cast<std::uint32_t>( bank.kanji_stats.size() ) - kanji.stats_begin;
				bank.kanji.push_back( kanji );
			}
		}

		void parseTags( const json::Value& root, Bank& bank )
		{
			for ( const json::Value& row : root.items() )
			{
				if ( !row.isArray() || row.size() < 5 || row[0].asString().empty() )
				{
					++bank.skipped;
					continue;
				}
				bank.tags.push_back(
						{
								.name     = row[0].asString(),
								.category = row[1].asString(),
								.notes    = row[3].asString(),
								.order    = toInt32( row[2].asInt() ),
								.score    = toInt32( row[4].asInt() ),
						}
				);
			}
		}

		Result<Bank> loadBank( const Source& source, const BankFile& file, int version )
		{
			auto text = source.read( file.name );
			if ( !text )
			{
				return std::unexpected( text.error() );
			}
			auto document = json::Document::parse( std::move( *text ) );
			if ( !document )
			{
				return failWith( file.name, document.error() );
			}

			Bank bank;
			bank.document.emplace( std::move( *document ) );
			const json::Value& root = bank.document->root();
			if ( !root.isArray() )
			{
				return fail( "{}: expected a JSON array", file.name );
			}

			switch ( file.kind )
			{
				case BankKind::Term:
					parseTerms( root, version, bank );
					break;
				case BankKind::TermMeta:
					parseMeta( root, false, bank );
					break;
				case BankKind::KanjiMeta:
					parseMeta( root, true, bank );
					break;
				case BankKind::Kanji:
					parseKanji( root, version, bank );
					break;
				case BankKind::Tag:
					parseTags( root, bank );
					break;
			}
			return bank;
		}

		// ---------------------------------------------------------------------------------------------------------------------
		// Output
		// ---------------------------------------------------------------------------------------------------------------------

		// Streams the string pool straight to the output file, deduplicating short strings (tags, readings, ...).
		class PoolWriter
		{
		public:
			explicit PoolWriter( std::ofstream& out ) :
				out_( &out )
			{
			}

			StrRef add( std::string_view text, bool dedupe )
			{
				if ( text.empty() )
				{
					return {};
				}
				if ( dedupe )
				{
					if ( const auto it = seen_.find( text ); it != seen_.end() )
					{
						return it->second;
					}
				}
				if ( size_ + text.size() > std::numeric_limits<std::uint32_t>::max() )
				{
					overflow_ = true;
					return {};
				}

				const StrRef ref{ .offset = static_cast<std::uint32_t>( size_ ), .length = static_cast<std::uint32_t>( text.size() ) };
				buffer_.append( text );
				size_ += text.size();
				if ( buffer_.size() >= flush_threshold )
				{
					flush();
				}
				if ( dedupe )
				{
					seen_.emplace( std::string( text ), ref );
				}
				return ref;
			}

			void flush()
			{
				out_->write( buffer_.data(), static_cast<std::streamsize>( buffer_.size() ) );
				buffer_.clear();
			}

			[[nodiscard]] std::uint64_t size() const noexcept
			{
				return size_;
			}

			[[nodiscard]] bool overflowed() const noexcept
			{
				return overflow_;
			}

		private:
			static constexpr std::size_t flush_threshold = 8U << 20U;

			std::ofstream*                                                       out_;
			std::string                                                          buffer_;
			std::uint64_t                                                        size_     = 0;
			bool                                                                 overflow_ = false;
			std::unordered_map<std::string, StrRef, StringHash, std::equal_to<>> seen_;
		};

		using IndexKey = std::pair<std::uint64_t, std::uint32_t>;

		struct BuiltIndex
		{
			std::vector<IndexSlot>     slots;
			std::vector<std::uint32_t> postings;
		};

		BuiltIndex buildIndex( std::vector<IndexKey>& keys )
		{
			std::ranges::sort( keys );
			const auto duplicates = std::ranges::unique( keys );
			keys.erase( duplicates.begin(), duplicates.end() );

			std::size_t groups = 0;
			for ( std::size_t i = 0; i < keys.size(); ++i )
			{
				if ( i == 0 || keys[i].first != keys[i - 1].first )
				{
					++groups;
				}
			}

			BuiltIndex index;
			index.slots.resize( std::bit_ceil( std::max<std::size_t>( 16, groups * 2 ) ) );
			index.postings.reserve( keys.size() );
			const std::size_t mask = index.slots.size() - 1;

			for ( std::size_t i = 0; i < keys.size(); )
			{
				const std::uint64_t hash  = keys[i].first;
				const auto          begin = static_cast<std::uint32_t>( index.postings.size() );
				for ( ; i < keys.size() && keys[i].first == hash; ++i )
				{
					index.postings.push_back( keys[i].second );
				}

				std::size_t pos = hash & mask;
				while ( index.slots[pos].hash != 0 )
				{
					pos = ( pos + 1 ) & mask;
				}
				index.slots[pos] = { .hash = hash, .begin = begin, .count = static_cast<std::uint32_t>( index.postings.size() ) - begin };
			}
			return index;
		}

		class Compiler
		{
		public:
			explicit Compiler( std::ofstream& out ) :
				out_( &out ),
				pool_( out )
			{
			}

			void addTag( std::string_view name, std::string_view category, std::string_view notes, std::int32_t order, std::int32_t score )
			{
				tag_names_.emplace_back( name );
				tags_.push_back( { .name = pool_.add( name, true ), .category = pool_.add( category, true ), .notes = pool_.add( notes, true ), .order = order, .score = score } );
			}

			void add( const Bank& bank )
			{
				for ( const ParsedTag& tag : bank.tags )
				{
					addTag( tag.name, tag.category, tag.notes, tag.order, tag.score );
				}
				for ( const ParsedTerm& term : bank.terms )
				{
					addTerm( bank, term );
				}
				for ( const ParsedMeta& meta : bank.meta )
				{
					const auto index = static_cast<std::uint32_t>( meta_.size() );
					meta_keys_.emplace_back( indexHash( meta.expression ), index );
					meta_.push_back(
							{
									.expression = pool_.add( meta.expression, true ),
									.reading    = pool_.add( meta.reading, true ),
									.display    = pool_.add( meta.display, true ),
									.extra      = pool_.add( bank.resolve( meta.extra ), true ),
									.value      = meta.value,
									.kind       = meta.kind,
							}
					);
				}
				for ( const ParsedKanji& kanji : bank.kanji )
				{
					addKanji( bank, kanji );
				}
				images_ += bank.images;
				skipped_ += bank.skipped;
			}

			Result<Header> finish( std::string_view index_json, std::string_view styles )
			{
				Header header{};
				header.index_json = pool_.add( index_json, false );
				header.styles_css = pool_.add( styles, false );
				pool_.flush();
				if ( pool_.overflowed() )
				{
					return fail( "dictionary is too large (string pool exceeds 4 GiB)" );
				}
				header.pool = { .offset = sizeof( Header ), .count = pool_.size() };

				std::ranges::stable_sort( kanji_, {}, &KanjiRecord::codepoint );
				sortTags();

				auto term_index = buildIndex( term_keys_ );
				auto meta_index = buildIndex( meta_keys_ );

				header.terms         = write( terms_ );
				header.glosses       = write( glosses_ );
				header.term_index    = write( term_index.slots );
				header.term_postings = write( term_index.postings );
				header.meta          = write( meta_ );
				header.meta_index    = write( meta_index.slots );
				header.meta_postings = write( meta_index.postings );
				header.kanji         = write( kanji_ );
				header.tags          = write( tags_ );
				header.string_lists  = write( string_lists_ );

				header.magic       = magic;
				header.version     = version;
				header.header_size = sizeof( Header );
				header.file_size   = static_cast<std::uint64_t>( out_->tellp() );
				header.import_time = static_cast<std::uint64_t>( std::chrono::duration_cast<std::chrono::seconds>( std::chrono::system_clock::now().time_since_epoch() ).count() );

				out_->seekp( 0 );
				out_->write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
				out_->flush();
				if ( !*out_ )
				{
					return fail( "write error" );
				}
				return header;
			}

			[[nodiscard]] std::uint64_t termCount() const noexcept
			{
				return terms_.size();
			}

			[[nodiscard]] std::uint64_t metaCount() const noexcept
			{
				return meta_.size();
			}

			[[nodiscard]] std::uint64_t kanjiCount() const noexcept
			{
				return kanji_.size();
			}

			[[nodiscard]] std::uint64_t tagCount() const noexcept
			{
				return tags_.size();
			}

			[[nodiscard]] std::uint64_t images() const noexcept
			{
				return images_;
			}

			[[nodiscard]] std::uint64_t skipped() const noexcept
			{
				return skipped_;
			}

		private:
			void addTerm( const Bank& bank, const ParsedTerm& term )
			{
				TermRecord record{
					.expression      = pool_.add( term.expression, true ),
					.reading         = pool_.add( term.reading, true ),
					.definition_tags = pool_.add( term.definition_tags, true ),
					.term_tags       = pool_.add( term.term_tags, true ),
					.rules_text      = pool_.add( term.rules, true ),
					.rules           = rule::parse( term.rules ) | ( rule::parse( term.definition_tags ) & rule::from_tags ),
					.score           = term.score,
					.sequence        = term.sequence,
					.gloss_begin     = static_cast<std::uint32_t>( glosses_.size() ),
				};

				for ( std::uint32_t i = 0; i < term.gloss_count; ++i )
				{
					const ParsedGloss& gloss = bank.glosses[term.gloss_begin + i];
					const auto         data  = bank.resolve( gloss.data );
					glosses_.push_back( { .data = pool_.add( data, gloss.kind == GlossKind::Text && data.size() <= 64 ), .kind = gloss.kind } );
				}
				record.gloss_count = static_cast<std::uint32_t>( glosses_.size() ) - record.gloss_begin;

				const auto index = static_cast<std::uint32_t>( terms_.size() );
				addKeys( term.expression, index );
				if ( term.reading != term.expression )
				{
					addKeys( term.reading, index );
				}
				terms_.push_back( record );
			}

			// A term is found by its spelling and by its search key (without stress marks, ё as е).
			void addKeys( std::string_view text, std::uint32_t index )
			{
				term_keys_.emplace_back( indexHash( text ), index );
				if ( const std::string key = searchKey( text ); !key.empty() )
				{
					term_keys_.emplace_back( indexHash( key ), index );
				}
			}

			void addKanji( const Bank& bank, const ParsedKanji& kanji )
			{
				KanjiRecord record{
					.codepoint      = static_cast<std::uint32_t>( utf8::first( kanji.character ) ),
					.character      = pool_.add( kanji.character, true ),
					.onyomi         = pool_.add( kanji.onyomi, true ),
					.kunyomi        = pool_.add( kanji.kunyomi, true ),
					.tags           = pool_.add( kanji.tags, true ),
					.meanings_begin = static_cast<std::uint32_t>( string_lists_.size() ),
					.meanings_count = kanji.meanings_count,
				};
				for ( std::uint32_t i = 0; i < kanji.meanings_count; ++i )
				{
					string_lists_.push_back( pool_.add( bank.kanji_meanings[kanji.meanings_begin + i], true ) );
				}

				record.stats_begin = static_cast<std::uint32_t>( string_lists_.size() );
				record.stats_count = kanji.stats_count;
				for ( std::uint32_t i = 0; i < kanji.stats_count; ++i )
				{
					const auto& [key, value] = bank.kanji_stats[kanji.stats_begin + i];
					string_lists_.push_back( pool_.add( key, true ) );
					string_lists_.push_back( pool_.add( bank.resolve( value ), true ) );
				}
				kanji_.push_back( record );
			}

			// Tags are binary searched by name; the first definition of a duplicated name wins.
			void sortTags()
			{
				std::vector<std::size_t> order( tags_.size() );
				for ( std::size_t i = 0; i < order.size(); ++i )
				{
					order[i] = i;
				}
				std::ranges::stable_sort( order, {}, [this]( std::size_t i ) -> const std::string& { return tag_names_[i]; } );

				std::vector<TagRecord> sorted;
				sorted.reserve( tags_.size() );
				for ( std::size_t k = 0; k < order.size(); ++k )
				{
					if ( k > 0 && tag_names_[order[k]] == tag_names_[order[k - 1]] )
					{
						continue;
					}
					sorted.push_back( tags_[order[k]] );
				}
				tags_ = std::move( sorted );
			}

			void align()
			{
				static constexpr std::array<char, 8> zeros{};
				const auto                           pos = static_cast<std::uint64_t>( out_->tellp() );
				out_->write( zeros.data(), static_cast<std::streamsize>( ( 8 - ( pos % 8 ) ) % 8 ) );
			}

			template <typename T>
			Section write( const std::vector<T>& items )
			{
				align();
				const Section section{ .offset = static_cast<std::uint64_t>( out_->tellp() ), .count = items.size() };
				out_->write( reinterpret_cast<const char*>( items.data() ), static_cast<std::streamsize>( items.size() * sizeof( T ) ) );
				return section;
			}

			std::ofstream*           out_;
			PoolWriter               pool_;
			std::vector<TermRecord>  terms_;
			std::vector<GlossRecord> glosses_;
			std::vector<IndexKey>    term_keys_;
			std::vector<MetaRecord>  meta_;
			std::vector<IndexKey>    meta_keys_;
			std::vector<KanjiRecord> kanji_;
			std::vector<TagRecord>   tags_;
			std::vector<std::string> tag_names_;
			std::vector<StrRef>      string_lists_;
			std::uint64_t            images_  = 0;
			std::uint64_t            skipped_ = 0;
		};

		// Removes the partially written output unless the import succeeded.
		struct TemporaryFile
		{
			TemporaryFile( const TemporaryFile& )            = delete;
			TemporaryFile& operator=( const TemporaryFile& ) = delete;
			TemporaryFile( TemporaryFile&& )                 = delete;
			TemporaryFile& operator=( TemporaryFile&& )      = delete;

			explicit TemporaryFile( fs::path file ) :
				path( std::move( file ) )
			{
			}

			~TemporaryFile()
			{
				if ( !keep )
				{
					std::error_code ec;
					fs::remove( path, ec );
				}
			}

			fs::path path;
			bool     keep = false;
		};

	} // namespace

	Result<std::string> readTitle( const fs::path& source )
	{
		auto opened = openSource( source );
		if ( !opened )
		{
			return std::unexpected( opened.error() );
		}
		auto text = ( *opened )->read( "index.json" );
		if ( !text )
		{
			return std::unexpected( text.error() );
		}
		auto index = json::Document::parse( std::move( *text ) );
		if ( !index )
		{
			return failWith( "index.json", index.error() );
		}
		std::string title( index->root()["title"].asString() );
		if ( title.empty() )
		{
			return fail( "index.json has no title" );
		}
		return title;
	}

	Result<ImportSummary> compile( const fs::path& source_path, const fs::path& output, const ImportOptions& options )
	{
		const auto started = std::chrono::steady_clock::now();
		const auto report  = [&]( std::string_view stage, std::uint64_t done, std::uint64_t total ) {
            if ( options.on_progress )
            {
                options.on_progress( { .stage = stage, .done = done, .total = total } );
            }
		};

		report( "reading", 0, 0 );
		auto opened = openSource( source_path );
		if ( !opened )
		{
			return std::unexpected( opened.error() );
		}
		const Source& source = **opened;

		auto index_json = source.read( "index.json" );
		if ( !index_json )
		{
			return std::unexpected( index_json.error() );
		}
		auto index = json::Document::parse( *index_json );
		if ( !index )
		{
			return failWith( "index.json", index.error() );
		}

		const json::Value& info = index->root();
		ImportSummary      summary;
		summary.title    = std::string( info["title"].asString() );
		summary.revision = std::string( info["revision"].asString() );
		if ( summary.title.empty() )
		{
			return fail( "index.json has no title" );
		}
		const auto format_version = static_cast<int>( info["format"].asInt( info["version"].asInt( 0 ) ) );
		if ( format_version < 1 || format_version > 3 )
		{
			return fail( "unsupported dictionary format version {}", format_version );
		}

		std::vector<BankFile> banks;
		std::string           styles;
		for ( const std::string& name : source.names() )
		{
			if ( auto bank = classify( name ) )
			{
				banks.push_back( std::move( *bank ) );
			}
			else if ( name == "styles.css" )
			{
				styles = source.read( name ).value_or( std::string() );
			}
		}
		std::ranges::sort( banks, []( const BankFile& a, const BankFile& b ) { return std::tie( a.kind, a.number ) < std::tie( b.kind, b.number ); } );

		std::error_code ec;
		fs::create_directories( output.parent_path(), ec );
		fs::path temp_path = output;
		temp_path += ".tmp";
		TemporaryFile temp( temp_path );

		std::ofstream out( temp.path, std::ios::binary | std::ios::trunc );
		if ( !out )
		{
			return fail( "cannot create {}", temp.path.string() );
		}
		const Header placeholder{};
		out.write( reinterpret_cast<const char*>( &placeholder ), sizeof( placeholder ) );

		Compiler compiler( out );
		for ( const json::Member& tag : info["tagMeta"].members() )
		{
			compiler.addTag(
					tag.key,
					tag.value["category"].asString(),
					tag.value["notes"].asString(),
					toInt32( tag.value["order"].asInt() ),
					toInt32( tag.value["score"].asInt() )
			);
		}

		{
			ThreadPool                            pool( options.threads, options.background, "lg-import" );
			const std::size_t                     window = static_cast<std::size_t>( pool.size() ) * 2;
			std::deque<std::future<Result<Bank>>> pending;
			std::size_t                           next = 0;

			const auto submit = [&] {
				const BankFile& file = banks[next++];
				pending.push_back( pool.submit( [&source, &file, format_version] { return loadBank( source, file, format_version ); } ) );
			};
			while ( next < banks.size() && pending.size() < window )
			{
				submit();
			}

			std::uint64_t done = 0;
			report( "parsing", 0, banks.size() );
			while ( !pending.empty() )
			{
				if ( options.stop.stop_requested() )
				{
					return fail( "import cancelled" );
				}

				auto bank = pending.front().get();
				pending.pop_front();
				if ( next < banks.size() )
				{
					submit();
				}
				if ( !bank )
				{
					return std::unexpected( bank.error() );
				}
				compiler.add( *bank );
				report( "parsing", ++done, banks.size() );
			}
		}

		report( "indexing", 0, 0 );
		const auto header = compiler.finish( *index_json, styles );
		if ( !header )
		{
			return std::unexpected( header.error() );
		}
		out.close();
		if ( out.fail() )
		{
			return fail( "cannot write {}", temp.path.string() );
		}

		// The dictionary it replaces may be mapped by the daemon.
		if ( auto moved = replaceMapped( temp.path, output ); !moved )
		{
			return fail( "cannot move dictionary into place: {}", moved.error().message );
		}
		temp.keep = true;

		summary.terms   = compiler.termCount();
		summary.meta    = compiler.metaCount();
		summary.kanji   = compiler.kanjiCount();
		summary.tags    = compiler.tagCount();
		summary.images  = compiler.images();
		summary.skipped = compiler.skipped();
		summary.bytes   = header->file_size;
		summary.seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - started ).count();
		return summary;
	}

} // namespace lexiglance::dict
