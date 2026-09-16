#include "Audio.h"

#include <lexiglance/core/Hash.h>
#include <lexiglance/core/Md5.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/net/Http.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <system_error>
#include <vector>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	// After windows.h, which it needs.
	#include <mmsystem.h>
#else
	#include <csignal>
	#include <fcntl.h>
	#include <spawn.h>
	#include <sys/wait.h>
	#include <unistd.h>
#endif

namespace lexiglance::daemon
{

	namespace
	{

		// JapanesePod101 answers unknown words with a "this audio is not available" clip.
		constexpr std::string_view jpod101_placeholder = "7e2c2f954ef6051373ba916f000168dc";

		std::string replaceAll( std::string text, std::string_view from, std::string_view to )
		{
			for ( std::size_t at = text.find( from ); at != std::string::npos; at = text.find( from, at + to.size() ) )
			{
				text.replace( at, from.size(), to );
			}
			return text;
		}

		// Wikimedia Commons names pronunciations "<prefix>-<word>.ogg" and keeps an MP3 of each (which MCI plays, unlike
		// Ogg) under a path made from the file name's MD5.
		std::string commonsUrl( std::string_view prefix, std::string_view word )
		{
			std::string name = std::format( "{}-{}.ogg", prefix, word );
			std::ranges::replace( name, ' ', '_' );
			const std::string hash = md5Hex( name );
			const std::string file = net::urlEncode( name );
			return std::format( "https://upload.wikimedia.org/wikipedia/commons/transcoded/{}/{}/{}/{}.mp3", hash.substr( 0, 1 ), hash.substr( 0, 2 ), file, file );
		}

#ifdef _WIN32
		constexpr char             path_separator = ';';
		constexpr std::string_view program_suffix = ".exe";
#else
		constexpr char             path_separator = ':';
		constexpr std::string_view program_suffix;
#endif

		std::optional<std::string> findProgram( std::string_view name )
		{
			const char*            path = std::getenv( "PATH" );
			const std::string_view directories( path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin" );
			for ( const auto part : std::views::split( directories, path_separator ) )
			{
				std::string candidate( part.begin(), part.end() );
				if ( candidate.empty() )
				{
					continue;
				}
				candidate.append( "/" ).append( name ).append( program_suffix );
#ifdef _WIN32
				std::error_code ec;
				if ( std::filesystem::is_regular_file( std::filesystem::path( std::u8string( reinterpret_cast<const char8_t*>( candidate.data() ), candidate.size() ) ), ec ) )
#else
				if ( ::access( candidate.c_str(), X_OK ) == 0 )
#endif
				{
					return candidate;
				}
			}
			return std::nullopt;
		}

		// The first installed player that decodes MP3.
		std::optional<std::vector<std::string>> playerCommand( const std::string& file )
		{
			struct Candidate
			{
				std::string_view name;
				std::string_view options;
			};
			static constexpr std::array<Candidate, 5> candidates{ {
					{ .name = "ffplay", .options = "-nodisp -autoexit -loglevel quiet" },
					{ .name = "mpv", .options = "--no-video --really-quiet" },
					{ .name = "mpg123", .options = "-q" },
					{ .name = "gst-play-1.0", .options = "-q" },
					{ .name = "pw-play", .options = "" },
			} };
			for ( const Candidate& candidate : candidates )
			{
				if ( auto program = findProgram( candidate.name ) )
				{
					std::vector<std::string> command{ std::move( *program ) };
					for ( const auto option : std::views::split( candidate.options, ' ' ) )
					{
						if ( !option.empty() )
						{
							command.emplace_back( option.begin(), option.end() );
						}
					}
					command.push_back( file );
					return command;
				}
			}
			return std::nullopt;
		}

#ifdef _WIN32

		// The MCI alias of the clip playing.
		constexpr std::wstring_view mci_alias = L"lexiglance_audio";

		std::wstring wide( std::string_view text )
		{
			const int    size = MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
			std::wstring out( static_cast<std::size_t>( std::max( 0, size ) ), L'\0' );
			MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), out.data(), size );
			return out;
		}

		bool mci( const std::wstring& command )
		{
			return mciSendStringW( command.c_str(), nullptr, 0, nullptr ) == 0;
		}

		// Plays through MCI's DirectShow device, which decodes MP3 and WAV on every Windows.
		bool playMci( const std::filesystem::path& file )
		{
			if ( !mci( std::format( L"open \"{}\" type mpegvideo alias {}", file.wstring(), mci_alias ) ) )
			{
				return false;
			}
			if ( !mci( std::format( L"play {}", mci_alias ) ) )
			{
				( void )mci( std::format( L"close {}", mci_alias ) );
				return false;
			}
			return true;
		}

		// A command line as CommandLineToArgvW splits it again.
		std::wstring commandLine( const std::vector<std::string>& command )
		{
			std::wstring line;
			for ( const auto& argument : command )
			{
				line.append( line.empty() ? L"\"" : L" \"" );
				std::size_t backslashes = 0;
				for ( const wchar_t c : wide( argument ) )
				{
					if ( c == L'\\' )
					{
						++backslashes;
						continue;
					}
					line.append( c == L'"' ? ( backslashes * 2 ) + 1 : backslashes, L'\\' );
					line.push_back( c );
					backslashes = 0;
				}
				line.append( backslashes * 2, L'\\' );
				line.push_back( L'"' );
			}
			return line;
		}

		// No window, and no handles of the daemon (its lock file) inherited.
		Result<void*> spawn( const std::vector<std::string>& command )
		{
			std::wstring        line = commandLine( command );
			STARTUPINFOW        startup{};
			PROCESS_INFORMATION process{};
			startup.cb = sizeof( startup );
			if ( CreateProcessW( nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process ) == FALSE )
			{
				return fail( "cannot start {} (error {})", command.front(), GetLastError() );
			}
			CloseHandle( process.hThread );
			return static_cast<void*>( process.hProcess );
		}

		// Players tell formats apart by content; MCI needs the extension.
		std::string_view extensionOf( std::string_view data )
		{
			if ( data.starts_with( "RIFF" ) )
			{
				return ".wav";
			}
			if ( data.starts_with( "OggS" ) )
			{
				return ".ogg";
			}
			if ( data.starts_with( "fLaC" ) )
			{
				return ".flac";
			}
			return ".mp3";
		}

		// Windows refuses to truncate a file that a player still has open, and MCI and the external players all keep
		// the clip they are playing open. Naming each clip after its URL means a new clip never lands on the name of
		// the one still playing, and replaying a clip reuses the file already on disk instead of rewriting it.
		std::filesystem::path clipFile( const std::filesystem::path& directory, const AudioPlayer::Clip& clip )
		{
			auto file = directory / std::format( "audio-{:016x}", hash64( clip.url ) );
			file += extensionOf( clip.data );
			return file;
		}

		// The clips of earlier lookups, once their player has let go of them.
		void sweepClips( const std::filesystem::path& directory, const std::filesystem::path& keep )
		{
			std::error_code ec;
			for ( const auto& entry : std::filesystem::directory_iterator( directory, ec ) )
			{
				if ( entry.path() != keep && entry.path().filename().string().starts_with( "audio-" ) )
				{
					std::error_code ignored;
					std::filesystem::remove( entry.path(), ignored );
				}
			}
		}

#else

		Result<pid_t> spawn( std::vector<std::string>& command )
		{
			std::vector<char*> argv;
			argv.reserve( command.size() + 1 );
			for ( std::string& argument : command )
			{
				argv.push_back( argument.data() );
			}
			argv.push_back( nullptr );

			posix_spawn_file_actions_t actions;
			posix_spawn_file_actions_init( &actions );
			posix_spawn_file_actions_addopen( &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0 );
			posix_spawn_file_actions_addopen( &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0 );
			posix_spawn_file_actions_addopen( &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0 );

			// The daemon blocks its termination signals for a dedicated thread; the player must not inherit that.
			posix_spawnattr_t attributes;
			posix_spawnattr_init( &attributes );
			sigset_t none;
			sigemptyset( &none );
			posix_spawnattr_setsigmask( &attributes, &none );
			posix_spawnattr_setflags( &attributes, POSIX_SPAWN_SETSIGMASK );

			pid_t     pid   = -1;
			const int error = posix_spawn( &pid, argv.front(), &actions, &attributes, argv.data(), environ );
			posix_spawnattr_destroy( &attributes );
			posix_spawn_file_actions_destroy( &actions );
			if ( error != 0 )
			{
				return fail( "cannot start {}: {}", command.front(), std::strerror( error ) );
			}
			return pid;
		}

#endif

	} // namespace

	std::string audioUrl( std::string_view source, std::string_view expression, std::string_view reading, const lang::Language* language )
	{
		const std::string_view spoken = reading.empty() ? expression : reading;
		if ( source == "jpod101" )
		{
			if ( language == nullptr || language->code() != "ja" )
			{
				return {};
			}
			return std::format( "https://assets.languagepod101.com/dictionary/japanese/audiomp3.php?kanji={}&kana={}", net::urlEncode( expression ), net::urlEncode( spoken ) );
		}
		if ( source == "commons" )
		{
			if ( language == nullptr || language->commonsPrefix().empty() )
			{
				return {};
			}
			return commonsUrl( language->commonsPrefix(), expression );
		}
		std::string url = replaceAll( std::string( source ), "{term}", net::urlEncode( expression ) );
		url             = replaceAll( std::move( url ), "{expression}", net::urlEncode( expression ) );
		url             = replaceAll( std::move( url ), "{language}", language != nullptr ? language->code() : std::string_view() );
		return replaceAll( std::move( url ), "{reading}", net::urlEncode( spoken ) );
	}

	AudioPlayer::~AudioPlayer()
	{
		stop();
	}

	Result<std::shared_ptr<const AudioPlayer::Clip>> AudioPlayer::find( std::string_view expression, std::string_view reading, const lang::Language* language, const config::AudioSettings& settings )
	{
		const std::string key = std::format( "{}\t{}\t{}", expression, reading, language != nullptr ? language->code() : std::string_view() );
		{
			const std::scoped_lock lock( cache_mutex_ );
			if ( const auto it = cache_.find( key ); it != cache_.end() )
			{
				if ( !it->second )
				{
					return fail( "No audio for this word" );
				}
				return it->second;
			}
		}

		// The sources with words of the language; Wikimedia Commons when none has any.
		std::vector<std::pair<std::string, bool>> urls;
		for ( const auto& source : settings.sources )
		{
			if ( auto url = audioUrl( source, expression, reading, language ); !url.empty() )
			{
				urls.emplace_back( std::move( url ), source == "jpod101" );
			}
		}
		if ( urls.empty() )
		{
			if ( auto url = audioUrl( "commons", expression, reading, language ); !url.empty() )
			{
				urls.emplace_back( std::move( url ), false );
			}
		}

		std::shared_ptr<const Clip> found;
		std::string                 error;
		for ( const auto& [url, jpod101] : urls )
		{
			auto response = net::fetch( { .url = url, .timeout = std::chrono::seconds( 8 ) } );
			if ( !response )
			{
				error = response.error().message;
				continue;
			}
			if ( response->status != 200 || response->body.size() < 256 || response->content_type.starts_with( "text/" ) )
			{
				continue;
			}
			if ( jpod101 && md5Hex( response->body ) == jpod101_placeholder )
			{
				continue;
			}
			found = std::make_shared<const Clip>( Clip{ .url = url, .data = std::move( response->body ), .placeholder_hash = jpod101 ? std::string( jpod101_placeholder ) : std::string() } );
			break;
		}

		// Network failures are not remembered, so the next click tries again.
		if ( !found && !error.empty() )
		{
			return fail( "{}", error );
		}
		const std::scoped_lock lock( cache_mutex_ );
		if ( cache_.size() >= 128 )
		{
			cache_.clear();
		}
		cache_[key] = found;
		if ( !found )
		{
			return fail( "No audio for this word" );
		}
		return found;
	}

	Result<> AudioPlayer::play( std::string_view expression, std::string_view reading, const lang::Language* language, const config::AudioSettings& settings )
	{
		auto clip = find( expression, reading, language, settings );
		if ( !clip )
		{
			return std::unexpected( clip.error() );
		}

		const std::scoped_lock lock( play_mutex_ );
		stop();
		const auto directory = paths::runtimeDir();
		if ( auto made = paths::ensureDirectory( directory, true ); !made )
		{
			return made;
		}
#ifdef _WIN32
		const auto file = clipFile( directory, **clip );
		sweepClips( directory, file );
		// Replaying a word finds its clip already on disk, where rewriting it could fail while a player holds it.
		std::error_code ec;
		const bool      reusable = std::filesystem::file_size( file, ec ) == ( *clip )->data.size();
#else
		const auto file     = directory / "audio";
		const bool reusable = false;
#endif
		if ( !reusable )
		{
			std::ofstream out( file, std::ios::binary | std::ios::trunc );
			out.write( ( *clip )->data.data(), static_cast<std::streamsize>( ( *clip )->data.size() ) );
			if ( !out )
			{
				// Without the reason this reads as a broken path, which it almost never is.
				return fail( "cannot write {}: {}", file.string(), std::error_code( errno, std::generic_category() ).message() );
			}
		}

#ifdef _WIN32
		if ( playMci( file ) )
		{
			mci_open_ = true;
			return {};
		}
		auto command = playerCommand( file.string() );
		if ( !command )
		{
			return fail( "Windows cannot play this clip (install ffmpeg or mpv for other formats)" );
		}
#else
		auto command = playerCommand( file.string() );
		if ( !command )
		{
			return fail( "No audio player found (install ffmpeg or mpv)" );
		}
#endif
		auto player = spawn( *command );
		if ( !player )
		{
			return std::unexpected( player.error() );
		}
		player_ = *player;
		return {};
	}

	void AudioPlayer::stop()
	{
#ifdef _WIN32
		if ( std::exchange( mci_open_, false ) )
		{
			( void )mci( std::format( L"close {}", mci_alias ) );
		}
		if ( void* process = std::exchange( player_, nullptr ); process != nullptr )
		{
			TerminateProcess( process, 1 );
			WaitForSingleObject( process, 1000 );
			CloseHandle( process );
		}
#else
		// Unreaped players stay zombies until here, so their pid can never be reused under us.
		if ( const pid_t pid = std::exchange( player_, -1 ); pid > 0 )
		{
			::kill( pid, SIGKILL );
			::waitpid( pid, nullptr, 0 );
		}
#endif
	}

} // namespace lexiglance::daemon
