#ifndef LEXIGLANCE_TRANSLATE_MODEL_H
#define LEXIGLANCE_TRANSLATE_MODEL_H

#include <lexiglance/core/Error.h>
#include <lexiglance/translate/Tokenizer.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::ocr
{
	class OnnxModel;
}

namespace lexiglance::translate
{

	// A model comes with its weights in full, or quantized to eight bits: a quarter of the download and about half the
	// memory, but on most processors slower, and a little less accurate.
	enum class Precision : std::uint8_t
	{
		Compact,
		Full,
	};

	// As the configuration names them ("compact", "full"); anything else is compact.
	[[nodiscard]] Precision        precisionNamed( std::string_view name );
	[[nodiscard]] std::string_view precisionName( Precision precision );

	// A model's files as its directory keeps them: Hugging Face's names, the ONNX files from its onnx/ folder. Both
	// precisions share the first two.
	inline constexpr std::array<std::string_view, 4> compact_files{ "config.json", "tokenizer.json", "encoder_model_quantized.onnx", "decoder_model_merged_quantized.onnx" };
	inline constexpr std::array<std::string_view, 4> full_files{ "config.json", "tokenizer.json", "encoder_model.onnx", "decoder_model_merged.onnx" };

	[[nodiscard]] constexpr const std::array<std::string_view, 4>& modelFiles( Precision precision )
	{
		return precision == Precision::Full ? full_files : compact_files;
	}

	// The weights of that precision alone: the files the other one does not share, so removing them leaves it working.
	[[nodiscard]] constexpr std::array<std::string_view, 2> weightFiles( Precision precision )
	{
		const auto& files = modelFiles( precision );
		return { files[2], files[3] };
	}

	[[nodiscard]] constexpr Precision otherPrecision( Precision precision )
	{
		return precision == Precision::Full ? Precision::Compact : Precision::Full;
	}

	// Which precision of the model in `directory` is downloaded: `wanted` when it is, else the other one; none when
	// neither is complete.
	[[nodiscard]] std::optional<Precision> downloaded( const std::filesystem::path& directory, Precision wanted );

	// The ordinary state before a download, rather than a fault.
	inline constexpr std::string_view model_absent = "the translation model is not downloaded";

	// An OPUS-MT (Marian) model, run with ONNX Runtime: the encoder reads the text once, then the decoder writes the
	// translation a piece at a time, taking the likeliest piece each time and keeping its cache between them.
	class Model
	{
	public:
		[[nodiscard]] static Result<std::unique_ptr<Model>> load( const std::filesystem::path& directory, Precision precision, const std::filesystem::path& runtime_dir, int threads );

		~Model();

		Model( const Model& )            = delete;
		Model& operator=( const Model& ) = delete;
		Model( Model&& )                 = delete;
		Model& operator=( Model&& )      = delete;

		// The text in the model's target language (English); empty when there is nothing to translate. A sentence takes a
		// few hundred milliseconds: worker threads only, one at a time.
		[[nodiscard]] Result<std::string> translate( std::string_view text );

	private:
		Model() = default;

		std::unique_ptr<ocr::OnnxModel> encoder_;
		std::unique_ptr<ocr::OnnxModel> decoder_;
		std::unique_ptr<Tokenizer>      tokenizer_;
		std::int64_t                    start_ = 0;
		std::int64_t                    eos_   = 0;
		// Pieces never to write (padding).
		std::vector<std::int64_t> banned_;
		int                       layers_    = 0;
		int                       heads_     = 0;
		int                       head_size_ = 0;
		// The decoder's cache, as its inputs and outputs are named, layer by layer.
		std::vector<std::string> past_;
		std::vector<std::string> outputs_;
	};

} // namespace lexiglance::translate

#endif // LEXIGLANCE_TRANSLATE_MODEL_H
