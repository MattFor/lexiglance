#include <lexiglance/ocr/Onnx.h>

#include <onnxruntime_c_api.h>

#include <array>
#include <mutex>
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
#elifdef __APPLE__
		constexpr std::array library_names{ "libonnxruntime.dylib", "libonnxruntime.1.dylib" };
#else
		constexpr std::array library_names{ "libonnxruntime.so", "libonnxruntime.so.1" };
#endif

		// ONNX Runtime's entry point, from our own copy of the library or else the system's.
		void* entryPoint( const std::filesystem::path& directory )
		{
			std::vector<std::filesystem::path> candidates{ directory / library_names.front() };
			candidates.insert( candidates.end(), library_names.begin(), library_names.end() );
			for ( const auto& candidate : candidates )
			{
#ifdef _WIN32
				if ( HMODULE library = LoadLibraryW( candidate.c_str() ); library != nullptr )
				{
					return reinterpret_cast<void*>( GetProcAddress( library, "OrtGetApiBase" ) );
				}
#else
				if ( void* library = dlopen( candidate.c_str(), RTLD_NOW | RTLD_LOCAL ); library != nullptr )
				{
					return dlsym( library, "OrtGetApiBase" );
				}
#endif
			}
			return nullptr;
		}

		struct Runtime
		{
			const OrtApi* api = nullptr;
			OrtEnv*       env = nullptr;
			std::string   error;
		};

		// One runtime and environment for the whole process; the library stays loaded until exit.
		Runtime& runtime( const std::filesystem::path& directory )
		{
			static Runtime        instance;
			static std::once_flag once;
			std::call_once( once, [&] {
				void* entry = entryPoint( directory );
				if ( entry == nullptr )
				{
					instance.error = "ONNX Runtime is not installed";
					return;
				}
				using GetApiBase  = const OrtApiBase* ( * )();
				const auto    get = reinterpret_cast<GetApiBase>( entry );
				const OrtApi* api = get != nullptr ? get()->GetApi( api_version ) : nullptr;
				if ( api == nullptr )
				{
					instance.error = "ONNX Runtime is too old (1.16 or newer is needed)";
					return;
				}
				if ( OrtStatus* status = api->CreateEnv( ORT_LOGGING_LEVEL_ERROR, "lexiglance", &instance.env ); status != nullptr )
				{
					instance.error = api->GetErrorMessage( status );
					api->ReleaseStatus( status );
					return;
				}
				instance.api = api;
			} );
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
		Runtime& rt = runtime( runtime_dir );
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
