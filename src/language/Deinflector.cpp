#include <lexiglance/language/Deinflector.h>

#include <lexiglance/core/Utf8.h>

#include <algorithm>

namespace lexiglance::lang
{

	namespace
	{

		constexpr std::size_t max_candidates = 256;

	} // namespace

	std::uint16_t Deinflector::addTransform( std::string name, std::string description )
	{
		transforms_.push_back( { .name = std::move( name ), .description = std::move( description ) } );
		return static_cast<std::uint16_t>( transforms_.size() - 1 );
	}

	void Deinflector::addRule( std::string_view inflected, std::string_view deinflected, std::uint32_t conditions_in, std::uint32_t conditions_out, std::uint16_t transform )
	{
		rules_.push_back(
				{
						.inflected      = std::string( inflected ),
						.deinflected    = std::string( deinflected ),
						.conditions_in  = conditions_in,
						.conditions_out = conditions_out,
						.transform      = transform,
				}
		);
	}

	void Deinflector::finalize()
	{
		kana_buckets_.assign( kana_last - kana_first + 1, {} );
		other_buckets_.clear();
		for ( std::uint32_t i = 0; i < rules_.size(); ++i )
		{
			const char32_t last = utf8::last( rules_[i].inflected );
			if ( last >= kana_first && last <= kana_last )
			{
				kana_buckets_[last - kana_first].push_back( i );
			}
			else
			{
				other_buckets_[last].push_back( i );
			}
		}
	}

	std::span<const std::uint32_t> Deinflector::bucket( char32_t last ) const noexcept
	{
		if ( last >= kana_first && last <= kana_last )
		{
			return kana_buckets_.empty() ? std::span<const std::uint32_t>{} : kana_buckets_[last - kana_first];
		}
		const auto it = other_buckets_.find( last );
		return it != other_buckets_.end() ? std::span<const std::uint32_t>( it->second ) : std::span<const std::uint32_t>{};
	}

	void Deinflector::deinflect( std::string_view text, std::vector<Deinflection>& out ) const
	{
		out.clear();
		out.push_back( { .text = std::string( text ) } );

		for ( std::size_t i = 0; i < out.size() && out.size() < max_candidates; ++i )
		{
			if ( out[i].length >= Deinflection::max_chain )
			{
				continue;
			}

			for ( const std::uint32_t index : bucket( utf8::last( out[i].text ) ) )
			{
				const Rule&         rule    = rules_[index];
				const Deinflection& current = out[i];
				if ( current.conditions != 0 && ( current.conditions & rule.conditions_in ) == 0 )
				{
					continue;
				}
				if ( !std::string_view( current.text ).ends_with( rule.inflected ) || ( current.text.size() == rule.inflected.size() && rule.deinflected.empty() ) )
				{
					continue;
				}
				if ( minimum_stem_ > 0 && utf8::length( std::string_view( current.text ).substr( 0, current.text.size() - rule.inflected.size() ) ) < minimum_stem_ )
				{
					continue;
				}

				Deinflection next;
				next.text.reserve( current.text.size() - rule.inflected.size() + rule.deinflected.size() );
				next.text.append( current.text, 0, current.text.size() - rule.inflected.size() );
				next.text.append( rule.deinflected );
				next.conditions           = rule.conditions_out;
				next.chain                = current.chain;
				next.length               = current.length;
				next.chain[next.length++] = rule.transform;

				const bool duplicate = std::ranges::any_of( out, [&]( const Deinflection& d ) { return d.conditions == next.conditions && d.text == next.text; } );
				if ( !duplicate && !next.text.empty() )
				{
					out.push_back( std::move( next ) );
					if ( out.size() >= max_candidates )
					{
						return;
					}
				}
			}
		}
	}

	std::string_view Deinflector::transformName( std::uint16_t id ) const noexcept
	{
		return id < transforms_.size() ? std::string_view( transforms_[id].name ) : std::string_view();
	}

} // namespace lexiglance::lang
