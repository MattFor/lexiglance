#include <lexiglance/translate/Model.h>

#include <lexiglance/core/Json.h>
#include <lexiglance/ocr/Onnx.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>

namespace lexiglance::translate
{

	namespace
	{

		// Longer input is cut there (a screenful of text, not a book).
		constexpr std::size_t max_input = 256;

		Result<std::string> readFile( const std::filesystem::path& path )
		{
			const std::ifstream in( path, std::ios::binary );
			if ( !in )
			{
				return fail( "cannot read {}", path.filename().string() );
			}
			std::ostringstream text;
			text << in.rdbuf();
			return text.str();
		}

		// The pieces that would repeat a run of three already written: greedy decoding otherwise tends to loop ("the the
		// the").
		void repeating( const std::vector<std::int64_t>& written, std::vector<std::int64_t>& out )
		{
			const std::size_t n = written.size();
			for ( std::size_t i = 0; n >= 2 && i + 2 < n; ++i )
			{
				if ( written[i] == written[n - 2] && written[i + 1] == written[n - 1] )
				{
					out.push_back( written[i + 2] );
				}
			}
		}

		bool complete( const std::filesystem::path& directory, Precision precision )
		{
			return std::ranges::all_of( modelFiles( precision ), [&]( std::string_view file ) {
				std::error_code ec;
				return std::filesystem::exists( directory / file, ec );
			} );
		}

	} // namespace

	Precision precisionNamed( std::string_view name )
	{
		return name == "full" ? Precision::Full : Precision::Compact;
	}

	std::string_view precisionName( Precision precision )
	{
		return precision == Precision::Full ? "full" : "compact";
	}

	std::optional<Precision> downloaded( const std::filesystem::path& directory, Precision wanted )
	{
		const Precision other = wanted == Precision::Full ? Precision::Compact : Precision::Full;
		if ( complete( directory, wanted ) )
		{
			return wanted;
		}
		if ( complete( directory, other ) )
		{
			return other;
		}
		return std::nullopt;
	}

	Model::~Model() = default;

	Result<std::unique_ptr<Model>> Model::load( const std::filesystem::path& directory, Precision precision, const std::filesystem::path& runtime_dir, int threads )
	{
		if ( !complete( directory, precision ) )
		{
			return fail( "{}", model_absent );
		}
		const auto&            files = modelFiles( precision );
		std::unique_ptr<Model> model( new Model() );

		auto config_text = readFile( directory / "config.json" );
		if ( !config_text )
		{
			return std::unexpected( config_text.error() );
		}
		auto config = json::Document::parse( std::move( *config_text ) );
		if ( !config )
		{
			return fail( "config.json: {}", config.error().message );
		}
		const json::Value& c = config->root();
		if ( c["model_type"].asString() != "marian" )
		{
			return fail( "{} is not an OPUS-MT (Marian) model", directory.filename().string() );
		}
		model->start_    = c["decoder_start_token_id"].asInt( c["pad_token_id"].asInt( 0 ) );
		model->eos_      = c["eos_token_id"].asInt( 0 );
		model->layers_   = static_cast<int>( c["decoder_layers"].asInt( 0 ) );
		model->heads_    = static_cast<int>( c["decoder_attention_heads"].asInt( 0 ) );
		const auto width = c["d_model"].asInt( 0 );
		if ( model->layers_ <= 0 || model->heads_ <= 0 || width <= 0 || width % model->heads_ != 0 )
		{
			return fail( "config.json does not describe the decoder" );
		}
		model->head_size_ = static_cast<int>( width / model->heads_ );
		model->banned_.push_back( c["pad_token_id"].asInt( model->start_ ) );
		for ( const json::Value& words : c["bad_words_ids"].items() )
		{
			if ( words.size() == 1 )
			{
				model->banned_.push_back( words[0].asInt( -1 ) );
			}
		}

		auto tokenizer_text = readFile( directory / "tokenizer.json" );
		if ( !tokenizer_text )
		{
			return std::unexpected( tokenizer_text.error() );
		}
		auto tokenizer = Tokenizer::parse( std::move( *tokenizer_text ) );
		if ( !tokenizer )
		{
			return fail( "tokenizer.json: {}", tokenizer.error().message );
		}
		model->tokenizer_ = std::make_unique<Tokenizer>( std::move( *tokenizer ) );

		auto encoder = ocr::OnnxModel::load( directory / files[2], runtime_dir, threads );
		if ( !encoder )
		{
			return std::unexpected( encoder.error() );
		}
		auto decoder = ocr::OnnxModel::load( directory / files[3], runtime_dir, threads );
		if ( !decoder )
		{
			return std::unexpected( decoder.error() );
		}
		model->encoder_ = std::move( *encoder );
		model->decoder_ = std::move( *decoder );

		// The cache, as the decoder names it: its own keys and values, and those of the encoder's output, per layer.
		model->outputs_.emplace_back( "logits" );
		for ( int layer = 0; layer < model->layers_; ++layer )
		{
			for ( const std::string_view part : { "decoder.key", "decoder.value", "encoder.key", "encoder.value" } )
			{
				model->past_.push_back( std::format( "past_key_values.{}.{}", layer, part ) );
				model->outputs_.push_back( std::format( "present.{}.{}", layer, part ) );
			}
		}
		const auto inputs  = model->decoder_->inputNames();
		const auto outputs = model->decoder_->outputNames();
		for ( const std::string_view needed : { "input_ids", "encoder_hidden_states", "encoder_attention_mask", "use_cache_branch" } )
		{
			if ( !std::ranges::contains( inputs, needed ) )
			{
				return fail( "{} lacks the input {} (it must be a merged decoder)", files[3], needed );
			}
		}
		if ( !std::ranges::all_of( model->past_, [&]( const std::string& name ) { return std::ranges::contains( inputs, name ); } ) ||
		     !std::ranges::all_of( model->outputs_, [&]( const std::string& name ) { return std::ranges::contains( outputs, name ); } ) )
		{
			return fail( "{} does not have the {} layers config.json gives", files[3], model->layers_ );
		}
		return model;
	}

	Result<std::string> Model::translate( std::string_view text )
	{
		using ocr::Input;
		auto ids = tokenizer_->encode( text );
		if ( ids.size() <= 1 )
		{
			return std::string();
		}
		if ( ids.size() > max_input )
		{
			ids.resize( max_input - 1 );
			ids.push_back( tokenizer_->endOfSequence() );
		}
		const auto                        length = static_cast<std::int64_t>( ids.size() );
		const std::vector<std::int64_t>   mask( ids.size(), 1 );
		const std::array<std::int64_t, 2> sequence{ 1, length };

		const std::array encoder_inputs{
			Input{ .name = "input_ids", .type = Input::Type::Int64, .shape = sequence, .data = ids.data(), .bytes = ids.size() * sizeof( std::int64_t ) },
			Input{ .name = "attention_mask", .type = Input::Type::Int64, .shape = sequence, .data = mask.data(), .bytes = mask.size() * sizeof( std::int64_t ) },
		};
		const std::array<std::string, 1> encoder_output{ "last_hidden_state" };
		auto                             encoded = encoder_->run( encoder_inputs, encoder_output );
		if ( !encoded )
		{
			return fail( "encoder: {}", encoded.error().message );
		}
		const ocr::Tensor& hidden = encoded->front();

		// Empty to begin with: the first step fills it.
		std::vector<ocr::Tensor> cache( past_.size() );
		for ( ocr::Tensor& tensor : cache )
		{
			tensor.shape = { 1, heads_, 0, head_size_ };
		}

		std::vector<std::int64_t> written{ start_ };
		const std::size_t         most = std::min<std::size_t>( max_input, ( ids.size() * 3 ) + 10 );
		for ( std::size_t step = 0; step < most; ++step )
		{
			const std::int64_t                token = written.back();
			const std::array<std::int64_t, 2> one{ 1, 1 };
			const std::array<std::int64_t, 1> flag_shape{ 1 };
			const bool                        cached = step > 0;

			std::vector<Input> inputs{
				Input{ .name = "encoder_attention_mask", .type = Input::Type::Int64, .shape = sequence, .data = mask.data(), .bytes = mask.size() * sizeof( std::int64_t ) },
				Input{ .name = "input_ids", .type = Input::Type::Int64, .shape = one, .data = &token, .bytes = sizeof( token ) },
				Input{ .name = "encoder_hidden_states", .type = Input::Type::Float, .shape = hidden.shape, .data = hidden.data.data(), .bytes = hidden.data.size() * sizeof( float ) },
				Input{ .name = "use_cache_branch", .type = Input::Type::Bool, .shape = flag_shape, .data = &cached, .bytes = sizeof( cached ) },
			};
			for ( std::size_t i = 0; i < past_.size(); ++i )
			{
				inputs.push_back( { .name = past_[i], .type = Input::Type::Float, .shape = cache[i].shape, .data = cache[i].data.data(), .bytes = cache[i].data.size() * sizeof( float ) } );
			}
			auto result = decoder_->run( inputs, outputs_ );
			if ( !result )
			{
				return fail( "decoder: {}", result.error().message );
			}

			// The likeliest next piece, from the logits of the last position, the forbidden ones left out.
			ocr::Tensor& logits = result->front();
			if ( logits.shape.size() != 3 || logits.shape[2] <= 0 || logits.data.size() < static_cast<std::size_t>( logits.shape[2] ) )
			{
				return fail( "decoder: unexpected logits" );
			}
			const auto                classes = static_cast<std::size_t>( logits.shape[2] );
			const std::span<float>    row( logits.data.data() + ( logits.data.size() - classes ), classes );
			std::vector<std::int64_t> forbidden = banned_;
			repeating( written, forbidden );
			for ( const std::int64_t id : forbidden )
			{
				if ( id >= 0 && static_cast<std::size_t>( id ) < classes )
				{
					row[static_cast<std::size_t>( id )] = -std::numeric_limits<float>::infinity();
				}
			}
			const auto         likeliest = std::ranges::max_element( row );
			const std::int64_t next      = *likeliest > -std::numeric_limits<float>::infinity() ? likeliest - row.begin() : -1;
			if ( next < 0 || next == eos_ )
			{
				break;
			}
			written.push_back( next );

			// The decoder's own cache grows every step; the encoder's is made once, on the first.
			for ( std::size_t i = 0; i < past_.size(); ++i )
			{
				const bool encoder_part = i % 4 >= 2;
				if ( !encoder_part || step == 0 )
				{
					cache[i] = std::move( ( *result )[i + 1] );
				}
			}
		}
		return tokenizer_->decode( std::span( written ).subspan( 1 ) );
	}

} // namespace lexiglance::translate
