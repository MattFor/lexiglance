#include <lexiglance/ocr/Onnx.h>

#include <onnxruntime_c_api.h>

#include <array>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <dlfcn.h>
#endif

namespace lexiglance::ocr
{

	namespace
	{

		// The oldest runtime whose C API has everything used here; newer ones answer for it too.
		constexpr std::uint32_t api_version = 16;

#ifdef _WIN32
		constexpr std::array library_names{ "onnxruntime.dll" };
	#if defined( _M_ARM64 ) || defined( __aarch64__ )
		constexpr std::string_view vc_redist_url = "https://aka.ms/vs/17/release/vc_redist.arm64.exe";
	#else
		constexpr std::string_view vc_redist_url = "https://aka.ms/vs/17/release/vc_redist.x64.exe";
	#endif
#elifdef __APPLE__
		constexpr std::array library_names{ "libonnxruntime.dylib", "libonnxruntime.1.dylib" };
#else
		constexpr std::array library_names{ "libonnxruntime.so", "libonnxruntime.so.1" };
#endif

		// Why the library at `candidate` did not load, in words the user can act on.
		std::string loadError( [[maybe_unused]] const std::filesystem::path& candidate )
		{
#ifdef _WIN32
			const DWORD       code    = GetLastError();
			const std::string missing = missingVcRuntime();
			if ( !missing.empty() )
			{
				// What to do about it on a line of its own: the settings application installs it at a click, and the
				// address is for anyone reading this outside it. Short first line, so it reads at a glance.
				return std::format(
						"ONNX Runtime needs {}, which this computer does not have ({} cannot be found).\n"
						"Lexiglance can install it: Overview, Check health, Download and install. By hand: {}",
						vc_runtime_absent,
						missing,
						vc_redist_url
				);
			}
			return std::format( "cannot load {} (Windows error {})", candidate.filename().string(), code );
#else
			const char* reason = dlerror();
			return reason != nullptr ? std::string( reason ) : std::string( runtime_absent );
#endif
		}

		struct Entry
		{
			void*       symbol = nullptr;
			std::string error;
		};

		// ONNX Runtime's entry point, from our own copy of the library or else the system's. Not on Windows: the
		// onnxruntime.dll in System32 is Windows' own, an older release that cannot read the models.
		Entry entryPoint( const std::filesystem::path& directory )
		{
#ifdef _WIN32
			const std::vector<std::filesystem::path> candidates{ directory / library_names.front() };
#else
			std::vector<std::filesystem::path> candidates{ directory / library_names.front() };
			candidates.insert( candidates.end(), library_names.begin(), library_names.end() );
#endif
			Entry entry;
			for ( const auto& candidate : candidates )
			{
#ifdef _WIN32
				if ( HMODULE library = LoadLibraryExW( candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH ); library != nullptr )
				{
					entry.symbol = reinterpret_cast<void*>( GetProcAddress( library, "OrtGetApiBase" ) );
					return entry;
				}
#else
				if ( void* library = dlopen( candidate.c_str(), RTLD_NOW | RTLD_LOCAL ); library != nullptr )
				{
					entry.symbol = dlsym( library, "OrtGetApiBase" );
					return entry;
				}
#endif
				// The first candidate is our own copy, whose absence is the ordinary "not downloaded yet" case.
				std::error_code ec;
				if ( std::filesystem::exists( candidate, ec ) )
				{
					entry.error = loadError( candidate );
				}
			}
			if ( entry.error.empty() )
			{
				entry.error = runtime_absent;
			}
			return entry;
		}

		struct Runtime
		{
			const OrtApi* api = nullptr;
			OrtEnv*       env = nullptr;
			std::string   error;
		};

		// One runtime and environment for the whole process; the library stays loaded until exit. Until one has loaded
		// every call tries again, since the runtime can be downloaded while the daemon runs.
		Runtime runtime( const std::filesystem::path& directory )
		{
			static Runtime         instance;
			static std::mutex      mutex;
			const std::scoped_lock lock( mutex );
			if ( instance.api != nullptr )
			{
				return instance;
			}
			Entry entry = entryPoint( directory );
			if ( entry.symbol == nullptr )
			{
				instance.error = std::move( entry.error );
				return instance;
			}
			using GetApiBase  = const OrtApiBase* ( * )();
			const auto    get = reinterpret_cast<GetApiBase>( entry.symbol );
			const OrtApi* api = get != nullptr ? get()->GetApi( api_version ) : nullptr;
			if ( api == nullptr )
			{
				instance.error = "ONNX Runtime is too old (1.16 or newer is needed)";
				return instance;
			}
			if ( OrtStatus* status = api->CreateEnv( ORT_LOGGING_LEVEL_ERROR, "lexiglance", &instance.env ); status != nullptr )
			{
				instance.error = api->GetErrorMessage( status );
				api->ReleaseStatus( status );
				return instance;
			}
			instance.api = api;
			return instance;
		}

		// For calls whose failure changes nothing (tuning options, freeing names): the status is released all the same.
		void ignore( const OrtApi* api, OrtStatus* status )
		{
			if ( status != nullptr )
			{
				api->ReleaseStatus( status );
			}
		}

		Result<> check( const OrtApi* api, OrtStatus* status )
		{
			if ( status == nullptr )
			{
				return {};
			}
			std::string message = api->GetErrorMessage( status );
			api->ReleaseStatus( status );
			return fail( "{}", message );
		}

	} // namespace

	Result<std::unique_ptr<OnnxModel>> OnnxModel::load( const std::filesystem::path& model, const std::filesystem::path& runtime_dir, int threads )
	{
		const Runtime rt = runtime( runtime_dir );
		if ( rt.api == nullptr )
		{
			return fail( "{}", rt.error );
		}
		const OrtApi* api = rt.api;

		std::unique_ptr<OnnxModel> result( new OnnxModel() );
		result->api_ = api;

		OrtSessionOptions* options = nullptr;
		if ( auto made = check( api, api->CreateSessionOptions( &options ) ); !made )
		{
			return std::unexpected( made.error() );
		}
		ignore( api, api->SetIntraOpNumThreads( options, threads ) );
		ignore( api, api->SetInterOpNumThreads( options, 1 ) );
		ignore( api, api->SetSessionGraphOptimizationLevel( options, ORT_ENABLE_ALL ) );
		// Idle worker threads sleep instead of spinning: far less CPU while the pointer moves, for a little latency.
		ignore( api, api->AddSessionConfigEntry( options, "session.intra_op.allow_spinning", "0" ) );
		ignore( api, api->AddSessionConfigEntry( options, "session.inter_op.allow_spinning", "0" ) );
		const auto created = check( api, api->CreateSession( rt.env, model.c_str(), options, &result->session_ ) );
		api->ReleaseSessionOptions( options );
		if ( !created )
		{
			return fail( "cannot load {}: {}", model.filename().string(), created.error().message );
		}
		if ( auto memory = check( api, api->CreateCpuMemoryInfo( OrtArenaAllocator, OrtMemTypeDefault, &result->memory_ ) ); !memory )
		{
			return std::unexpected( memory.error() );
		}

		OrtAllocator* allocator = nullptr;
		ignore( api, api->GetAllocatorWithDefaultOptions( &allocator ) );
		char* name = nullptr;
		if ( api->SessionGetInputName( result->session_, 0, allocator, &name ) == nullptr && name != nullptr )
		{
			result->input_ = name;
			ignore( api, api->AllocatorFree( allocator, name ) );
		}
		name = nullptr;
		if ( api->SessionGetOutputName( result->session_, 0, allocator, &name ) == nullptr && name != nullptr )
		{
			result->output_ = name;
			ignore( api, api->AllocatorFree( allocator, name ) );
		}
		if ( result->input_.empty() || result->output_.empty() )
		{
			return fail( "{} has no usable input or output", model.filename().string() );
		}
		return result;
	}

	OnnxModel::~OnnxModel()
	{
		if ( api_ != nullptr )
		{
			if ( session_ != nullptr )
			{
				api_->ReleaseSession( session_ );
			}
			if ( memory_ != nullptr )
			{
				api_->ReleaseMemoryInfo( memory_ );
			}
		}
	}

	Result<Tensor> OnnxModel::run( std::span<float> input, std::span<const std::int64_t> shape )
	{
		OrtValue* value = nullptr;
		if ( auto made = check( api_, api_->CreateTensorWithDataAsOrtValue( memory_, input.data(), input.size_bytes(), shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &value ) ); !made )
		{
			return std::unexpected( made.error() );
		}
		const char* input_name  = input_.c_str();
		const char* output_name = output_.c_str();
		OrtValue*   output      = nullptr;
		const auto  ran         = check( api_, api_->Run( session_, nullptr, &input_name, &value, 1, &output_name, 1, &output ) );
		api_->ReleaseValue( value );
		if ( !ran )
		{
			return std::unexpected( ran.error() );
		}

		Tensor                     tensor;
		OrtTensorTypeAndShapeInfo* info  = nullptr;
		std::size_t                count = 0;
		std::size_t                dims  = 0;
		void*                      data  = nullptr;
		if ( api_->GetTensorTypeAndShape( output, &info ) == nullptr )
		{
			ignore( api_, api_->GetDimensionsCount( info, &dims ) );
			tensor.shape.resize( dims );
			ignore( api_, api_->GetDimensions( info, tensor.shape.data(), dims ) );
			ignore( api_, api_->GetTensorShapeElementCount( info, &count ) );
			api_->ReleaseTensorTypeAndShapeInfo( info );
		}
		if ( api_->GetTensorMutableData( output, &data ) == nullptr && data != nullptr )
		{
			const auto* values = static_cast<const float*>( data );
			tensor.data.assign( values, values + count );
		}
		api_->ReleaseValue( output );
		return tensor;
	}

	std::string OnnxModel::metadata( const char* key ) const
	{
		OrtModelMetadata* metadata  = nullptr;
		OrtAllocator*     allocator = nullptr;
		std::string       result;
		if ( api_->SessionGetModelMetadata( session_, &metadata ) != nullptr || api_->GetAllocatorWithDefaultOptions( &allocator ) != nullptr )
		{
			return result;
		}
		char* value = nullptr;
		if ( api_->ModelMetadataLookupCustomMetadataMap( metadata, allocator, key, &value ) == nullptr && value != nullptr )
		{
			result = value;
			ignore( api_, api_->AllocatorFree( allocator, value ) );
		}
		api_->ReleaseModelMetadata( metadata );
		return result;
	}

} // namespace lexiglance::ocr
