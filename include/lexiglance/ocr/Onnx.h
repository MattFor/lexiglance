#ifndef LEXIGLANCE_OCR_ONNX_H
#define LEXIGLANCE_OCR_ONNX_H

#include <lexiglance/core/Error.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct OrtApi;
struct OrtSession;
struct OrtMemoryInfo;

namespace lexiglance::ocr
{

	struct Tensor
	{
		std::vector<std::int64_t> shape;
		std::vector<float>        data;
	};

	// A named input for OnnxModel::run: its element type, shape and data, which must stay valid while the model runs.
	struct Input
	{
		enum class Type : std::uint8_t
		{
			Float,
			Int64,
			Bool
		};

		std::string_view              name;
		Type                          type = Type::Float;
		std::span<const std::int64_t> shape;
		const void*                   data  = nullptr;
		std::size_t                   bytes = 0;
	};

	// The two ways OCR is simply not set up yet: the models are not there, or the runtime to run them is not. Both are
	// ordinary states rather than faults, which is what loadFailureActionable() tells apart.
	inline constexpr std::string_view paddle_absent  = "PaddleOCR is not downloaded";
	inline constexpr std::string_view runtime_absent = "ONNX Runtime is not installed";

	// What a Windows without Microsoft's runtime is called, in the message that says so and in the health report, which
	// looks for it to offer installing the thing rather than a page to go to.
	inline constexpr std::string_view vc_runtime_absent = "the Microsoft Visual C++ Redistributable";

	// Whether a failure to load PaddleOCR is one the user can act on, rather than one of the states above. Such a
	// reason is worth reporting in place of a fallback engine's own, which would otherwise hide it behind a download
	// that would not help.
	[[nodiscard]] bool loadFailureActionable( std::string_view message );

	// The Visual C++ runtime libraries Windows cannot find, as a readable list, or empty when it has them all (and
	// always empty elsewhere). Microsoft's onnxruntime.dll is built with MSVC and imports these, while Lexiglance
	// brings the UCRT along and needs none of them, so this is the usual reason a downloaded runtime will not load.
	[[nodiscard]] std::string missingVcRuntime();

	// Those of `names` that `present` answers false for, as a readable list; empty when it answers true for them all.
	[[nodiscard]] std::string missingLibraries( std::span<const char* const> names, const std::function<bool( const char* )>& present );

	// One ONNX model. ONNX Runtime is loaded at run time (libonnxruntime.so from `runtime_dir`, then the system's; on
	// Windows onnxruntime.dll from `runtime_dir` only), so it stays an optional dependency.
	class OnnxModel
	{
	public:
		[[nodiscard]] static Result<std::unique_ptr<OnnxModel>> load( const std::filesystem::path& model, const std::filesystem::path& runtime_dir, int threads );

		~OnnxModel();

		OnnxModel( const OnnxModel& )            = delete;
		OnnxModel& operator=( const OnnxModel& ) = delete;
		OnnxModel( OnnxModel&& )                 = delete;
		OnnxModel& operator=( OnnxModel&& )      = delete;

		// Runs the model on one float input and returns its first output.
		[[nodiscard]] Result<Tensor> run( std::span<float> input, std::span<const std::int64_t> shape );

		// Runs the model on named inputs and returns the named outputs (float tensors), in the order asked for.
		[[nodiscard]] Result<std::vector<Tensor>> run( std::span<const Input> inputs, std::span<const std::string> outputs );

		// The names of the model's inputs and outputs.
		[[nodiscard]] std::vector<std::string> inputNames() const;
		[[nodiscard]] std::vector<std::string> outputNames() const;

		// A custom metadata entry, e.g. a recognition model's "character" list; empty when absent.
		[[nodiscard]] std::string metadata( const char* key ) const;

	private:
		OnnxModel() = default;

		const OrtApi*  api_     = nullptr;
		OrtSession*    session_ = nullptr;
		OrtMemoryInfo* memory_  = nullptr;
		std::string    input_;
		std::string    output_;
	};

} // namespace lexiglance::ocr

#endif // LEXIGLANCE_OCR_ONNX_H
