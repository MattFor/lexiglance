#include <lexiglance/config/Keys.h>

#include <algorithm>
#include <array>
#include <utility>

namespace lexiglance::config
{

	namespace
	{

		struct Alias
		{
			std::string_view name;
			KeyGroup         keys;
		};

		// Canonical names follow X11 keysym spelling; the aliases cover what people usually type.
		const std::vector<Alias>& aliases()
		{
			static const std::vector<Alias> table{
				{ "Shift", { Key::ShiftL, Key::ShiftR } },
				{ "Shift_L", { Key::ShiftL } },
				{ "Shift_R", { Key::ShiftR } },
				{ "Control", { Key::ControlL, Key::ControlR } },
				{ "Ctrl", { Key::ControlL, Key::ControlR } },
				{ "Control_L", { Key::ControlL } },
				{ "Control_R", { Key::ControlR } },
				{ "Alt", { Key::AltL, Key::AltR } },
				{ "Alt_L", { Key::AltL } },
				{ "LAlt", { Key::AltL } },
				{ "Alt_R", { Key::AltR } },
				{ "RAlt", { Key::AltR } },
				{ "AltGr", { Key::AltR } },
				{ "Super", { Key::SuperL, Key::SuperR } },
				{ "Win", { Key::SuperL, Key::SuperR } },
				{ "Windows", { Key::SuperL, Key::SuperR } },
				{ "Meta", { Key::SuperL, Key::SuperR } },
				{ "Cmd", { Key::SuperL, Key::SuperR } },
				{ "Super_L", { Key::SuperL } },
				{ "Super_R", { Key::SuperR } },
				{ "Caps_Lock", { Key::CapsLock } },
				{ "Menu", { Key::Menu } },
				{ "Space", { Key::Space } },
				{ "Tab", { Key::Tab } },
				{ "Escape", { Key::Escape } },
				{ "F1", { Key::F1 } },
				{ "F2", { Key::F2 } },
				{ "F3", { Key::F3 } },
				{ "F4", { Key::F4 } },
				{ "F5", { Key::F5 } },
				{ "F6", { Key::F6 } },
				{ "F7", { Key::F7 } },
				{ "F8", { Key::F8 } },
				{ "F9", { Key::F9 } },
				{ "F10", { Key::F10 } },
				{ "F11", { Key::F11 } },
				{ "F12", { Key::F12 } },
				{ "MouseLeft", { Key::MouseLeft } },
				{ "MouseMiddle", { Key::MouseMiddle } },
				{ "MouseRight", { Key::MouseRight } },
				{ "MouseBack", { Key::MouseBack } },
				{ "MouseForward", { Key::MouseForward } },
			};
			return table;
		}

		constexpr std::array<std::string_view, 30> canonical{
			"Shift_L",
			"Shift_R",
			"Control_L",
			"Control_R",
			"Alt_L",
			"Alt_R",
			"Super_L",
			"Super_R",
			"Caps_Lock",
			"Menu",
			"Space",
			"Tab",
			"Escape",
			"F1",
			"F2",
			"F3",
			"F4",
			"F5",
			"F6",
			"F7",
			"F8",
			"F9",
			"F10",
			"F11",
			"F12",
			"MouseLeft",
			"MouseMiddle",
			"MouseRight",
			"MouseBack",
			"MouseForward",
		};

		constexpr std::array<std::string_view, 17> suggestions{
			"Super",
			"Alt_L",
			"Alt_R",
			"Shift",
			"Shift_L",
			"Shift_R",
			"Control",
			"Control_L",
			"Control_R",
			"Super_L",
			"Super_R",
			"Menu",
			"Caps_Lock",
			"MouseMiddle",
			"MouseBack",
			"MouseForward",
			"F12",
		};

		bool equalsIgnoreCase( std::string_view a, std::string_view b ) noexcept
		{
			const auto lower = []( char c ) { return c >= 'A' && c <= 'Z' ? static_cast<char>( c - 'A' + 'a' ) : c; };
			return a.size() == b.size() && std::ranges::equal( a, b, {}, lower, lower );
		}

	} // namespace

	Result<KeyGroup> parseKey( std::string_view name )
	{
		for ( const Alias& alias : aliases() )
		{
			if ( equalsIgnoreCase( alias.name, name ) )
			{
				return alias.keys;
			}
		}
		return fail( "unknown key \"{}\"", name );
	}

	Result<KeyChord> parseChord( std::span<const std::string> names )
	{
		KeyChord chord;
		for ( const std::string& name : names )
		{
			auto group = parseKey( name );
			if ( !group )
			{
				return std::unexpected( group.error() );
			}
			chord.push_back( std::move( *group ) );
		}
		if ( chord.empty() )
		{
			return fail( "the scan trigger needs at least one key" );
		}
		return chord;
	}

	std::string_view keyName( Key key ) noexcept
	{
		const auto index = static_cast<std::size_t>( key );
		return index < canonical.size() ? canonical[index] : std::string_view( "?" );
	}

	bool isMouseButton( Key key ) noexcept
	{
		return key >= Key::MouseLeft;
	}

	std::span<const std::string_view> keyNames() noexcept
	{
		return suggestions;
	}

} // namespace lexiglance::config
