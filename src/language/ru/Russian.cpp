#include "Russian.h"

#include <lexiglance/core/Utf8.h>
#include <lexiglance/dictionary/WordRules.h>

#include <array>
#include <string>

namespace lexiglance::lang::ru
{

	namespace
	{

		namespace rule = dict::rule;

		// Intermediate condition (bits 24-31 are the deinflector's): rules for the text as written, not for a form another
		// rule produced.
		constexpr std::uint32_t surface = 1U << 31U;

		constexpr std::array<std::string_view, 20> consonants{ "б", "в", "г", "д", "ж", "з", "к", "л", "м", "н", "п", "р", "с", "т", "ф", "х", "ц", "ч", "ш", "щ" };

		bool velarOrSibilant( std::string_view consonant )
		{
			return consonant == "г" || consonant == "к" || consonant == "х" || consonant == "ж" || consonant == "ш" || consonant == "ч" || consonant == "щ";
		}

		// Reflexive verbs add -ся after a consonant and -сь after a vowel: учусь, учится, учиться.
		std::string reflexive( std::string_view text )
		{
			const char32_t last  = utf8::last( text );
			const bool     vowel = std::u32string_view( U"аеёиоуыэюя" ).contains( last );
			return std::string( text ).append( vowel ? "сь" : "ся" );
		}

		struct Rules
		{
			Deinflector* d;

			void add( std::string_view inflected, std::string_view dictionary, std::uint32_t in, std::uint32_t out, std::uint16_t transform ) const
			{
				d->addRule( inflected, dictionary, in, out, transform );
			}

			void noun( std::string_view inflected, std::string_view dictionary, std::uint16_t transform ) const
			{
				add( inflected, dictionary, surface, rule::noun, transform );
			}

			void adjective( std::string_view inflected, std::string_view dictionary, std::uint16_t transform, std::uint32_t in = surface ) const
			{
				add( inflected, dictionary, in, rule::adjective, transform );
			}

			// A verb form and, unless `plain_only`, its reflexive twin (читаю -> читать, учусь -> учиться).
			void verb( std::string_view inflected, std::string_view dictionary, std::uint16_t transform, std::uint32_t in = surface, bool plain_only = false ) const
			{
				add( inflected, dictionary, in, rule::verb, transform );
				if ( !plain_only )
				{
					add( reflexive( inflected ), reflexive( dictionary ), in, rule::verb, transform );
				}
			}
		};

		void nounRules( const Rules& r )
		{
			Deinflector& d             = *r.d;
			const auto   gen_sg        = d.addTransform( "genitive singular", "genitive singular" );
			const auto   dat_sg        = d.addTransform( "dative singular", "dative singular" );
			const auto   acc_sg        = d.addTransform( "accusative singular", "accusative singular" );
			const auto   ins_sg        = d.addTransform( "instrumental singular", "instrumental singular" );
			const auto   prep_sg       = d.addTransform( "prepositional singular", "prepositional singular" );
			const auto   dat_prep_sg   = d.addTransform( "dative/prepositional singular", "dative or prepositional singular" );
			const auto   oblique_sg    = d.addTransform( "genitive/dative/prepositional singular", "genitive, dative or prepositional singular" );
			const auto   gen_sg_pl     = d.addTransform( "genitive singular or nominative plural", "genitive singular or nominative plural" );
			const auto   oblique_or_pl = d.addTransform( "genitive/dative/prepositional singular or nominative plural", "genitive, dative or prepositional singular, or nominative plural" );
			const auto   nom_pl        = d.addTransform( "nominative plural", "nominative plural" );
			const auto   gen_pl        = d.addTransform( "genitive plural", "genitive plural" );
			const auto   dat_pl        = d.addTransform( "dative plural", "dative plural" );
			const auto   ins_pl        = d.addTransform( "instrumental plural", "instrumental plural" );
			const auto   prep_pl       = d.addTransform( "prepositional plural", "prepositional plural" );

			// Masculine, hard stem: стол.
			r.noun( "а", "", gen_sg );
			r.noun( "у", "", dat_sg );
			r.noun( "ом", "", ins_sg );
			r.noun( "ем", "", ins_sg );
			r.noun( "е", "", prep_sg );
			r.noun( "ы", "", nom_pl );
			r.noun( "и", "", nom_pl );
			r.noun( "ов", "", gen_pl );
			r.noun( "ев", "", gen_pl );
			r.noun( "ей", "", gen_pl );
			r.noun( "ам", "", dat_pl );
			r.noun( "ами", "", ins_pl );
			r.noun( "ах", "", prep_pl );

			// Masculine with a vowel that drops out: кусок -> куска, отец -> отца.
			for ( const auto& [stem, word] : { std::pair{ std::string_view( "к" ), std::string_view( "ок" ) }, std::pair{ std::string_view( "к" ), std::string_view( "ек" ) }, std::pair{ std::string_view( "ц" ), std::string_view( "ец" ) } } )
			{
				const std::string s( stem );
				r.noun( s + "а", word, gen_sg );
				r.noun( s + "у", word, dat_sg );
				r.noun( s + "ом", word, ins_sg );
				r.noun( s + "ем", word, ins_sg );
				r.noun( s + "е", word, prep_sg );
				r.noun( s + ( stem == "ц" ? "ы" : "и" ), word, nom_pl );
				r.noun( s + "ов", word, gen_pl );
				r.noun( s + "ев", word, gen_pl );
				r.noun( s + "ам", word, dat_pl );
				r.noun( s + "ами", word, ins_pl );
				r.noun( s + "ах", word, prep_pl );
			}

			// Soft stem in -ь, masculine (конь) and feminine (ночь).
			r.noun( "я", "ь", gen_sg );
			r.noun( "ю", "ь", dat_sg );
			r.noun( "ем", "ь", ins_sg );
			r.noun( "ью", "ь", ins_sg );
			r.noun( "е", "ь", prep_sg );
			r.noun( "и", "ь", oblique_or_pl );
			r.noun( "ей", "ь", gen_pl );
			r.noun( "ям", "ь", dat_pl );
			r.noun( "ями", "ь", ins_pl );
			r.noun( "ях", "ь", prep_pl );
			r.noun( "ам", "ь", dat_pl );
			r.noun( "ами", "ь", ins_pl );
			r.noun( "ах", "ь", prep_pl );

			// Masculine in -й (музей, also -ий: гений).
			r.noun( "я", "й", gen_sg );
			r.noun( "ю", "й", dat_sg );
			r.noun( "ем", "й", ins_sg );
			r.noun( "е", "й", prep_sg );
			r.noun( "ии", "ий", prep_sg );
			r.noun( "и", "й", nom_pl );
			r.noun( "ев", "й", gen_pl );
			r.noun( "ям", "й", dat_pl );
			r.noun( "ями", "й", ins_pl );
			r.noun( "ях", "й", prep_pl );

			// Feminine (and some masculine) in -а: книга, папа.
			r.noun( "ы", "а", gen_sg_pl );
			r.noun( "и", "а", gen_sg_pl );
			r.noun( "е", "а", dat_prep_sg );
			r.noun( "у", "а", acc_sg );
			r.noun( "ой", "а", ins_sg );
			r.noun( "ою", "а", ins_sg );
			r.noun( "ей", "а", ins_sg );
			r.noun( "ам", "а", dat_pl );
			r.noun( "ами", "а", ins_pl );
			r.noun( "ах", "а", prep_pl );
			r.noun( "ок", "ка", gen_pl );
			r.noun( "ек", "ка", gen_pl );

			// Feminine in -я: неделя, and -ия: армия.
			r.noun( "и", "я", gen_sg_pl );
			r.noun( "е", "я", dat_prep_sg );
			r.noun( "ю", "я", acc_sg );
			r.noun( "ей", "я", ins_sg );
			r.noun( "ею", "я", ins_sg );
			r.noun( "ям", "я", dat_pl );
			r.noun( "ями", "я", ins_pl );
			r.noun( "ях", "я", prep_pl );
			r.noun( "ь", "я", gen_pl );
			r.noun( "ии", "ия", oblique_sg );
			r.noun( "ий", "ия", gen_pl );

			// Neuter in -о: место.
			r.noun( "а", "о", gen_sg_pl );
			r.noun( "у", "о", dat_sg );
			r.noun( "ом", "о", ins_sg );
			r.noun( "е", "о", prep_sg );
			r.noun( "ам", "о", dat_pl );
			r.noun( "ами", "о", ins_pl );
			r.noun( "ах", "о", prep_pl );

			// Neuter in -е: море, and -ие: здание.
			r.noun( "я", "е", gen_sg_pl );
			r.noun( "ю", "е", dat_sg );
			r.noun( "ем", "е", ins_sg );
			r.noun( "ей", "е", gen_pl );
			r.noun( "ям", "е", dat_pl );
			r.noun( "ями", "е", ins_pl );
			r.noun( "ях", "е", prep_pl );
			r.noun( "ии", "ие", prep_sg );
			r.noun( "ий", "ие", gen_pl );

			// Neuter in -мя: время.
			r.noun( "мени", "мя", oblique_sg );
			r.noun( "менем", "мя", ins_sg );
			r.noun( "мена", "мя", nom_pl );
			r.noun( "мен", "мя", gen_pl );
			r.noun( "менам", "мя", dat_pl );
			r.noun( "менами", "мя", ins_pl );
			r.noun( "менах", "мя", prep_pl );

			// Genitive plural without an ending: книг -> книга, мест -> место.
			for ( const std::string_view consonant : consonants )
			{
				r.noun( consonant, std::string( consonant ) + "а", gen_pl );
				r.noun( consonant, std::string( consonant ) + "о", gen_pl );
			}
		}

		void adjectiveRules( const Rules& r )
		{
			Deinflector& d               = *r.d;
			const auto   gen_sg          = d.addTransform( "genitive singular", "masculine or neuter genitive singular" );
			const auto   dat_sg          = d.addTransform( "dative singular", "masculine or neuter dative singular" );
			const auto   ins_sg          = d.addTransform( "instrumental singular or dative plural", "masculine or neuter instrumental singular, or dative plural" );
			const auto   prep_sg         = d.addTransform( "prepositional singular", "masculine or neuter prepositional singular" );
			const auto   feminine        = d.addTransform( "feminine", "feminine nominative singular" );
			const auto   feminine_other  = d.addTransform( "feminine genitive/dative/instrumental/prepositional", "feminine singular in a case other than the nominative or accusative" );
			const auto   feminine_acc    = d.addTransform( "feminine accusative", "feminine accusative singular" );
			const auto   neuter          = d.addTransform( "neuter", "neuter nominative or accusative singular" );
			const auto   plural          = d.addTransform( "plural", "nominative or accusative plural" );
			const auto   gen_prep_pl     = d.addTransform( "genitive/prepositional plural", "genitive or prepositional plural" );
			const auto   ins_pl          = d.addTransform( "instrumental plural", "instrumental plural" );
			const auto   short_form      = d.addTransform( "short form", "short form" );
			const auto   short_or_adverb = d.addTransform( "short form or adverb", "neuter short form, or the adverb" );
			const auto   comparative     = d.addTransform( "comparative", "comparative (more ...)" );
			const auto   superlative     = d.addTransform( "superlative", "superlative (most ...)" );

			struct Ending
			{
				std::string_view text;
				std::uint16_t    transform;
			};
			const std::array<Ending, 12> hard{ {
					{ .text = "ого", .transform = gen_sg },
					{ .text = "ому", .transform = dat_sg },
					{ .text = "ым", .transform = ins_sg },
					{ .text = "ом", .transform = prep_sg },
					{ .text = "ая", .transform = feminine },
					{ .text = "ой", .transform = feminine_other },
					{ .text = "ою", .transform = feminine_other },
					{ .text = "ую", .transform = feminine_acc },
					{ .text = "ое", .transform = neuter },
					{ .text = "ые", .transform = plural },
					{ .text = "ых", .transform = gen_prep_pl },
					{ .text = "ыми", .transform = ins_pl },
			} };
			const std::array<Ending, 12> soft{ {
					{ .text = "его", .transform = gen_sg },
					{ .text = "ему", .transform = dat_sg },
					{ .text = "им", .transform = ins_sg },
					{ .text = "ем", .transform = prep_sg },
					{ .text = "яя", .transform = feminine },
					{ .text = "ей", .transform = feminine_other },
					{ .text = "ею", .transform = feminine_other },
					{ .text = "юю", .transform = feminine_acc },
					{ .text = "ее", .transform = neuter },
					{ .text = "ие", .transform = plural },
					{ .text = "их", .transform = gen_prep_pl },
					{ .text = "ими", .transform = ins_pl },
			} };
			// After г, к, х, ж, ш, ч and щ the endings mix both sets: русский, русского, русские; хороший, хорошего.
			const std::array<Ending, 8> mixed{ {
					{ .text = "ого", .transform = gen_sg },
					{ .text = "ому", .transform = dat_sg },
					{ .text = "ом", .transform = prep_sg },
					{ .text = "ая", .transform = feminine },
					{ .text = "ой", .transform = feminine_other },
					{ .text = "ою", .transform = feminine_other },
					{ .text = "ую", .transform = feminine_acc },
					{ .text = "ое", .transform = neuter },
			} };

			for ( const Ending& ending : hard )
			{
				r.adjective( ending.text, "ый", ending.transform );
				if ( ending.text != "ой" )
				{
					r.adjective( ending.text, "ой", ending.transform );
				}
			}
			for ( const Ending& ending : soft )
			{
				r.adjective( ending.text, "ий", ending.transform );
			}
			for ( const Ending& ending : mixed )
			{
				r.adjective( ending.text, "ий", ending.transform );
			}
			// Stressed -ой after г, к, х and sibilants: большой, большие.
			for ( const std::string_view ending : { "им", "ие", "их", "ими" } )
			{
				const auto* const it = std::ranges::find( soft, ending, &Ending::text );
				r.adjective( ending, "ой", it->transform );
			}
			// Reflexive participles decline like хороший with -ся: читающегося -> читающийся.
			for ( const Ending& ending : soft )
			{
				if ( ending.text != "ею" )
				{
					r.adjective( reflexive( ending.text ), "ийся", ending.transform );
				}
			}
			r.adjective( "аяся", "ийся", feminine );
			r.adjective( "уюся", "ийся", feminine_acc );

			// Short forms: красива, красиво, красивы, красив; труден (трудный), короток (короткий).
			for ( const std::string_view lemma : { "ый", "ий", "ой" } )
			{
				r.adjective( "а", lemma, short_form );
				r.adjective( "о", lemma, short_or_adverb );
			}
			r.adjective( "ы", "ый", short_form );
			r.adjective( "ы", "ой", short_form );
			r.adjective( "и", "ий", short_form );
			for ( const std::string_view consonant : consonants )
			{
				r.adjective( consonant, std::string( consonant ) + ( velarOrSibilant( consonant ) ? "ий" : "ый" ), short_form );
			}
			r.adjective( "ен", "ный", short_form );
			r.adjective( "ок", "кий", short_form );
			r.adjective( "ек", "кий", short_form );
			// Short passive participles: прочитан, прочитана -> прочитанный.
			for ( const std::string_view ending : { "н", "на", "но", "ны" } )
			{
				r.adjective( ending, "нный", short_form );
			}

			r.adjective( "ее", "ый", comparative );
			r.adjective( "ее", "ий", comparative );
			r.adjective( "ей", "ый", comparative );
			// Superlatives decline, so they also follow the declension rules: быстрейшего -> быстрейший -> быстрый.
			r.adjective( "ейший", "ый", superlative, surface | rule::adjective );
			r.adjective( "айший", "ий", superlative, surface | rule::adjective );
			r.adjective( "чайший", "кий", superlative, surface | rule::adjective );
			r.adjective( "жайший", "гий", superlative, surface | rule::adjective );
			r.adjective( "шайший", "хий", superlative, surface | rule::adjective );
		}

		void verbRules( const Rules& r )
		{
			Deinflector& d          = *r.d;
			const auto   first_sg   = d.addTransform( "first person singular", "first person singular (я), present or future" );
			const auto   second_sg  = d.addTransform( "second person singular", "second person singular (ты), present or future" );
			const auto   third_sg   = d.addTransform( "third person singular", "third person singular (он, она, оно), present or future" );
			const auto   first_pl   = d.addTransform( "first person plural", "first person plural (мы), present or future" );
			const auto   second_pl  = d.addTransform( "second person plural", "second person plural (вы), present or future" );
			const auto   third_pl   = d.addTransform( "third person plural", "third person plural (они), present or future" );
			const auto   past       = d.addTransform( "past", "past tense" );
			const auto   imperative = d.addTransform( "imperative", "imperative" );
			const auto   gerund     = d.addTransform( "gerund", "adverbial participle (деепричастие)" );
			const auto   pres_act   = d.addTransform( "present active participle", "present active participle" );
			const auto   past_act   = d.addTransform( "past active participle", "past active participle" );
			const auto   pres_pass  = d.addTransform( "present passive participle", "present passive participle" );
			const auto   past_pass  = d.addTransform( "past passive participle", "past passive participle" );

			struct Person
			{
				std::string_view first_sg;
				std::string_view second_sg;
				std::string_view third_sg;
				std::string_view first_pl;
				std::string_view second_pl;
				std::string_view third_pl;
			};
			// Present (or perfective future) endings and the infinitive ending they come from.
			const auto present = [&]( const Person& p, std::string_view infinitive ) {
				r.verb( p.first_sg, infinitive, first_sg );
				r.verb( p.second_sg, infinitive, second_sg );
				r.verb( p.third_sg, infinitive, third_sg );
				r.verb( p.first_pl, infinitive, first_pl );
				r.verb( p.second_pl, infinitive, second_pl );
				r.verb( p.third_pl, infinitive, third_pl );
			};

			// First conjugation on a vowel stem: читать, гулять, уметь.
			present( { .first_sg = "ю", .second_sg = "ешь", .third_sg = "ет", .first_pl = "ем", .second_pl = "ете", .third_pl = "ют" }, "ть" );
			// Second conjugation: говорить, смотреть, стоять; держать and слышать after a sibilant.
			for ( const std::string_view infinitive : { "ить", "еть", "ять" } )
			{
				present( { .first_sg = "ю", .second_sg = "ишь", .third_sg = "ит", .first_pl = "им", .second_pl = "ите", .third_pl = "ят" }, infinitive );
			}
			present( { .first_sg = "у", .second_sg = "ишь", .third_sg = "ит", .first_pl = "им", .second_pl = "ите", .third_pl = "ат" }, "ать" );
			r.verb( "у", "ить", first_sg );
			r.verb( "ат", "ить", third_pl );
			// Consonants that change in the first person: люблю, хожу, вожу, плачу, пущу, прошу; сижу, лечу, вишу.
			r.verb( "лю", "ить", first_sg );
			r.verb( "лю", "еть", first_sg );
			r.verb( "жу", "дить", first_sg );
			r.verb( "жу", "зить", first_sg );
			r.verb( "жу", "деть", first_sg );
			r.verb( "чу", "тить", first_sg );
			r.verb( "чу", "теть", first_sg );
			r.verb( "щу", "стить", first_sg );
			r.verb( "шу", "сить", first_sg );
			r.verb( "шу", "сеть", first_sg );
			// рисовать -> рисую, воевать -> воюю, давать -> даю, крикнуть -> крикну, нести -> несу.
			present( { .first_sg = "ую", .second_sg = "уешь", .third_sg = "ует", .first_pl = "уем", .second_pl = "уете", .third_pl = "уют" }, "овать" );
			present( { .first_sg = "юю", .second_sg = "юешь", .third_sg = "юет", .first_pl = "юем", .second_pl = "юете", .third_pl = "юют" }, "евать" );
			present( { .first_sg = "аю", .second_sg = "аешь", .third_sg = "ает", .first_pl = "аем", .second_pl = "аете", .third_pl = "ают" }, "авать" );
			present( { .first_sg = "ну", .second_sg = "нешь", .third_sg = "нет", .first_pl = "нем", .second_pl = "нете", .third_pl = "нут" }, "нуть" );
			present( { .first_sg = "у", .second_sg = "ешь", .third_sg = "ет", .first_pl = "ем", .second_pl = "ете", .third_pl = "ут" }, "ти" );

			for ( const std::string_view ending : { "л", "ла", "ло", "ли" } )
			{
				r.verb( ending, "ть", past );
			}

			r.verb( "й", "ть", imperative );
			r.verb( "йте", "ть", imperative );
			for ( const std::string_view infinitive : { "ить", "еть", "ать", "ти" } )
			{
				r.verb( "и", infinitive, imperative );
				r.verb( "ите", infinitive, imperative );
			}
			r.verb( "ь", "ить", imperative );
			r.verb( "ьте", "ить", imperative );
			r.verb( "уй", "овать", imperative );
			r.verb( "уйте", "овать", imperative );
			r.verb( "ни", "нуть", imperative );
			r.verb( "ните", "нуть", imperative );

			for ( const std::string_view infinitive : { "ть", "ить", "еть", "ти" } )
			{
				r.verb( "я", infinitive, gerund );
			}
			r.verb( "а", "ать", gerund );
			r.verb( "а", "ить", gerund );
			r.verb( "уя", "овать", gerund );
			r.verb( "в", "ть", gerund, surface, true );
			r.verb( "вши", "ть", gerund );

			// Participles are adjectives, declined or not: читающего -> читающий -> читать.
			constexpr std::uint32_t declined = surface | rule::adjective;
			r.verb( "ющий", "ть", pres_act, declined );
			r.verb( "ующий", "овать", pres_act, declined );
			r.verb( "ущий", "ти", pres_act, declined );
			r.verb( "ящий", "ить", pres_act, declined );
			r.verb( "ящий", "еть", pres_act, declined );
			r.verb( "ащий", "ать", pres_act, declined );
			r.verb( "ащий", "ить", pres_act, declined );
			r.verb( "вший", "ть", past_act, declined );
			r.verb( "емый", "ть", pres_pass, declined, true );
			r.verb( "уемый", "овать", pres_pass, declined, true );
			r.verb( "имый", "ить", pres_pass, declined, true );
			r.verb( "нный", "ть", past_pass, declined, true );
			r.verb( "енный", "ить", past_pass, declined, true );
			r.verb( "тый", "ть", past_pass, declined, true );
		}

	} // namespace

	Russian::Russian()
	{
		const Rules rules{ .d = &deinflector_ };
		nounRules( rules );
		adjectiveRules( rules );
		verbRules( rules );
		deinflector_.setMinimumStem( 2 );
		deinflector_.finalize();
	}

	bool Russian::isScriptCharacter( char32_t c ) const noexcept
	{
		return c >= 0x0400 && c <= 0x052F;
	}

	std::span<const Fold> Russian::folds() const noexcept
	{
		static constexpr std::array<Fold, 6> russian{ {
				{ U'ё', U'е' },
				{ U'Ё', U'Е' },
				{ U'ѐ', U'е' },
				{ U'Ѐ', U'Е' },
				{ U'ѝ', U'и' },
				{ U'Ѝ', U'И' },
		} };
		return russian;
	}

	std::string_view Russian::sampleText() const noexcept
	{
		return "Съешь же ещё этих мягких французских булок, да выпей чаю";
	}

	std::string_view Russian::exampleSentence() const noexcept
	{
		return "Вчера мы с другом долго читали интересные книги.";
	}

	std::span<const std::string_view> Russian::sampleWords() const noexcept
	{
		static constexpr std::array<std::string_view, 2> words{ "книги", "говорю" };
		return words;
	}

} // namespace lexiglance::lang::ru
