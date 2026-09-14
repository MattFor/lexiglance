#include <lexiglance/lookup/DictionarySet.h>

namespace lexiglance::lookup
{

	DictionarySet::DictionarySet( std::vector<LoadedDictionary> dictionaries ) :
		dictionaries_( std::move( dictionaries ) )
	{
		for ( std::size_t i = 0; i < dictionaries_.size(); ++i )
		{
			const auto& dictionary = *dictionaries_[i].dictionary;
			const auto  index      = static_cast<std::uint16_t>( i );
			if ( !dictionary.terms().empty() )
			{
				terms_.push_back( index );
			}
			if ( !dictionary.meta().empty() )
			{
				meta_.push_back( index );
			}
			if ( !dictionary.kanji().empty() )
			{
				kanji_.push_back( index );
			}
		}
	}

} // namespace lexiglance::lookup
