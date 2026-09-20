#ifndef LEXIGLANCE_CONFIG_KEYS_H
#define LEXIGLANCE_CONFIG_KEYS_H

#include <lexiglance/core/Error.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::config
{

	// Platform neutral keys usable in a scan trigger. Backends map them to native key codes / mouse buttons.
	enum class Key : std::uint8_t
	{
		ShiftL,
		ShiftR,
		ControlL,
		ControlR,
		AltL,
		AltR,
		SuperL,
		SuperR,
		CapsLock,
		Menu,
		Space,
		Tab,
		Escape,
		F1,
		F2,
		F3,
		F4,
		F5,
		F6,
		F7,
		F8,
		F9,
		F10,
		F11,
		F12,
		MouseLeft,
		MouseMiddle,
		MouseRight,
		MouseBack,
		MouseForward,
	};

	// Any one key of a group satisfies it ("Super" is Super_L or Super_R).
	using KeyGroup = std::vector<Key>;

	// A chord is held when every one of its groups is satisfied.
	using KeyChord = std::vector<KeyGroup>;

	[[nodiscard]] Result<KeyGroup> parseKey( std::string_view name );

	[[nodiscard]] Result<KeyChord> parseChord( std::span<const std::string> names );

	// A key held together with a chord for something more (the sentence key): its group, or none when the name is empty
	// or unknown, or the chord has one of its keys already (it would always be held with it).
	[[nodiscard]] KeyGroup extraKey( std::string_view name, const KeyChord& chord );

	[[nodiscard]] std::string_view keyName( Key key ) noexcept;

	// keyName as this platform's users know the key: "Left Super" on X11, "Left Win" on Windows,
	// "Left Alt" / "Left Ctrl" / "Caps Lock" everywhere.
	[[nodiscard]] std::string_view displayKeyName( Key key ) noexcept;

	// Any name parseKey accepts, spelled as this platform's users know the key. Unknown names come back unchanged.
	[[nodiscard]] std::string displayName( std::string_view name );

	[[nodiscard]] bool isMouseButton( Key key ) noexcept;

	// Names accepted by parseKey, for configuration UIs.
	[[nodiscard]] std::span<const std::string_view> keyNames() noexcept;

} // namespace lexiglance::config

#endif // LEXIGLANCE_CONFIG_KEYS_H
