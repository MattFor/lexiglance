#include <lexiglance/ocr/Paddle.h>

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/ocr/Onnx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace lexiglance::ocr
{

	namespace
	{

		constexpr int rec_height    = 48;
		constexpr int rec_min_width = 320;
		constexpr int rec_max_width = 3200;

		// The region of the image a model looks at, possibly rotated a quarter turn counter-clockwise (vertical text).
		struct View
		{
			const Image* image = nullptr;
			Box          box;
			bool         rotate = false;

			[[nodiscard]] int width() const noexcept
			{
				return rotate ? box.height : box.width;
			}

			[[nodiscard]] int height() const noexcept
			{
				return rotate ? box.width : box.height;
			}

			// Channel c of the pixel at (x, y) in view coordinates, sampled bilinearly.
			[[nodiscard]] float sample( double x, double y, int c ) const
			{
				double       sx = x;
				double       sy = y;
				const double w  = box.width;
				if ( rotate )
				{
					sx = w - 1.0 - y;
					sy = x;
				}
				sx += box.x;
				sy += box.y;
				const int    x0 = std::clamp( static_cast<int>( std::floor( sx ) ), 0, image->width - 1 );
				const int    y0 = std::clamp( static_cast<int>( std::floor( sy ) ), 0, image->height - 1 );
				const int    x1 = std::min( x0 + 1, image->width - 1 );
				const int    y1 = std::min( y0 + 1, image->height - 1 );
				const double fx = std::clamp( sx - x0, 0.0, 1.0 );
				const double fy = std::clamp( sy - y0, 0.0, 1.0 );
				const auto   at = [&]( int px, int py ) {
					return static_cast<double>( image->rgb[( ( static_cast<std::size_t>( py ) * static_cast<std::size_t>( image->width ) ) + static_cast<std::size_t>( px ) ) * 3 + static_cast<std::size_t>( c )] );
				};
				const double top    = ( at( x0, y0 ) * ( 1.0 - fx ) ) + ( at( x1, y0 ) * fx );
				const double bottom = ( at( x0, y1 ) * ( 1.0 - fx ) ) + ( at( x1, y1 ) * fx );
				return static_cast<float>( ( top * ( 1.0 - fy ) ) + ( bottom * fy ) );
			}
		};

		// A view resized to width x height into a CHW tensor in BGR order, scaled to [-1, 1]; columns from `content` on
		// stay 0 (padding).
		std::vector<float> tensor( const View& view, int width, int height, int content )
		{
			std::vector<float> out( static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) * 3, 0.0F );
			const double       sx    = static_cast<double>( view.width() ) / std::max( 1, content );
			const double       sy    = static_cast<double>( view.height() ) / height;
			const auto         plane = static_cast<std::size_t>( width ) * static_cast<std::size_t>( height );
			for ( int y = 0; y < height; ++y )
			{
				for ( int x = 0; x < content; ++x )
				{
					const double vx = ( ( x + 0.5 ) * sx ) - 0.5;
					const double vy = ( ( y + 0.5 ) * sy ) - 0.5;
					const auto   i  = ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( width ) ) + static_cast<std::size_t>( x );
					for ( int c = 0; c < 3; ++c )
					{
						// PaddleOCR models were trained on BGR images.
						out[( static_cast<std::size_t>( 2 - c ) * plane ) + i] = ( view.sample( vx, vy, c ) / 127.5F ) - 1.0F;
					}
				}
			}
			return out;
		}

		struct Variant
		{
			char32_t from;
			char32_t to;
		};

		// Simplified Chinese forms the recogniser sometimes prefers (it was trained mostly on Chinese), sorted.
		constexpr std::array simplified = std::to_array<Variant>( {
#include "Simplified.inc"
		} );

		std::string japaneseForm( const std::string& text )
		{
			std::size_t    at = 0;
			const char32_t c  = utf8::decode( text, at );
			if ( at != text.size() )
			{
				return text;
			}
			const auto* const it = std::ranges::lower_bound( simplified, c, {}, &Variant::from );
			return it != simplified.end() && it->from == c ? utf8::fromUtf32( std::u32string( 1, it->to ) ) : text;
		}

		float meanConfidence( const TextLine& line )
		{
			float sum = 0.0F;
			for ( const Character& c : line.characters )
			{
				sum += c.confidence;
			}
			return line.characters.empty() ? 0.0F : sum / static_cast<float>( line.characters.size() );
		}

		int roundTo32( double value )
		{
			return std::max( 32, static_cast<int>( std::lround( value / 32.0 ) ) * 32 );
		}

	} // namespace

	std::vector<Box> textBoxes( std::span<const float> map, int width, int height, float threshold, float box_threshold, double unclip )
	{
		const auto index = [&]( int x, int y ) { return ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( width ) ) + static_cast<std::size_t>( x ); };

		// Threshold, then a 2x2 dilation as PaddleOCR does.
		std::vector<std::uint8_t> bitmap( map.size(), 0 );
		for ( int y = 0; y < height; ++y )
		{
			for ( int x = 0; x < width; ++x )
			{
				if ( map[index( x, y )] > threshold )
				{
					bitmap[index( x, y )] = 1;
					if ( x + 1 < width )
					{
						bitmap[index( x + 1, y )] = 1;
					}
					if ( y + 1 < height )
					{
						bitmap[index( x, y + 1 )] = 1;
					}
					if ( x + 1 < width && y + 1 < height )
					{
						bitmap[index( x + 1, y + 1 )] = 1;
					}
				}
			}
		}

		std::vector<Box>                 boxes;
		std::vector<std::uint8_t>        seen( map.size(), 0 );
		std::vector<std::pair<int, int>> stack;
		for ( int sy = 0; sy < height; ++sy )
		{
			for ( int sx = 0; sx < width; ++sx )
			{
				if ( bitmap[index( sx, sy )] == 0 || seen[index( sx, sy )] != 0 )
				{
					continue;
				}
				int x0 = sx;
				int y0 = sy;
				int x1 = sx;
				int y1 = sy;
				stack.assign( 1, { sx, sy } );
				seen[index( sx, sy )] = 1;
				while ( !stack.empty() )
				{
					const auto [x, y] = stack.back();
					stack.pop_back();
					x0 = std::min( x0, x );
					y0 = std::min( y0, y );
					x1 = std::max( x1, x );
					y1 = std::max( y1, y );
					for ( const auto& [dx, dy] : std::array<std::pair<int, int>, 4>{ { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } } )
					{
						const int nx = x + dx;
						const int ny = y + dy;
						if ( nx >= 0 && ny >= 0 && nx < width && ny < height && bitmap[index( nx, ny )] != 0 && seen[index( nx, ny )] == 0 )
						{
							seen[index( nx, ny )] = 1;
							stack.emplace_back( nx, ny );
						}
					}
				}

				const int w = x1 - x0 + 1;
				const int h = y1 - y0 + 1;
				if ( std::min( w, h ) < 3 )
				{
					continue;
				}
				double sum = 0.0;
				for ( int y = y0; y <= y1; ++y )
				{
					for ( int x = x0; x <= x1; ++x )
					{
						sum += map[index( x, y )];
					}
				}
				const auto score = static_cast<float>( sum / ( static_cast<double>( w ) * h ) );
				if ( score < box_threshold )
				{
					continue;
				}
				// DB unclip: grow by area * ratio / perimeter.
				const double grow = ( static_cast<double>( w ) * h * unclip ) / ( 2.0 * ( w + h ) );
				const int    g    = static_cast<int>( std::lround( grow ) );
				if ( std::min( w, h ) + ( 2 * g ) < 5 )
				{
					continue;
				}
				boxes.push_back( { .x = x0 - g, .y = y0 - g, .width = w + ( 2 * g ), .height = h + ( 2 * g ), .score = score, .inner_x = x0, .inner_y = y0, .inner_width = w, .inner_height = h } );
			}
		}
		return boxes;
	}

	std::vector<Decoded> decodeCtc( std::span<const float> probabilities, int steps, int classes, std::span<const std::string> dictionary )
	{
		std::vector<Decoded> out;
		int                  previous = 0;
		int                  count    = 0;
		for ( int t = 0; t < steps; ++t )
		{
			const auto row  = probabilities.subspan( static_cast<std::size_t>( t ) * static_cast<std::size_t>( classes ), static_cast<std::size_t>( classes ) );
			const auto best = static_cast<int>( std::ranges::max_element( row ) - row.begin() );
			const auto p    = row[static_cast<std::size_t>( best )];
			if ( best != 0 && best == previous && !out.empty() )
			{
				out.back().last = t;
				out.back().confidence += p;
				++count;
			}
			else if ( best != 0 )
			{
				if ( !out.empty() )
				{
					out.back().confidence /= static_cast<float>( count );
				}
				const auto  index = static_cast<std::size_t>( best - 1 );
				std::string text  = index < dictionary.size() ? dictionary[index] : std::string( " " );
				out.push_back( { .text = std::move( text ), .confidence = p, .first = t, .last = t } );
				count = 1;
			}
			previous = best;
		}
		if ( !out.empty() )
		{
			out.back().confidence /= static_cast<float>( count );
		}
		return out;
	}

	Result<std::unique_ptr<PaddleOcr>> PaddleOcr::load( const std::filesystem::path& directory, const std::filesystem::path& runtime_dir, int threads, std::span<const lang::Language* const> languages )
	{
		// Nothing downloaded yet: no runtime is loaded either, so the one downloaded with the models is the one used.
		if ( std::error_code ec; !std::filesystem::exists( directory / "det.onnx", ec ) )
		{
			return fail( "{}", paddle_absent );
		}
		auto detector = OnnxModel::load( directory / "det.onnx", runtime_dir, threads );
		if ( !detector )
		{
			return std::unexpected( detector.error() );
		}
		auto ocr          = std::make_unique<PaddleOcr>();
		ocr->detector_    = std::move( *detector );
		const auto wanted = languages.empty() ? lang::languages() : languages;
		for ( const lang::Language* language : wanted )
		{
			const auto models = language->ocrModels();
			if ( models.paddle.empty() )
			{
				continue;
			}
			// Languages sharing a model share its recogniser.
			const std::string name = models.paddleFile();
			if ( const auto it = std::ranges::find( ocr->recognizers_, name, &Recognizer::file ); it != ocr->recognizers_.end() )
			{
				it->languages.emplace_back( language->code() );
				continue;
			}
			const auto      file = directory / name;
			std::error_code ec;
			if ( !std::filesystem::exists( file, ec ) )
			{
				continue;
			}
			if ( auto own = loadRecognizer( file, runtime_dir, threads ) )
			{
				own->file = name;
				own->languages.emplace_back( language->code() );
				ocr->recognizers_.push_back( std::move( *own ) );
			}
			else
			{
				log::warn( "ocr: {} not loaded: {}", file.filename().string(), own.error().message );
			}
		}
		if ( ocr->recognizers_.empty() || std::ranges::any_of( wanted, []( const lang::Language* language ) { return language->ocrModels().paddle.empty(); } ) )
		{
			auto recognizer = loadRecognizer( directory / "rec.onnx", runtime_dir, threads );
			if ( !recognizer )
			{
				return std::unexpected( recognizer.error() );
			}
			ocr->recognizers_.insert( ocr->recognizers_.begin(), std::move( *recognizer ) );
		}
		return ocr;
	}

	bool PaddleOcr::hasDefaultRecognizer() const noexcept
	{
		return std::ranges::any_of( recognizers_, []( const Recognizer& recognizer ) { return recognizer.languages.empty(); } );
	}

	Result<PaddleOcr::Recognizer> PaddleOcr::loadRecognizer( const std::filesystem::path& file, const std::filesystem::path& runtime_dir, int threads )
	{
		auto model = OnnxModel::load( file, runtime_dir, threads );
		if ( !model )
		{
			return std::unexpected( model.error() );
		}
		Recognizer recognizer{ .model = std::move( *model ), .dictionary = {}, .file = file.filename().string(), .languages = {} };
		// The character list travels inside the recognition model.
		const std::string characters = recognizer.model->metadata( "character" );
		std::size_t       start      = 0;
		while ( start < characters.size() )
		{
			const auto end = characters.find( '\n', start );
			recognizer.dictionary.push_back( characters.substr( start, end == std::string::npos ? std::string::npos : end - start ) );
			start = end == std::string::npos ? characters.size() : end + 1;
		}
		if ( recognizer.dictionary.empty() )
		{
			return fail( "{} carries no character list", file.filename().string() );
		}
		return recognizer;
	}

	std::vector<std::string> PaddleOcr::languages() const
	{
		std::vector<std::string> out;
		for ( const Recognizer& recognizer : recognizers_ )
		{
			out.insert( out.end(), recognizer.languages.begin(), recognizer.languages.end() );
		}
		return out;
	}

	std::vector<Box> PaddleOcr::detect( const Image& image )
	{
		map_.clear();
		if ( image.width < 8 || image.height < 8 )
		{
			return {};
		}
		// Screen text is detected at its own size (half the work of enlarging it); only small regions are enlarged,
		// to about 480 px, and nothing goes beyond 1600 px.
		const double                      scale  = std::min( std::max( 480.0 / std::min( image.width, image.height ), 1.0 ), 1600.0 / std::max( image.width, image.height ) );
		const int                         width  = roundTo32( image.width * scale );
		const int                         height = roundTo32( image.height * scale );
		const View                        view{ .image = &image, .box = { .x = 0, .y = 0, .width = image.width, .height = image.height, .score = 0.0F }, .rotate = false };
		auto                              input = tensor( view, width, height, width );
		const std::array<std::int64_t, 4> shape{ 1, 3, height, width };
		auto                              output = detector_->run( input, shape );
		if ( !output || output->shape.size() != 4 || output->data.size() != static_cast<std::size_t>( output->shape[2] * output->shape[3] ) )
		{
			return {};
		}
		map_height_   = static_cast<int>( output->shape[2] );
		map_width_    = static_cast<int>( output->shape[3] );
		image_width_  = image.width;
		image_height_ = image.height;
		map_          = std::move( output->data );
		return redetect( 0.3F, 0.5F );
	}

	std::vector<Box> PaddleOcr::redetect( float threshold, float box_threshold ) const
	{
		if ( map_.empty() )
		{
			return {};
		}
		auto       boxes = textBoxes( map_, map_width_, map_height_, threshold, box_threshold );
		const auto sx    = static_cast<double>( image_width_ ) / map_width_;
		const auto sy    = static_cast<double>( image_height_ ) / map_height_;
		for ( Box& box : boxes )
		{
			const int x0 = std::clamp( static_cast<int>( std::floor( box.x * sx ) ), 0, image_width_ );
			const int y0 = std::clamp( static_cast<int>( std::floor( box.y * sy ) ), 0, image_height_ );
			const int x1 = std::clamp( static_cast<int>( std::ceil( ( box.x + box.width ) * sx ) ), 0, image_width_ );
			const int y1 = std::clamp( static_cast<int>( std::ceil( ( box.y + box.height ) * sy ) ), 0, image_height_ );
			box          = { .x            = x0,
				             .y            = y0,
				             .width        = x1 - x0,
				             .height       = y1 - y0,
				             .score        = box.score,
				             .inner_x      = static_cast<int>( std::floor( box.inner_x * sx ) ),
				             .inner_y      = static_cast<int>( std::floor( box.inner_y * sy ) ),
				             .inner_width  = static_cast<int>( std::ceil( box.inner_width * sx ) ),
				             .inner_height = static_cast<int>( std::ceil( box.inner_height * sy ) ) };
		}
		std::erase_if( boxes, []( const Box& box ) { return box.width < 4 || box.height < 4; } );
		return boxes;
	}

	TextLine PaddleOcr::recognize( const Image& image, const Box& box, bool prefer_vertical )
	{
		// Each recogniser reads its scripts; the most confident reading is the text's.
		const Recognizer* chosen = nullptr;
		TextLine          best;
		for ( const Recognizer& recognizer : recognizers_ )
		{
			if ( TextLine line = read( recognizer, image, box, prefer_vertical ); chosen == nullptr || meanConfidence( line ) > meanConfidence( best ) )
			{
				best   = std::move( line );
				chosen = &recognizer;
			}
		}
		if ( chosen == nullptr || meanConfidence( best ) >= 0.9F )
		{
			return best;
		}
		// Stylised lettering (thick outlines, shadows, pixel fonts) reads better with another margin around it; the
		// most confident reading wins.
		const int side = std::max( 1, std::min( box.inner_width, box.inner_height ) );
		for ( const double margin : { 0.3, 0.5 } )
		{
			const int pad = static_cast<int>( std::lround( side * margin ) );
			const int x0  = std::max( 0, box.inner_x - pad );
			const int y0  = std::max( 0, box.inner_y - pad );
			const int x1  = std::min( image.width, box.inner_x + box.inner_width + pad );
			const int y1  = std::min( image.height, box.inner_y + box.inner_height + pad );
			Box       other{ box };
			other.x       = x0;
			other.y       = y0;
			other.width   = x1 - x0;
			other.height  = y1 - y0;
			TextLine line = read( *chosen, image, other, prefer_vertical || best.vertical );
			if ( line.vertical == best.vertical && meanConfidence( line ) > meanConfidence( best ) )
			{
				best = std::move( line );
			}
		}
		return best;
	}

	TextLine PaddleOcr::read( const Recognizer& recognizer, const Image& image, const Box& box, bool prefer_vertical )
	{
		TextLine line;
		line.box      = box;
		line.vertical = box.height * 2 >= box.width * 3 || ( prefer_vertical && box.height > box.width );
		const View view{ .image = &image, .box = box, .rotate = line.vertical };
		if ( view.width() < 2 || view.height() < 2 )
		{
			return line;
		}

		const int                         content = std::clamp( static_cast<int>( std::ceil( rec_height * static_cast<double>( view.width() ) / view.height() ) ), 1, rec_max_width );
		const int                         width   = std::max( content, rec_min_width );
		auto                              input   = tensor( view, width, rec_height, content );
		const std::array<std::int64_t, 4> shape{ 1, 3, rec_height, width };
		const auto                        output = recognizer.model->run( input, shape );
		if ( !output || output->shape.size() != 3 )
		{
			return line;
		}
		const int  steps   = static_cast<int>( output->shape[1] );
		const int  classes = static_cast<int>( output->shape[2] );
		const auto decoded = decodeCtc( output->data, steps, classes, recognizer.dictionary );

		// Time steps map linearly onto the input width, of which `content` columns are the view.
		const double        step_px = static_cast<double>( width ) / steps * view.width() / content;
		const double        origin  = line.vertical ? box.y : box.x;
		const double        length  = view.width();
		std::vector<double> centres;
		centres.reserve( decoded.size() );
		for ( const Decoded& d : decoded )
		{
			centres.push_back( ( ( d.first + d.last + 1 ) / 2.0 ) * step_px );
		}
		for ( std::size_t i = 0; i < decoded.size(); ++i )
		{
			const double previous = i > 0 ? centres[i - 1] : ( centres[i] - ( i + 1 < centres.size() ? centres[i + 1] - centres[i] : centres[i] ) );
			const double next     = i + 1 < centres.size() ? centres[i + 1] : ( centres[i] + ( centres[i] - previous ) );
			const double from     = std::clamp( ( previous + centres[i] ) / 2.0, 0.0, length );
			const double to       = std::clamp( ( centres[i] + next ) / 2.0, 0.0, length );
			line.characters.push_back( { .text = japaneseForm( decoded[i].text ), .confidence = decoded[i].confidence, .from = static_cast<float>( origin + from ), .to = static_cast<float>( origin + to ) } );
		}
		return line;
	}

} // namespace lexiglance::ocr
