#ifndef LEXIGLANCE_OCR_ONNX_H
#define LEXIGLANCE_OCR_ONNX_H

#include <lexiglance/core/Error.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
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
