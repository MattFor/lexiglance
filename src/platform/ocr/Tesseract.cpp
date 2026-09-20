#include "Tesseract.h"

#include <lexiglance/core/Paths.h>

#include <array>
#include <cstdlib>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <dlfcn.h>
#endif

namespace lexiglance::platform
{

	namespace
	{

		// TessPageIteratorLevel and TessPageSegMode values from tesseract/capi.h.
		constexpr int level_textline            = 2;
		constexpr int level_word                = 3;
		constexpr int level_symbol              = 4;
		constexpr int psm_single_block          = 6;
		constexpr int psm_single_block_vertical = 5;

#ifdef _WIN32
		// Tesseract's Windows installer (UB Mannheim) names the library after the major version; MSYS2 and vcpkg builds
		// after the minor one.
		constexpr std::array library_names{ "libtesseract-5.dll", "libtesseract-5.5.dll", "libtesseract-5.4.dll", "libtesseract-5.3.dll", "tesseract55.dll", "tesseract54.dll", "tesseract53.dll" };

		std::filesystem::path environmentPath( const wchar_t* name )
		{
			const wchar_t* value = _wgetenv( name );
			return value != nullptr && *value != L'\0' ? std::filesystem::path( value ) : std::filesystem::path();
		}

		// Where the installer puts Tesseract, which it does not add to PATH.
		std::vector<std::filesystem::path> installDirectories()
		{
			std::vector<std::filesystem::path> directories;
			for ( const wchar_t* variable : { L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA" } )
			{
				if ( auto base = environmentPath( variable ); !base.empty() )
				{
					directories.push_back( variable == std::wstring_view( L"LOCALAPPDATA" ) ? base / "Programs" / "Tesseract-OCR" : base / "Tesseract-OCR" );
				}
			}
			return directories;
		}

		void* openLibrary()
		{
			// Next to its own DLLs (Leptonica and the image libraries) first, then wherever PATH finds it.
			for ( const auto& directory : installDirectories() )
			{
				for ( const char* name : library_names )
				{
					const auto candidate = directory / name;
					if ( HMODULE library = LoadLibraryExW( candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH ); library != nullptr )
					{
						return static_cast<void*>( library );
					}
				}
			}
			for ( const char* name : library_names )
			{
				if ( HMODULE library = LoadLibraryA( name ); library != nullptr )
				{
					return static_cast<void*>( library );
				}
			}
			return nullptr;
		}

		void* symbol( void* library, const char* name )
		{
			return reinterpret_cast<void*>( GetProcAddress( static_cast<HMODULE>( library ), name ) );
		}

		void closeLibrary( void* library )
		{
			FreeLibrary( static_cast<HMODULE>( library ) );
		}

		// Diagnostics would only clutter the daemon's log.
		constexpr const char* null_device = "NUL";
#else
		void* openLibrary()
		{
			void* library = dlopen( "libtesseract.so.5", RTLD_NOW | RTLD_LOCAL );
			return library != nullptr ? library : dlopen( "libtesseract.so", RTLD_NOW | RTLD_LOCAL );
		}

		void* symbol( void* library, const char* name )
		{
			return dlsym( library, name );
		}

		void closeLibrary( void* library )
		{
			dlclose( library );
		}

		// Diagnostics would only clutter the daemon's stderr.
		constexpr const char* null_device = "/dev/null";
#endif

		template <typename F>
		bool bind( void* library, const char* name, F& target )
		{
			target = reinterpret_cast<F>( symbol( library, name ) );
			return target != nullptr;
		}

	} // namespace

	struct TesseractEngine::Functions
	{
		void* ( *create )()                                                    = nullptr;
		void ( *destroy )( void* )                                             = nullptr;
		int ( *init )( void*, const char*, const char* )                       = nullptr;
		void ( *end )( void* )                                                 = nullptr;
		void ( *set_page_seg_mode )( void*, int )                              = nullptr;
		int ( *set_variable )( void*, const char*, const char* )               = nullptr;
		void ( *set_image )( void*, const unsigned char*, int, int, int, int ) = nullptr;
		void ( *set_source_resolution )( void*, int )                          = nullptr;
		int ( *recognize )( void*, void* )                                     = nullptr;
		void* ( *get_iterator )( void* )                                       = nullptr;
		void ( *clear )( void* )                                               = nullptr;
		void ( *iterator_delete )( void* )                                     = nullptr;
		void* ( *iterator_page )( void* )                                      = nullptr;
		char* ( *iterator_text )( const void*, int )                           = nullptr;
		int ( *iterator_next )( void*, int )                                   = nullptr;
		float ( *iterator_confidence )( const void*, int )                     = nullptr;
		int ( *page_box )( const void*, int, int*, int*, int*, int* )          = nullptr;
		int ( *page_at_beginning )( const void*, int )                         = nullptr;
		void ( *delete_text )( const char* )                                   = nullptr;
		const char* ( *version )()                                             = nullptr;
	};

	Result<std::unique_ptr<TesseractEngine>> TesseractEngine::load( const std::filesystem::path& datapath, const std::string& language, bool vertical )
	{
		void* library = openLibrary();
		if ( library == nullptr )
		{
			return fail( "libtesseract is not installed" );
		}

		std::unique_ptr<TesseractEngine> engine( new TesseractEngine() );
		engine->library_   = library;
		engine->functions_ = std::make_unique<Functions>();
		Functions& f       = *engine->functions_;

		const bool bound = bind( library, "TessBaseAPICreate", f.create ) && bind( library, "TessBaseAPIDelete", f.destroy ) && bind( library, "TessBaseAPIInit3", f.init ) &&
		                   bind( library, "TessBaseAPIEnd", f.end ) && bind( library, "TessBaseAPISetPageSegMode", f.set_page_seg_mode ) &&
		                   bind( library, "TessBaseAPISetVariable", f.set_variable ) && bind( library, "TessBaseAPISetImage", f.set_image ) &&
		                   bind( library, "TessBaseAPISetSourceResolution", f.set_source_resolution ) && bind( library, "TessBaseAPIRecognize", f.recognize ) &&
		                   bind( library, "TessBaseAPIGetIterator", f.get_iterator ) && bind( library, "TessBaseAPIClear", f.clear ) &&
		                   bind( library, "TessResultIteratorDelete", f.iterator_delete ) && bind( library, "TessResultIteratorGetPageIterator", f.iterator_page ) &&
		                   bind( library, "TessResultIteratorGetUTF8Text", f.iterator_text ) && bind( library, "TessResultIteratorNext", f.iterator_next ) &&
		                   bind( library, "TessResultIteratorConfidence", f.iterator_confidence ) && bind( library, "TessPageIteratorBoundingBox", f.page_box ) &&
		                   bind( library, "TessPageIteratorIsAtBeginningOf", f.page_at_beginning ) && bind( library, "TessDeleteText", f.delete_text ) &&
		                   bind( library, "TessVersion", f.version );
		if ( !bound )
		{
			return fail( "libtesseract lacks the expected C API" );
		}

		engine->version_ = f.version();
		engine->handle_  = f.create();
		// UTF-8, which the executables use as their code page on Windows too.
		const std::u8string directory = datapath.u8string();
		if ( engine->handle_ == nullptr || f.init( engine->handle_, reinterpret_cast<const char*>( directory.c_str() ), language.c_str() ) != 0 )
		{
			return fail( "cannot load the {} OCR model from {}", language, datapath.string() );
		}

		f.set_page_seg_mode( engine->handle_, vertical ? psm_single_block_vertical : psm_single_block );
		// Inversion is done before recognition.
		( void )f.set_variable( engine->handle_, "tessedit_do_invert", "0" );
		( void )f.set_variable( engine->handle_, "debug_file", null_device );
		return engine;
	}

	TesseractEngine::~TesseractEngine()
	{
		if ( functions_ && handle_ != nullptr )
		{
			functions_->end( handle_ );
			functions_->destroy( handle_ );
		}
		if ( library_ != nullptr )
		{
			closeLibrary( library_ );
		}
	}

	std::vector<OcrSymbol> TesseractEngine::recognize( std::span<const std::uint8_t> pixels, int width, int height )
	{
		const Functions& f = *functions_;
		f.set_image( handle_, pixels.data(), width, height, 1, width );
		f.set_source_resolution( handle_, 144 );

		std::vector<OcrSymbol> symbols;
		if ( f.recognize( handle_, nullptr ) != 0 )
		{
			f.clear( handle_ );
			return symbols;
		}

		void* iterator = f.get_iterator( handle_ );
		if ( iterator == nullptr )
		{
			f.clear( handle_ );
			return symbols;
		}

		void* page = f.iterator_page( iterator );
		int   line = -1;
		do
		{
			if ( f.page_at_beginning( page, level_textline ) != 0 )
			{
				++line;
			}
			const bool word = f.page_at_beginning( page, level_word ) != 0;
			char*      text = f.iterator_text( iterator, level_symbol );
			if ( text == nullptr )
			{
				continue;
			}
			std::array<int, 4> box{};
			if ( f.page_box( page, level_symbol, box.data(), &box[1], &box[2], &box[3] ) != 0 )
			{
				symbols.push_back( {
						.text       = text,
						.box        = { .x = box[0], .y = box[1], .width = box[2] - box[0], .height = box[3] - box[1] },
						.line       = std::max( line, 0 ),
						.confidence = f.iterator_confidence( iterator, level_symbol ),
						.word       = word,
				} );
			}
			f.delete_text( text );
		} while ( f.iterator_next( iterator, level_symbol ) != 0 );

		f.iterator_delete( iterator );
		f.clear( handle_ );
		return symbols;
	}

	std::optional<std::filesystem::path> findTessdata( std::string_view language, std::string_view model )
	{
		namespace fs = std::filesystem;
		std::vector<fs::path> candidates{ paths::ocrDir() / model };
		if ( const char* prefix = std::getenv( "TESSDATA_PREFIX" ); prefix != nullptr && *prefix != '\0' )
		{
			candidates.emplace_back( prefix );
			candidates.push_back( fs::path( prefix ) / "tessdata" );
		}
#ifdef _WIN32
		for ( const auto& directory : installDirectories() )
		{
			candidates.push_back( directory / "tessdata" );
		}
#else
		for ( const char* system : { "/usr/share/tessdata", "/usr/share/tesseract-ocr/5/tessdata", "/usr/share/tesseract-ocr/4.00/tessdata", "/usr/local/share/tessdata" } )
		{
			candidates.emplace_back( system );
		}
#endif

		const std::string file = std::string( language ) + ".traineddata";
		for ( const auto& directory : candidates )
		{
			std::error_code ec;
			if ( fs::is_regular_file( directory / file, ec ) )
			{
				return directory;
			}
		}
		return std::nullopt;
	}

} // namespace lexiglance::platform
