#include "Elevation.h"

// Only Windows has any of this; elsewhere nothing here is built (and the tidy checks on Linux see an empty file).
#ifdef _WIN32

	#include <algorithm>
	#include <cstdint>
	#include <format>
	#include <string_view>
	#include <utility>
	#include <vector>

	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	// After windows.h, which they need.
	#include <oleauto.h>
	#include <sddl.h>
	#include <taskschd.h>

namespace lexiglance::daemon::elevation
{

	namespace
	{

		namespace fs = std::filesystem;

		std::wstring wide( std::string_view text )
		{
			if ( text.empty() )
			{
				return {};
			}
			const int    size = MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
			std::wstring out( static_cast<std::size_t>( std::max( 0, size ) ), L'\0' );
			MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), out.data(), size );
			return out;
		}

		std::string narrow( std::wstring_view text )
		{
			if ( text.empty() )
			{
				return {};
			}
			const int   size = WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0, nullptr, nullptr );
			std::string out( static_cast<std::size_t>( std::max( 0, size ) ), '\0' );
			WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), out.data(), size, nullptr, nullptr );
			return out;
		}

		// A COM interface, released with its owner.
		template <typename T>
		class Com
		{
		public:
			Com() = default;
			~Com()
			{
				if ( pointer_ != nullptr )
				{
					pointer_->Release();
				}
			}

			Com( const Com& )            = delete;
			Com& operator=( const Com& ) = delete;
			Com( Com&& )                 = delete;
			Com& operator=( Com&& )      = delete;

			T* operator->() const noexcept
			{
				return pointer_;
			}

			T** out() noexcept
			{
				return &pointer_;
			}

		private:
			T* pointer_ = nullptr;
		};

		// A BSTR, freed with its owner.
		class Bstr
		{
		public:
			explicit Bstr( std::wstring_view text ) :
				text_( SysAllocStringLen( text.data(), static_cast<UINT>( text.size() ) ) )
			{
			}

			Bstr() = default;
			~Bstr()
			{
				SysFreeString( text_ );
			}

			Bstr( const Bstr& )            = delete;
			Bstr& operator=( const Bstr& ) = delete;
			Bstr( Bstr&& )                 = delete;
			Bstr& operator=( Bstr&& )      = delete;

			[[nodiscard]] BSTR get() const noexcept
			{
				return text_;
			}

			BSTR* out() noexcept
			{
				return &text_;
			}

		private:
			BSTR text_ = nullptr;
		};

		// COM on this thread for as long as it lives, unless the thread had it already.
		class Apartment
		{
		public:
			Apartment() :
				joined_( SUCCEEDED( CoInitializeEx( nullptr, COINIT_MULTITHREADED ) ) )
			{
			}

			~Apartment()
			{
				if ( joined_ )
				{
					CoUninitialize();
				}
			}

			Apartment( const Apartment& )            = delete;
			Apartment& operator=( const Apartment& ) = delete;
			Apartment( Apartment&& )                 = delete;
			Apartment& operator=( Apartment&& )      = delete;

		private:
			bool joined_;
		};

		std::string failure( HRESULT result )
		{
			return std::format( "error 0x{:08x}", static_cast<std::uint32_t>( result ) );
		}

		// What the Task Scheduler answers for a task that is not there: HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ) or
		// ( ERROR_PATH_NOT_FOUND ), spelt out (MinGW's error codes carry a suffix the checks object to).
		bool missing( HRESULT result )
		{
			constexpr auto file_not_found = static_cast<HRESULT>( 0x80070002U );
			constexpr auto path_not_found = static_cast<HRESULT>( 0x80070003U );
			return result == file_not_found || result == path_not_found;
		}

		// The task's name in the Task Scheduler's root folder: one for each user of the computer.
		std::wstring taskName()
		{
			std::wstring user( 257, L'\0' );
			auto         size = static_cast<DWORD>( user.size() );
			if ( GetUserNameW( user.data(), &size ) == FALSE || size == 0 )
			{
				return L"Lexiglance as administrator";
			}
			user.resize( size - 1 );
			// Characters a task's name cannot have (a backslash would make a folder).
			std::ranges::replace_if( user, []( wchar_t c ) { return std::wstring_view( L"\\/:*?\"<>|" ).find( c ) != std::wstring_view::npos; }, L'_' );
			return L"Lexiglance as administrator (" + user + L")";
		}

		// The root folder of the Task Scheduler of this computer.
		Result<> rootFolder( Com<ITaskService>& service, Com<ITaskFolder>& folder )
		{
			HRESULT result = CoCreateInstance( CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>( service.out() ) );
			if ( FAILED( result ) )
			{
				return fail( "the Task Scheduler is not available ({})", failure( result ) );
			}
			const VARIANT none{};
			result = service->Connect( none, none, none, none );
			if ( FAILED( result ) )
			{
				return fail( "cannot reach the Task Scheduler ({})", failure( result ) );
			}
			const Bstr root( L"\\" );
			result = service->GetFolder( root.get(), folder.out() );
			if ( FAILED( result ) )
			{
				return fail( "cannot open the Task Scheduler's folder ({})", failure( result ) );
			}
			return {};
		}

		std::wstring escaped( std::wstring_view text )
		{
			std::wstring out;
			for ( const wchar_t c : text )
			{
				switch ( c )
				{
					case L'&':
						out += L"&amp;";
						break;
					case L'<':
						out += L"&lt;";
						break;
					case L'>':
						out += L"&gt;";
						break;
					case L'"':
						out += L"&quot;";
						break;
					default:
						out += c;
				}
			}
			return out;
		}

		std::wstring unescaped( std::wstring_view text )
		{
			std::wstring out;
			while ( !text.empty() )
			{
				bool found = false;
				for ( const auto& [entity, c] : { std::pair{ std::wstring_view( L"&amp;" ), L'&' },
				                                  std::pair{ std::wstring_view( L"&lt;" ), L'<' },
				                                  std::pair{ std::wstring_view( L"&gt;" ), L'>' },
				                                  std::pair{ std::wstring_view( L"&quot;" ), L'"' },
				                                  std::pair{ std::wstring_view( L"&apos;" ), L'\'' } } )
				{
					if ( text.starts_with( entity ) )
					{
						out += c;
						text.remove_prefix( entity.size() );
						found = true;
						break;
					}
				}
				if ( !found )
				{
					out += text.front();
					text.remove_prefix( 1 );
				}
			}
			return out;
		}

		// The task: started on demand only (the daemon's autostart is what starts it at login), as this user in their
		// session with the highest privileges they have, as often as asked (a daemon replacing another), for as long as
		// it runs, and at normal priority (a task's default is below normal, which would slow lookups while a game
		// runs).
		std::wstring taskXml( const fs::path& program, const std::wstring& user )
		{
			return LR"(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Author>Lexiglance</Author>
    <Description>Starts Lexiglance with administrator rights, so that its trigger works over programs that run as administrator. Turned on and off in Lexiglance, Overview, Startup, Run as administrator.</Description>
  </RegistrationInfo>
  <Principals>
    <Principal id="Author">
      <UserId>)" + user +
			       LR"(</UserId>
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>false</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>4</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>)" + escaped( program.wstring() ) +
			       LR"(</Command>
      <Arguments>--from-task</Arguments>
    </Exec>
  </Actions>
</Task>
)";
		}

	} // namespace

	bool elevated()
	{
		HANDLE token = nullptr;
		if ( OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ) == FALSE )
		{
			return false;
		}
		TOKEN_ELEVATION elevation{};
		DWORD           size   = 0;
		const bool      answer = GetTokenInformation( token, TokenElevation, &elevation, sizeof( elevation ), &size ) != FALSE && elevation.TokenIsElevated != 0;
		CloseHandle( token );
		return answer;
	}

	std::string userSid()
	{
		HANDLE token = nullptr;
		if ( OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ) == FALSE )
		{
			return {};
		}
		DWORD size = 0;
		GetTokenInformation( token, TokenUser, nullptr, 0, &size );
		std::vector<unsigned char> buffer( size );
		std::string                sid;
		LPWSTR                     text = nullptr;
		if ( size > 0 && GetTokenInformation( token, TokenUser, buffer.data(), size, &size ) != FALSE && ConvertSidToStringSidW( reinterpret_cast<const TOKEN_USER*>( buffer.data() )->User.Sid, &text ) != FALSE )
		{
			sid = narrow( text );
			LocalFree( text );
		}
		CloseHandle( token );
		return sid;
	}

	Result<fs::path> taskProgram()
	{
		const Apartment   apartment;
		Com<ITaskService> service;
		Com<ITaskFolder>  folder;
		if ( auto opened = rootFolder( service, folder ); !opened )
		{
			return std::unexpected( opened.error() );
		}
		Com<IRegisteredTask> task;
		const Bstr           name( taskName() );
		HRESULT              result = folder->GetTask( name.get(), task.out() );
		if ( missing( result ) )
		{
			return fs::path();
		}
		if ( FAILED( result ) )
		{
			return fail( "cannot read the scheduled task ({})", failure( result ) );
		}
		Bstr xml;
		result = task->get_Xml( xml.out() );
		if ( FAILED( result ) || xml.get() == nullptr )
		{
			return fail( "cannot read the scheduled task ({})", failure( result ) );
		}
		const std::wstring_view     text( xml.get(), SysStringLen( xml.get() ) );
		constexpr std::wstring_view open  = L"<Command>";
		constexpr std::wstring_view close = L"</Command>";
		const auto                  begin = text.find( open );
		const auto                  end   = begin == std::wstring_view::npos ? begin : text.find( close, begin );
		if ( end == std::wstring_view::npos )
		{
			return fail( "the scheduled task starts no program" );
		}
		// The Task Scheduler takes a command in quotes as well.
		std::wstring command = unescaped( text.substr( begin + open.size(), end - begin - open.size() ) );
		if ( command.size() >= 2 && command.front() == L'"' && command.back() == L'"' )
		{
			command = command.substr( 1, command.size() - 2 );
		}
		return fs::path( command );
	}

	Result<> install( const fs::path& program )
	{
		const std::wstring user = wide( userSid() );
		if ( user.empty() )
		{
			return fail( "cannot tell which account this is" );
		}
		const Apartment   apartment;
		Com<ITaskService> service;
		Com<ITaskFolder>  folder;
		if ( auto opened = rootFolder( service, folder ); !opened )
		{
			return std::unexpected( opened.error() );
		}
		// Administrators and the system have it all; its user may read it, start it and delete it, but not change it.
		const Bstr sddl( L"D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFXSD;;;" + user + L")" );
		VARIANT    security{};
		security.vt      = VT_BSTR;
		security.bstrVal = sddl.get();
		const VARIANT        none{};
		const Bstr           name( taskName() );
		const Bstr           xml( taskXml( program, user ) );
		Com<IRegisteredTask> task;
		const HRESULT        result = folder->RegisterTask( name.get(), xml.get(), TASK_CREATE_OR_UPDATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN, security, task.out() );
		if ( result == E_ACCESSDENIED )
		{
			return fail( "setting it up takes administrator rights" );
		}
		if ( FAILED( result ) )
		{
			return fail( "the Task Scheduler did not take the task ({})", failure( result ) );
		}
		return {};
	}

	Result<> remove()
	{
		const Apartment   apartment;
		Com<ITaskService> service;
		Com<ITaskFolder>  folder;
		if ( auto opened = rootFolder( service, folder ); !opened )
		{
			return std::unexpected( opened.error() );
		}
		const Bstr    name( taskName() );
		const HRESULT result = folder->DeleteTask( name.get(), 0 );
		if ( SUCCEEDED( result ) || missing( result ) )
		{
			return {};
		}
		if ( result == E_ACCESSDENIED )
		{
			return fail( "deleting the scheduled task takes administrator rights" );
		}
		return fail( "cannot delete the scheduled task ({})", failure( result ) );
	}

	Result<> start()
	{
		const Apartment   apartment;
		Com<ITaskService> service;
		Com<ITaskFolder>  folder;
		if ( auto opened = rootFolder( service, folder ); !opened )
		{
			return std::unexpected( opened.error() );
		}
		Com<IRegisteredTask> task;
		const Bstr           name( taskName() );
		HRESULT              result = folder->GetTask( name.get(), task.out() );
		if ( FAILED( result ) )
		{
			return fail( "cannot find the scheduled task ({})", failure( result ) );
		}
		const VARIANT     none{};
		Com<IRunningTask> running;
		result = task->Run( none, running.out() );
		if ( FAILED( result ) )
		{
			return fail( "the Task Scheduler did not start it ({})", failure( result ) );
		}
		return {};
	}

	bool startUnelevated( const fs::path& program, std::span<const std::string> arguments )
	{
		// The desktop shell runs as the user without administrator rights: a copy of its token starts the program.
		DWORD shell = 0;
		if ( const HWND desktop = GetShellWindow(); desktop != nullptr )
		{
			GetWindowThreadProcessId( desktop, &shell );
		}
		if ( shell == 0 )
		{
			return false;
		}
		HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shell );
		if ( process == nullptr )
		{
			return false;
		}
		HANDLE token   = nullptr;
		HANDLE primary = nullptr;
		if ( OpenProcessToken( process, TOKEN_DUPLICATE, &token ) != FALSE )
		{
			constexpr DWORD access = TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID;
			if ( DuplicateTokenEx( token, access, nullptr, SecurityImpersonation, TokenPrimary, &primary ) == FALSE )
			{
				primary = nullptr;
			}
			CloseHandle( token );
		}
		CloseHandle( process );
		if ( primary == nullptr )
		{
			return false;
		}
		std::wstring line = L"\"" + program.wstring() + L"\"";
		for ( const std::string& argument : arguments )
		{
			line += argument.find_first_of( " \t" ) == std::string::npos ? L" " + wide( argument ) : L" \"" + wide( argument ) + L"\"";
		}
		STARTUPINFOW        startup{};
		PROCESS_INFORMATION started{};
		startup.cb      = sizeof( startup );
		const bool done = CreateProcessWithTokenW( primary, 0, program.c_str(), line.data(), CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &started ) != FALSE;
		CloseHandle( primary );
		if ( done )
		{
			CloseHandle( started.hThread );
			CloseHandle( started.hProcess );
		}
		return done;
	}

} // namespace lexiglance::daemon::elevation

#endif
