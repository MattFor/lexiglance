#include "TranslationInstall.h"

#include "Common.h"
#include "DaemonClient.h"
#include "OcrInstall.h"
#include "VcRedist.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/translate/Model.h>

#include <QDir>
#include <QFile>

#include <algorithm>
#include <memory>

namespace lexiglance::gui::translation_install
{

	namespace
	{

		// Hugging Face keeps the ONNX files in the repository's onnx/ folder; they are kept beside the others here.
		QUrl address( const lang::TranslationModel& model, std::string_view file )
		{
			const QString folder = file.ends_with( ".onnx" ) ? QStringLiteral( "onnx/" ) : QString();
			return { QStringLiteral( "https://huggingface.co/%1/resolve/%2/%3%4" ).arg( qs( model.repository ), qs( model.revision ), folder, qs( file ) ) };
		}

	} // namespace

	QString folder( const lang::Language& language )
	{
		return qs( ( paths::translationDir() / language.translationModel().directory() ).string() );
	}

	QString remove( const lang::Language& language, std::optional<translate::Precision> precision )
	{
		if ( language.translationModel().empty() )
		{
			return {};
		}
		QDir where( folder( language ) );
		// One precision only, while the other one is there to translate with: its weights go, the files both use stay.
		if ( precision && installed( language, translate::otherPrecision( *precision ) ) )
		{
			for ( const std::string_view file : translate::weightFiles( *precision ) )
			{
				const QString path = where.absolutePath() + "/" + qs( file );
				if ( QFile::exists( path ) && !QFile::remove( path ) )
				{
					return QStringLiteral( "Cannot delete %1" ).arg( QDir::toNativeSeparators( path ) );
				}
			}
			return {};
		}
		if ( where.exists() && !where.removeRecursively() )
		{
			return QStringLiteral( "Cannot delete %1" ).arg( QDir::toNativeSeparators( where.absolutePath() ) );
		}
		return {};
	}

	bool installed( const lang::Language& language, translate::Precision precision )
	{
		if ( language.translationModel().empty() )
		{
			return false;
		}
		const QString where = folder( language );
		return std::ranges::all_of( translate::modelFiles( precision ), [&]( std::string_view file ) { return QFile::exists( where + "/" + qs( file ) ); } );
	}

	std::optional<translate::Precision> present( const lang::Language& language, translate::Precision wanted )
	{
		if ( language.translationModel().empty() )
		{
			return std::nullopt;
		}
		return translate::downloaded( paths::translationDir() / language.translationModel().directory(), wanted );
	}

	std::uint64_t size( const lang::TranslationModel& model, translate::Precision precision )
	{
		return precision == translate::Precision::Full && model.full_bytes > 0 ? model.full_bytes : model.bytes;
	}

	translate::Precision precisionFor( const lang::Language& language, const config::TranslationSettings& settings )
	{
		const auto asked = translate::precisionNamed( settings.modelFor( language.code() ) );
		if ( std::ranges::any_of( settings.models, [&]( const config::LanguageModel& chosen ) { return chosen.language == language.code(); } ) )
		{
			return asked;
		}
		return present( language, asked ).value_or( asked );
	}

	std::vector<Wanted> wanted( std::span<const lang::Language* const> languages, const config::TranslationSettings& settings )
	{
		std::vector<Wanted> out;
		for ( const lang::Language* language : languages )
		{
			out.push_back( { .language = language, .precision = precisionFor( *language, settings ) } );
		}
		return out;
	}

	std::vector<Wanted> distinct( std::span<const Wanted> models )
	{
		std::vector<Wanted> out;
		for ( const Wanted& model : models )
		{
			if ( model.language == nullptr || model.language->translationModel().empty() )
			{
				continue;
			}
			const std::string directory = model.language->translationModel().directory();
			if ( std::ranges::none_of( out, [&]( const Wanted& other ) { return other.precision == model.precision && other.language->translationModel().directory() == directory; } ) )
			{
				out.push_back( model );
			}
		}
		return out;
	}

	std::vector<const lang::Language*> withModel( std::span<const lang::Language* const> languages )
	{
		std::vector<const lang::Language*> out;
		for ( const lang::Language* language : languages )
		{
			const auto& model = language->translationModel();
			if ( !model.empty() && std::ranges::none_of( out, [&]( const lang::Language* other ) { return other->translationModel().directory() == model.directory(); } ) )
			{
				out.push_back( language );
			}
		}
		return out;
	}

	std::vector<std::pair<QUrl, QString>> files( std::span<const Wanted> models, bool again )
	{
		std::vector<std::pair<QUrl, QString>> out;
		for ( const Wanted& model : distinct( models ) )
		{
			if ( !again && installed( *model.language, model.precision ) )
			{
				continue;
			}
			const QString where = folder( *model.language );
			for ( const std::string_view file : translate::modelFiles( model.precision ) )
			{
				const QString target = where + "/" + qs( file );
				if ( ( again || !QFile::exists( target ) ) && std::ranges::none_of( out, [&]( const auto& queued ) { return queued.second == target; } ) )
				{
					out.emplace_back( address( model.language->translationModel(), file ), target );
				}
			}
		}
		return out;
	}

	std::uint64_t bytes( std::span<const Wanted> models, bool again )
	{
		std::uint64_t total = 0;
		for ( const Wanted& model : distinct( models ) )
		{
			if ( again || !installed( *model.language, model.precision ) )
			{
				total += size( model.language->translationModel(), model.precision );
			}
		}
		return total;
	}

	QString megabytes( std::uint64_t bytes )
	{
		return QStringLiteral( "%1 MB" ).arg( ( bytes + 500'000 ) / 1'000'000 );
	}

	Installer::Installer( DaemonClient* client, QWidget* owner ) :
		QObject( owner ),
		client_( client ),
		owner_( owner ),
		downloader_( new Downloader( this ) )
	{
	}

	void Installer::install( std::vector<Wanted> models, bool again, const Progress& progress, Done done )
	{
		if ( busy_ )
		{
			if ( done )
			{
				done( QStringLiteral( "a download is already running" ) );
			}
			return;
		}
		auto list = files( models, again );
		// The same runtime as OCR's, which may have brought it already.
		const QString runtime = ocr_install::ocrPath( "runtime" );
		const QString archive = ocr_install::runtimeArchive();
		const bool    unpack  = !archive.isEmpty() && !QFile::exists( runtime + "/" + ocr_install::runtimeLibraryName() );
		QString       redist;
		ocr_install::appendRuntime( list, &redist, owner_, false );
		busy_ = true;
		done_ = std::move( done );
		if ( list.empty() )
		{
			client_->call( "translation.reload" );
			finish( {} );
			return;
		}
		log::info( "translation: downloading {} files", list.size() );

		auto remaining = std::make_shared<std::size_t>( list.size() );
		auto failure   = std::make_shared<QString>();
		// Each file's bytes so far and in all: the files come side by side, and the progress is theirs together.
		auto sizes = std::make_shared<std::vector<std::pair<qint64, qint64>>>( list.size() );
		for ( std::size_t index = 0; index < list.size(); ++index )
		{
			downloader_->download(
					list[index].first,
					list[index].second,
					[sizes, index, progress]( qint64 received, qint64 total ) {
						( *sizes )[index] = { received, total };
						qint64 done_bytes = 0;
						qint64 all        = 0;
						bool   known      = true;
						for ( const auto& [file_received, file_total] : *sizes )
						{
							done_bytes += file_received;
							all += file_total;
							known = known && file_total > 0;
						}
						if ( progress )
						{
							progress( done_bytes, known ? all : 0 );
						}
					},
					[this, remaining, failure, runtime, archive, unpack, redist]( const QString& error ) {
						if ( !error.isEmpty() )
						{
							*failure = error;
						}
						if ( --*remaining > 0 )
						{
							return;
						}
						if ( failure->isEmpty() && unpack )
						{
							*failure = ocr_install::unpackRuntime( runtime, archive );
						}
						if ( !failure->isEmpty() )
						{
							QFile::remove( redist );
							finish( *failure );
							return;
						}
						// Microsoft's runtime, which came along: installed, then the daemon starts again to find it.
						if ( !redist.isEmpty() )
						{
							vcredist::install( this, redist, [this]( const QString& problem ) {
								if ( problem.isEmpty() )
								{
									client_->startDaemon( true );
								}
								finish( problem );
							} );
							return;
						}
						client_->call( "translation.reload" );
						finish( {} );
					}
			);
		}
	}

	void Installer::finish( const QString& error )
	{
		busy_ = false;
		if ( !error.isEmpty() )
		{
			log::warn( "translation: download failed: {}", ss( error ) );
		}
		if ( auto done = std::exchange( done_, {} ) )
		{
			done( error );
		}
	}

} // namespace lexiglance::gui::translation_install
