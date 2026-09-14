#ifndef LEXIGLANCE_IPC_MESSAGES_H
#define LEXIGLANCE_IPC_MESSAGES_H

#include <lexiglance/core/Json.h>
#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/StructuredContent.h>
#include <lexiglance/lookup/Translator.h>

namespace lexiglance::ipc
{

	// Lookup result as sent to clients; glossaries are pre-rendered in `markup`.
	void writeLookup( json::Writer& out, const lookup::LookupResult& result, dict::Markup markup );

	void writeDictionary( json::Writer& out, const dict::Dictionary& dictionary, bool enabled );

} // namespace lexiglance::ipc

#endif // LEXIGLANCE_IPC_MESSAGES_H
