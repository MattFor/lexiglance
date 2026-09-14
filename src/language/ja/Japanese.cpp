#include "Japanese.h"

#include "../Variants.h"

#include <lexiglance/core/Utf8.h>
#include <lexiglance/dictionary/WordRules.h>
#include <lexiglance/language/ja/Furigana.h>
#include <lexiglance/language/ja/Kana.h>

#include <algorithm>
#include <array>
#include <string>

namespace lexiglance::lang::ja
{

	namespace
	{

		using namespace dict::rule;

		// Intermediate conditions (bits 24-31 are private to the deinflector).
		constexpr std::uint32_t masu       = 1U << 24U;
		constexpr std::uint32_t masen      = 1U << 25U;
		constexpr std::uint32_t te         = 1U << 26U;
		constexpr std::uint32_t final_only = 1U << 31U;

		struct GodanRow
		{
			std::string_view u;
			std::string_view i;
			std::string_view a;
			std::string_view e;
			std::string_view o;
			std::string_view te;
			std::string_view ta;
		};

		constexpr std::array<GodanRow, 9> godan{ {
				{ .u = "う", .i = "い", .a = "わ", .e = "え", .o = "お", .te = "って", .ta = "った" },
				{ .u = "く", .i = "き", .a = "か", .e = "け", .o = "こ", .te = "いて", .ta = "いた" },
				{ .u = "ぐ", .i = "ぎ", .a = "が", .e = "げ", .o = "ご", .te = "いで", .ta = "いだ" },
				{ .u = "す", .i = "し", .a = "さ", .e = "せ", .o = "そ", .te = "して", .ta = "した" },
				{ .u = "つ", .i = "ち", .a = "た", .e = "て", .o = "と", .te = "って", .ta = "った" },
				{ .u = "ぬ", .i = "に", .a = "な", .e = "ね", .o = "の", .te = "んで", .ta = "んだ" },
				{ .u = "ぶ", .i = "び", .a = "ば", .e = "べ", .o = "ぼ", .te = "んで", .ta = "んだ" },
				{ .u = "む", .i = "み", .a = "ま", .e = "め", .o = "も", .te = "んで", .ta = "んだ" },
				{ .u = "る", .i = "り", .a = "ら", .e = "れ", .o = "ろ", .te = "って", .ta = "った" },
		} };

		std::string cat( std::string_view a, std::string_view b )
		{
			std::string out( a );
			out.append( b );
			return out;
		}

		struct Form
		{
			std::string   inflected;
			std::string   dictionary;
			std::uint32_t conditions = 0;
		};

		// Past (た) forms. Te forms, -tara, -tari and -chau are derived from these.
		std::vector<Form> pastForms()
		{
			std::vector<Form> forms{ { "た", "る", v1 } };
			for ( const GodanRow& row : godan )
			{
				forms.push_back( { std::string( row.ta ), std::string( row.u ), v5 } );
			}
			forms.push_back( { "行った", "行く", v5 } );
			forms.push_back( { "いった", "いく", v5 } );
			forms.push_back( { "きた", "くる", vk } );
			forms.push_back( { "来た", "来る", vk } );
			forms.push_back( { "した", "する", vs } );
			forms.push_back( { "じた", "ずる", vz } );
			return forms;
		}

		// Replaces the trailing た/だ of a past form.
		std::string rebase( std::string_view past, std::string_view instead_of_ta, std::string_view instead_of_da )
		{
			const bool da = past.ends_with( "だ" );
			return cat( past.substr( 0, past.size() - std::string_view( "た" ).size() ), da ? instead_of_da : instead_of_ta );
		}

		void buildRules( Deinflector& d )
		{
			const auto add   = [&d]( std::string_view in, std::string_view out, std::uint32_t cin, std::uint32_t cout, std::uint16_t t ) { d.addRule( in, out, cin, cout, t ); };
			const auto verbs = pastForms();

			// Polite
			const auto polite = d.addTransform( "polite", "polite conjugation (~ます)" );
			add( "ます", "る", masu, v1, polite );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.i, "ます" ), row.u, masu, v5, polite );
			}
			add( "きます", "くる", masu, vk, polite );
			add( "来ます", "来る", masu, vk, polite );
			add( "します", "する", masu, vs, polite );
			add( "じます", "ずる", masu, vz, polite );
			add( "ございます", "ござる", masu, v5, polite );
			add( "さいます", "さる", masu, v5, polite );
			add( "ゃいます", "ゃる", masu, v5, polite );

			// Negative
			const auto negative = d.addTransform( "negative", "negative (~ない)" );
			add( "ない", "る", adj_i, v1, negative );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.a, "ない" ), row.u, adj_i, v5, negative );
			}
			add( "こない", "くる", adj_i, vk, negative );
			add( "来ない", "来る", adj_i, vk, negative );
			add( "しない", "する", adj_i, vs, negative );
			add( "じない", "ずる", adj_i, vz, negative );
			add( "くない", "い", adj_i, adj_i, negative );
			add( "ません", "ます", masen | final_only, masu, negative );

			// Past
			const auto past = d.addTransform( "past", "past tense (~た)" );
			for ( const Form& form : verbs )
			{
				add( form.inflected, form.dictionary, final_only, form.conditions, past );
			}
			add( "かった", "い", final_only, adj_i, past );
			add( "ました", "ます", final_only, masu, past );
			add( "ませんでした", "ません", final_only, masen, past );

			// Te form
			const auto te_form = d.addTransform( "-te", "te form (~て)" );
			for ( const Form& form : verbs )
			{
				add( rebase( form.inflected, "て", "で" ), form.dictionary, te, form.conditions, te_form );
			}
			add( "くて", "い", te, adj_i, te_form );
			add( "ないで", "ない", te, adj_i, te_form );
			add( "まして", "ます", te, masu, te_form );

			const auto teiru = d.addTransform( "-teiru", "progressive or perfect (~ている)" );
			add( "ている", "て", v1, te, teiru );
			add( "でいる", "で", v1, te, teiru );
			add( "てる", "て", v1, te, teiru );
			add( "でる", "で", v1, te, teiru );
			add( "ておる", "て", v5, te, teiru );

			const auto teoku = d.addTransform( "-teoku", "doing in advance (~ておく)" );
			add( "ておく", "て", v5, te, teoku );
			add( "でおく", "で", v5, te, teoku );
			add( "とく", "て", v5, te, teoku );
			add( "どく", "で", v5, te, teoku );

			const auto shimau = d.addTransform( "-shimau", "completion or regret (~てしまう)" );
			add( "てしまう", "て", v5, te, shimau );
			add( "でしまう", "で", v5, te, shimau );

			const auto chau = d.addTransform( "-chau", "contracted ~てしまう" );
			for ( const Form& form : verbs )
			{
				add( rebase( form.inflected, "ちゃう", "じゃう" ), form.dictionary, v5, form.conditions, chau );
			}

			// Conditionals
			const auto tara = d.addTransform( "-tara", "conditional (~たら)" );
			const auto tari = d.addTransform( "-tari", "listing actions (~たり)" );
			for ( const Form& form : verbs )
			{
				add( cat( form.inflected, "ら" ), form.dictionary, final_only, form.conditions, tara );
				add( cat( form.inflected, "り" ), form.dictionary, final_only, form.conditions, tari );
			}
			add( "かったら", "い", final_only, adj_i, tara );
			add( "かったり", "い", final_only, adj_i, tari );

			const auto ba = d.addTransform( "-ba", "conditional (~ば)" );
			add( "れば", "る", final_only, v1, ba );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.e, "ば" ), row.u, final_only, v5, ba );
			}
			add( "くれば", "くる", final_only, vk, ba );
			add( "来れば", "来る", final_only, vk, ba );
			add( "すれば", "する", final_only, vs, ba );
			add( "ずれば", "ずる", final_only, vz, ba );
			add( "ければ", "い", final_only, adj_i, ba );

			const auto kya = d.addTransform( "-kya", "colloquial ~なければ" );
			add( "なきゃ", "ない", final_only, adj_i, kya );
			add( "なくちゃ", "ない", final_only, adj_i, kya );

			// Voice
			const auto potential = d.addTransform( "potential", "potential (can do)" );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.e, "る" ), row.u, v1, v5, potential );
			}
			add( "れる", "る", v1, v1, potential );
			add( "これる", "くる", v1, vk, potential );
			add( "来れる", "来る", v1, vk, potential );

			const auto passive = d.addTransform( "passive", "passive" );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.a, "れる" ), row.u, v1, v5, passive );
			}
			add( "される", "する", v1, vs, passive );
			add( "こられる", "くる", v1, vk, passive );
			add( "来られる", "来る", v1, vk, passive );

			const auto potential_passive = d.addTransform( "potential or passive", "potential or passive (~られる)" );
			add( "られる", "る", v1, v1, potential_passive );

			const auto causative = d.addTransform( "causative", "causative (make/let do)" );
			add( "させる", "る", v1, v1, causative );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.a, "せる" ), row.u, v1, v5, causative );
				add( cat( row.a, "す" ), row.u, v5, v5, causative );
			}
			add( "させる", "する", v1, vs, causative );
			add( "こさせる", "くる", v1, vk, causative );
			add( "来させる", "来る", v1, vk, causative );

			// Mood
			const auto volitional = d.addTransform( "volitional", "volitional (let's ~)" );
			add( "よう", "る", final_only, v1, volitional );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.o, "う" ), row.u, final_only, v5, volitional );
			}
			add( "こよう", "くる", final_only, vk, volitional );
			add( "来よう", "来る", final_only, vk, volitional );
			add( "しよう", "する", final_only, vs, volitional );
			add( "じよう", "ずる", final_only, vz, volitional );
			add( "かろう", "い", final_only, adj_i, volitional );
			add( "ましょう", "ます", final_only, masu, volitional );

			const auto imperative = d.addTransform( "imperative", "imperative" );
			add( "ろ", "る", final_only, v1, imperative );
			add( "よ", "る", final_only, v1, imperative );
			for ( const GodanRow& row : godan )
			{
				add( row.e, row.u, final_only, v5, imperative );
			}
			add( "こい", "くる", final_only, vk, imperative );
			add( "来い", "来る", final_only, vk, imperative );
			add( "しろ", "する", final_only, vs, imperative );
			add( "せよ", "する", final_only, vs, imperative );
			add( "じろ", "ずる", final_only, vz, imperative );
			add( "ぜよ", "ずる", final_only, vz, imperative );
			add( "くれ", "くれる", final_only, v1, imperative );
			add( "ください", "くださる", final_only, v5, imperative );
			add( "なさい", "なさる", final_only, v5, imperative );
			add( "らっしゃい", "らっしゃる", final_only, v5, imperative );
			add( "っしゃい", "っしゃる", final_only, v5, imperative );
			add( "ませ", "ます", final_only, masu, imperative );

			const auto prohibitive = d.addTransform( "imperative negative", "prohibitive (don't ~)" );
			add( "るな", "る", final_only, v1, prohibitive );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.u, "な" ), row.u, final_only, v5, prohibitive );
			}
			add( "くるな", "くる", final_only, vk, prohibitive );
			add( "するな", "する", final_only, vs, prohibitive );

			const auto tai = d.addTransform( "-tai", "want to (~たい)" );
			add( "たい", "る", adj_i, v1, tai );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.i, "たい" ), row.u, adj_i, v5, tai );
			}
			add( "きたい", "くる", adj_i, vk, tai );
			add( "来たい", "来る", adj_i, vk, tai );
			add( "したい", "する", adj_i, vs, tai );

			const auto tagaru = d.addTransform( "-tagaru", "shows signs of wanting (~たがる)" );
			add( "たがる", "る", v5, v1, tagaru );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.i, "たがる" ), row.u, v5, v5, tagaru );
			}

			const auto sou = d.addTransform( "-sou", "appearing (~そう)" );
			add( "そう", "い", final_only, adj_i, sou );
			add( "さそう", "い", final_only, adj_i, sou );
			add( "そう", "る", final_only, v1, sou );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.i, "そう" ), row.u, final_only, v5, sou );
			}
			add( "しそう", "する", final_only, vs, sou );
			add( "きそう", "くる", final_only, vk, sou );

			const auto sugiru = d.addTransform( "-sugiru", "too much (~すぎる)" );
			for ( const std::string_view suffix : { std::string_view( "すぎる" ), std::string_view( "過ぎる" ) } )
			{
				add( suffix, "い", v1, adj_i, sugiru );
				add( cat( "さ", suffix ), "い", v1, adj_i, sugiru );
				add( suffix, "る", v1, v1, sugiru );
				for ( const GodanRow& row : godan )
				{
					add( cat( row.i, suffix ), row.u, v1, v5, sugiru );
				}
				add( cat( "し", suffix ), "する", v1, vs, sugiru );
			}

			const auto nasai = d.addTransform( "-nasai", "polite command (~なさい)" );
			add( "なさい", "る", final_only, v1, nasai );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.i, "なさい" ), row.u, final_only, v5, nasai );
			}
			add( "きなさい", "くる", final_only, vk, nasai );
			add( "しなさい", "する", final_only, vs, nasai );

			const auto masu_stem = d.addTransform( "masu stem", "continuative (masu stem)" );
			for ( const GodanRow& row : godan )
			{
				add( row.i, row.u, final_only, v5, masu_stem );
			}
			add( "し", "する", final_only, vs, masu_stem );

			// Classical and adjectival forms
			const auto zu = d.addTransform( "-zu", "negative (~ず)" );
			const auto nu = d.addTransform( "-nu", "negative (~ぬ)" );
			add( "ず", "る", final_only, v1, zu );
			add( "ぬ", "る", final_only, v1, nu );
			for ( const GodanRow& row : godan )
			{
				add( cat( row.a, "ず" ), row.u, final_only, v5, zu );
				add( cat( row.a, "ぬ" ), row.u, final_only, v5, nu );
			}
			add( "せず", "する", final_only, vs, zu );
			add( "せぬ", "する", final_only, vs, nu );
			add( "こず", "くる", final_only, vk, zu );
			add( "こぬ", "くる", final_only, vk, nu );

			const auto n = d.addTransform( "-n", "colloquial negative (~ん)" );
			add( "ん", "ない", final_only, adj_i, n );

			const auto adverb = d.addTransform( "adv", "adverbial (~く)" );
			add( "く", "い", final_only, adj_i, adverb );

			const auto noun = d.addTransform( "noun", "nominalised (~さ)" );
			add( "さ", "い", final_only, adj_i, noun );

			const auto ki = d.addTransform( "-ki", "attributive (~き)" );
			add( "き", "い", final_only, adj_i, ki );

			const auto ge = d.addTransform( "-ge", "appearance (~げ)" );
			add( "げ", "い", final_only, adj_i, ge );

			const auto suru = d.addTransform( "suru", "noun + する" );
			add( "する", "", vs, vs, suru );
		}

		// U+FF61..U+FF9F to their full width forms.
		constexpr std::array<char32_t, 63> halfwidth{
			0x3002,
			0x300C,
			0x300D,
			0x3001,
			0x30FB,
			0x30F2,
			0x30A1,
			0x30A3,
			0x30A5,
			0x30A7,
			0x30A9,
			0x30E3,
			0x30E5,
			0x30E7,
			0x30C3,
			0x30FC,
			0x30A2,
			0x30A4,
			0x30A6,
			0x30A8,
			0x30AA,
			0x30AB,
			0x30AD,
			0x30AF,
			0x30B1,
			0x30B3,
			0x30B5,
			0x30B7,
			0x30B9,
			0x30BB,
			0x30BD,
			0x30BF,
			0x30C1,
			0x30C4,
			0x30C6,
			0x30C8,
			0x30CA,
			0x30CB,
			0x30CC,
			0x30CD,
			0x30CE,
			0x30CF,
			0x30D2,
			0x30D5,
			0x30D8,
			0x30DB,
			0x30DE,
			0x30DF,
			0x30E0,
			0x30E1,
			0x30E2,
			0x30E4,
			0x30E6,
			0x30E8,
			0x30E9,
			0x30EA,
			0x30EB,
			0x30EC,
			0x30ED,
			0x30EF,
			0x30F3,
			0x309B,
			0x309C,
		};

		Units normalizeWidth( std::u32string_view source )
		{
			Units out;
			out.reserve( source.size() );
			for ( std::size_t i = 0; i < source.size(); ++i )
			{
				char32_t   c   = source[i];
				const auto end = static_cast<std::uint32_t>( i + 1 );
				if ( c >= 0xFF61 && c <= 0xFF9F )
				{
					c = halfwidth[c - 0xFF61];
				}
				if ( ( c == 0x3099 || c == 0x309A || c == 0x309B || c == 0x309C ) && !out.empty() )
				{
					const bool semi = c == 0x309A || c == 0x309C;
					if ( const char32_t combined = combineMark( out.back().first, semi ); combined != 0 )
					{
						out.back() = { combined, end };
						continue;
					}
				}
				out.emplace_back( c, end );
			}
			return out;
		}

		bool hasEmphatic( const Units& units )
		{
			for ( std::size_t i = 1; i < units.size(); ++i )
			{
				const char32_t c = units[i].first;
				if ( c == units[i - 1].first && ( c == U'っ' || c == U'ッ' || c == U'ー' ) )
				{
					return true;
				}
			}
			return false;
		}

		Units collapseEmphatic( const Units& in )
		{
			Units out;
			out.reserve( in.size() );
			for ( const auto& unit : in )
			{
				const char32_t c = unit.first;
				if ( !out.empty() && out.back().first == c && ( c == U'っ' || c == U'ッ' || c == U'ー' ) )
				{
					out.back().second = unit.second;
					continue;
				}
				out.push_back( unit );
			}
			return out;
		}

	} // namespace

	std::string toHiragana( std::string_view text )
	{
		std::string out;
		out.reserve( text.size() );
		for ( const char32_t c : utf8::codepoints( text ) )
		{
			utf8::append( out, toHiragana( c ) );
		}
		return out;
	}

	std::string toKatakana( std::string_view text )
	{
		std::string out;
		out.reserve( text.size() );
		for ( const char32_t c : utf8::codepoints( text ) )
		{
			utf8::append( out, toKatakana( c ) );
		}
		return out;
	}

	Japanese::Japanese()
	{
		buildRules( deinflector_ );
		deinflector_.finalize();
	}

	bool Japanese::isScriptCharacter( char32_t c ) const noexcept
	{
		return ( c >= 0x3040 && c <= 0x30FF ) || isKatakana( c ) || isKanji( c ) || c == U'「' || c == U'『' || c == U'（';
	}

	bool Japanese::isLookupCharacter( char32_t c ) const noexcept
	{
		return isKana( c ) || isKanji( c ) || isFullwidthAlphanumeric( c ) || isAsciiAlphanumeric( c ) || ( c >= 0x3099 && c <= 0x309C );
	}

	std::vector<RubySegment> Japanese::headword( std::string_view expression, std::string_view reading ) const
	{
		return distributeFurigana( expression, reading );
	}

	std::span<const std::string_view> Japanese::sampleWords() const noexcept
	{
		static constexpr std::array<std::string_view, 2> words{ "食べる", "日本語" };
		return words;
	}

	void Japanese::variants( std::u32string_view source, std::vector<TextVariant>& out ) const
	{
		const Units base = normalizeWidth( source );

		std::vector<Units> candidates;
		candidates.push_back( base );
		candidates.push_back( mapUnits( base, []( char32_t c ) { return toHiragana( c ); } ) );
		candidates.push_back( mapUnits( base, []( char32_t c ) { return toKatakana( c ); } ) );

		const bool alphanumeric = std::ranges::any_of( base, []( const auto& unit ) { return isAsciiAlphanumeric( unit.first ) || isFullwidthAlphanumeric( unit.first ); } );
		if ( alphanumeric )
		{
			candidates.push_back( mapUnits( base, []( char32_t c ) { return isAsciiAlphanumeric( c ) ? c + 0xFEE0 : c; } ) );
			candidates.push_back( mapUnits( base, []( char32_t c ) { return isFullwidthAlphanumeric( c ) ? c - 0xFEE0 : c; } ) );
		}

		const std::size_t plain = candidates.size();
		for ( std::size_t i = 0; i < plain; ++i )
		{
			if ( hasEmphatic( candidates[i] ) )
			{
				candidates.push_back( collapseEmphatic( candidates[i] ) );
			}
		}

		for ( const Units& units : candidates )
		{
			addVariant( out, units );
		}
	}

} // namespace lexiglance::lang::ja
