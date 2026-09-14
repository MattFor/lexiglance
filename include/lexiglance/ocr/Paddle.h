#ifndef LEXIGLANCE_OCR_PADDLE_H
#define LEXIGLANCE_OCR_PADDLE_H

#include <lexiglance/core/Error.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/ocr/Onnx.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lexiglance::ocr
{

	// RGB pixels, row by row.
	struct Image
	{
		int                       width  = 0;
		int                       height = 0;
		std::vector<std::uint8_t> rgb;
	};

	struct Box
	{
		int   x      = 0;
		int   y      = 0;
		int   width  = 0;
		int   height = 0;
		float score  = 0.0F;
		// The detected text itself, before the margin recognition needs.
		int inner_x      = 0;
		int inner_y      = 0;
		int inner_width  = 0;
		int inner_height = 0;
	};

	// A recognised character and its extent along the line, in image coordinates.
	struct Character
	{
		std::string text;
		float       confidence = 0.0F;
		float       from       = 0.0F;
		float       to         = 0.0F;
	};

	struct TextLine
	{
		Box                    box;
		bool                   vertical = false;
		std::vector<Character> characters;
	};

	struct Decoded
	{
		std::string text;
		float       confidence = 0.0F;
		int         first      = 0;
		int         last       = 0;
	};

	// DB post-processing of a text probability map: connected regions above `threshold` whose mean probability reaches
	// `box_threshold`, enlarged by the unclip ratio. Coordinates are those of the map.
	[[nodiscard]] std::vector<Box> textBoxes( std::span<const float> map, int width, int height, float threshold = 0.3F, float box_threshold = 0.5F, double unclip = 1.6 );

	// CTC greedy decoding of per step class probabilities (class 0 is the blank, the last one a space).
	[[nodiscard]] std::vector<Decoded> decodeCtc( std::span<const float> probabilities, int steps, int classes, std::span<const std::string> dictionary );

	// PaddleOCR (PP-OCR) text detection and recognition. Not thread-safe.
	class PaddleOcr
	{
	public:
		// `directory` holds det.onnx and rec.onnx, and the recognisers of languages rec.onnx cannot read (named by
		// lang::OcrModels::paddleFile) when they are installed; ONNX Runtime is looked for in `runtime_dir`.
		// Only what `languages` need is loaded (empty: every language); the default recogniser reads those without one of
		// their own, and anything when none of theirs is installed.
		[[nodiscard]] static Result<std::unique_ptr<PaddleOcr>> load( const std::filesystem::path& directory, const std::filesystem::path& runtime_dir, int threads, std::span<const lang::Language* const> languages = {} );

		// The languages with a recogniser of their own that is loaded.
		[[nodiscard]] std::vector<std::string> languages() const;

		// Whether the default recogniser (Chinese, Japanese, English) is loaded.
		[[nodiscard]] bool hasDefaultRecognizer() const noexcept;

		// Text boxes in image coordinates.
		[[nodiscard]] std::vector<Box> detect( const Image& image );

		// The last detection with other thresholds (fainter or sparser text, more noise), without running the model again.
		[[nodiscard]] std::vector<Box> redetect( float threshold, float box_threshold ) const;

		// Reads one box with every recogniser, keeping the most confident reading (which tells the script); boxes
		// clearly taller than wide are read as vertical text.
		[[nodiscard]] TextLine recognize( const Image& image, const Box& box, bool prefer_vertical = false );

	private:
		struct Recognizer
		{
			std::unique_ptr<OnnxModel> model;
			std::vector<std::string>   dictionary;
			std::string                file;
			// The languages it reads; none for the default recogniser.
			std::vector<std::string> languages;
		};

		[[nodiscard]] static Result<Recognizer> loadRecognizer( const std::filesystem::path& file, const std::filesystem::path& runtime_dir, int threads );
		[[nodiscard]] static TextLine           read( const Recognizer& recognizer, const Image& image, const Box& box, bool prefer_vertical );

		std::unique_ptr<OnnxModel> detector_;
		std::vector<Recognizer>    recognizers_;
		std::vector<float>         map_;
		int                        map_width_    = 0;
		int                        map_height_   = 0;
		int                        image_width_  = 0;
		int                        image_height_ = 0;
	};

} // namespace lexiglance::ocr

#endif // LEXIGLANCE_OCR_PADDLE_H
