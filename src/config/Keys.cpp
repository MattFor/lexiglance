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
				{ .name = "Shift", .keys = { Key::ShiftL, Key::ShiftR } },
				{ .name = "Shift_L", .keys = { Key::ShiftL } },
				{ .name = "Left Shift", .keys = { Key::ShiftL } },
				{ .name = "Shift_R", .keys = { Key::ShiftR } },
				{ .name = "Right Shift", .keys = { Key::ShiftR } },
				{ .name = "Control", .keys = { Key::ControlL, Key::ControlR } },
				{ .name = "Ctrl", .keys = { Key::ControlL, Key::ControlR } },
				{ .name = "Control_L", .keys = { Key::ControlL } },
				{ .name = "Left Ctrl", .keys = { Key::ControlL } },
				{ .name = "Left Control", .keys = { Key::ControlL } },
				{ .name = "Control_R", .keys = { Key::ControlR } },
				{ .name = "Right Ctrl", .keys = { Key::ControlR } },
				{ .name = "Right Control", .keys = { Key::ControlR } },
				{ .name = "Alt", .keys = { Key::AltL, Key::AltR } },
				{ .name = "Alt_L", .keys = { Key::AltL } },
				{ .name = "LAlt", .keys = { Key::AltL } },
				{ .name = "Left Alt", .keys = { Key::AltL } },
				{ .name = "Alt_R", .keys = { Key::AltR } },
				{ .name = "RAlt", .keys = { Key::AltR } },
				{ .name = "Right Alt", .keys = { Key::AltR } },
				{ .name = "AltGr", .keys = { Key::AltR } },
				{ .name = "Super", .keys = { Key::SuperL, Key::SuperR } },
				{ .name = "Win", .keys = { Key::SuperL, Key::SuperR } },
				{ .name = "Windows", .keys = { Key::SuperL, Key::SuperR } },
				{ .name = "Meta", .keys = { Key::SuperL, Key::SuperR } },
				{ .name = "Cmd", .keys = { Key::SuperL, Key::SuperR } },
				{ .name = "Super_L", .keys = { Key::SuperL } },
				{ .name = "Win_L", .keys = { Key::SuperL } },
				{ .name = "LWin", .keys = { Key::SuperL } },
				{ .name = "Left Win", .keys = { Key::SuperL } },
				{ .name = "Left Super", .keys = { Key::SuperL } },
				{ .name = "Super_R", .keys = { Key::SuperR } },
				{ .name = "Win_R", .keys = { Key::SuperR } },
				{ .name = "RWin", .keys = { Key::SuperR } },
				{ .name = "Right Win", .keys = { Key::SuperR } },
				{ .name = "Right Super", .keys = { Key::SuperR } },
				{ .name = "Caps_Lock", .keys = { Key::CapsLock } },
				{ .name = "Caps Lock", .keys = { Key::CapsLock } },
				{ .name = "Menu", .keys = { Key::Menu } },
				{ .name = "Space", .keys = { Key::Space } },
				{ .name = "Tab", .keys = { Key::Tab } },
				{ .name = "Escape", .keys = { Key::Escape } },
				{ .name = "F1", .keys = { Key::F1 } },
				{ .name = "F2", .keys = { Key::F2 } },
				{ .name = "F3", .keys = { Key::F3 } },
				{ .name = "F4", .keys = { Key::F4 } },
				{ .name = "F5", .keys = { Key::F5 } },
				{ .name = "F6", .keys = { Key::F6 } },
				{ .name = "F7", .keys = { Key::F7 } },
				{ .name = "F8", .keys = { Key::F8 } },
				{ .name = "F9", .keys = { Key::F9 } },
				{ .name = "F10", .keys = { Key::F10 } },
				{ .name = "F11", .keys = { Key::F11 } },
				{ .name = "F12", .keys = { Key::F12 } },
				{ .name = "MouseLeft", .keys = { Key::MouseLeft } },
				{ .name = "MouseMiddle", .keys = { Key::MouseMiddle } },
				{ .name = "MouseRight", .keys = { Key::MouseRight } },
				{ .name = "MouseBack", .keys = { Key::MouseBack } },
				{ .name = "MouseForward", .keys = { Key::MouseForward } },
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

		// Windows labels this key with a logo, not with X11's name for it; configuration files keep the canonical
		// spelling above, which parseKey accepts everywhere. What the UI shows is the everyday name.
#ifdef _WIN32
		constexpr std::string_view super_either = "Win";
		constexpr std::string_view super_left   = "Left Win";
		constexpr std::string_view super_right  = "Right Win";
#else
		constexpr std::string_view super_either = "Super";
		constexpr std::string_view super_left   = "Left Super";
		constexpr std::string_view super_right  = "Right Super";
#endif

		constexpr std::array<std::string_view, 17> suggestions{
			super_either,
			"Left Alt",
			"Right Alt",
			"Shift",
			"Left Shift",
			"Right Shift",
			"Ctrl",
			"Left Ctrl",
			"Right Ctrl",
			super_left,
			super_right,
			"Menu",
			"Caps Lock",
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

	KeyGroup extraKey( std::string_view name, const KeyChord& chord )
	{
		auto group = name.empty() ? Result<KeyGroup>( KeyGroup{} ) : parseKey( name );
		if ( !group )
		{
			return {};
		}
		const bool shared = std::ranges::any_of( *group, [&]( Key key ) { return std::ranges::any_of( chord, [&]( const KeyGroup& held ) { return std::ranges::contains( held, key ); } ); } );
		return shared ? KeyGroup{} : std::move( *group );
	}

	std::string_view keyName( Key key ) noexcept
	{
		const auto index = static_cast<std::size_t>( key );
		return index < canonical.size() ? canonical[index] : std::string_view( "?" );
	}

	std::string_view displayKeyName( Key key ) noexcept
	{
		switch ( key )
		{
			case Key::ShiftL:
				return "Left Shift";
			case Key::ShiftR:
				return "Right Shift";
			case Key::ControlL:
				return "Left Ctrl";
			case Key::ControlR:
				return "Right Ctrl";
			case Key::AltL:
				return "Left Alt";
			case Key::AltR:
				return "Right Alt";
			case Key::SuperL:
				return super_left;
			case Key::SuperR:
				return super_right;
			case Key::CapsLock:
				return "Caps Lock";
			case Key::MouseLeft:
				return "Left click";
			case Key::MouseMiddle:
				return "Middle click";
			case Key::MouseRight:
				return "Right click";
			case Key::MouseBack:
				return "Back";
			case Key::MouseForward:
				return "Forward";
			default:
				return keyName( key );
		}
	}

	std::string displayName( std::string_view name )
	{
		const auto group = parseKey( name );
		if ( !group )
		{
			return std::string( name );
		}
		if ( *group == KeyGroup{ Key::ShiftL, Key::ShiftR } )
		{
			return "Shift";
		}
		if ( *group == KeyGroup{ Key::ControlL, Key::ControlR } )
		{
			return "Ctrl";
		}
		if ( *group == KeyGroup{ Key::AltL, Key::AltR } )
		{
			return "Alt";
		}
		if ( *group == KeyGroup{ Key::SuperL, Key::SuperR } )
		{
			return std::string( super_either );
		}
		if ( group->size() == 1 )
		{
			return std::string( displayKeyName( group->front() ) );
		}
		return std::string( name );
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
