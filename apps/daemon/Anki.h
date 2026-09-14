#ifndef LEXIGLANCE_DAEMON_ANKI_H
#define LEXIGLANCE_DAEMON_ANKI_H

#include "Audio.h"

#include <lexiglance/config/Config.h>
#include <lexiglance/core/Error.h>
#include <lexiglance/lookup/Translator.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lexiglance::daemon
{

	struct NoteContext
	{
		// Text around the lookup and the byte offset where the looked-up text starts in it.
		std::string                              sentence;
		std::size_t                              sentence_offset = 0;
		std::shared_ptr<const AudioPlayer::Clip> audio;
	};

	using Markers = std::unordered_map<std::string, std::string>;

	// Values for Yomitan style {markers} of one entry.
	[[nodiscard]] Markers noteMarkers( const lookup::LookupResult& result, const lookup::TermEntry& entry, const NoteContext& context );

	[[nodiscard]] std::string fillTemplate( std::string_view pattern, const Markers& markers );

	// The sentence containing `offset`, cut at 。！？ and line breaks; returns it and the offset within it.
	[[nodiscard]] std::pair<std::string, std::size_t> sentenceAround( std::string_view text, std::size_t offset );

	// For each term (up to 24), whether Anki would accept it, i.e. has no note with the same first field. Blocking.
	[[nodiscard]] Result<std::vector<bool>> canAddNotes( const config::AnkiSettings& settings, const lookup::LookupResult& result, const NoteContext& context );

	// Adds a note through AnkiConnect and returns its id. Blocking; worker threads only.
	[[nodiscard]] Result<std::int64_t> addNote( const config::AnkiSettings& settings, const lookup::LookupResult& result, const lookup::TermEntry& entry, const NoteContext& context );

	// AnkiConnect's API version, asked with a short timeout: whether Anki is running with the add-on.
	[[nodiscard]] Result<std::int64_t> ankiVersion( const config::AnkiSettings& settings );

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_ANKI_H
