#include "ReportOverlay.h"

#include "Common.h"
#include "DaemonClient.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Version.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPixmap>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <utility>

namespace lexiglance::gui
{

	namespace
	{

		constexpr std::string_view picture_name = "popup.png";

		void keep( std::optional<json::Document>& into, const json::Value* result )
		{
			if ( result != nullptr )
			{
				if ( auto document = json::Document::parse( json::serialize( *result ) ) )
				{
					into = std::move( *document );
				}
			}
		}

		QLabel* wrapped( const QString& text )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			return label;
		}

	} // namespace

	report::Input Gathered::input( const QString& what ) const
	{
		return {
			.what         = ss( what.trimmed() ),
			.program      = std::format( "Lexiglance {} ({} build{}), settings application on Qt {}", version, channel, commit.empty() ? std::string() : " " + std::string( commit ), qVersion() ),
			.status       = status ? &status->root() : nullptr,
			.health       = health ? &health->root() : nullptr,
			.dictionaries = dictionaries ? &dictionaries->root() : nullptr,
		};
	}

	std::vector<report::Part> Gathered::parts( const QString& what, bool with_picture, bool extras ) const
	{
		auto out = report::parts( input( what ) );
		if ( extras )
		{
			out.insert( out.end(), extra.begin(), extra.end() );
		}
		if ( with_picture && !picture.empty() )
		{
			out.push_back( { .name = std::string( picture_name ), .title = "A picture of the latest popup", .contents = picture } );
		}
		return out;
	}

	void gatherReport( DaemonClient& client, std::vector<report::Part> extra, std::function<void( std::shared_ptr<const Gathered> )> done )
	{
		auto gathered      = std::make_shared<Gathered>();
		gathered->extra    = std::move( extra );
		auto       pending = std::make_shared<int>( 4 );
		const auto finish  = [gathered, pending, done = std::move( done )] {
			if ( --*pending == 0 )
			{
				done( gathered );
			}
		};
		client.call( "status", "{}", [gathered, finish]( const json::Value* result, const QString& ) {
			keep( gathered->status, result );
			finish();
		} );
		client.call(
				"health",
				"{\"interactive\":false}",
				[gathered, finish]( const json::Value* result, const QString& ) {
					keep( gathered->health, result );
					finish();
				},
				40000
		);
		client.call( "dictionaries.list", "{}", [gathered, finish]( const json::Value* result, const QString& ) {
			keep( gathered->dictionaries, result );
			finish();
		} );
		client.call( "popup.preview", "{\"last\":true}", [gathered, finish]( const json::Value* result, const QString& ) {
			if ( result != nullptr )
			{
				const auto png    = ( *result )["png"].asString();
				gathered->picture = QByteArray::fromBase64( QByteArray( png.data(), static_cast<qsizetype>( png.size() ) ) ).toStdString();
			}
			finish();
		} );
	}

	QString saveReport( const std::vector<report::Part>& parts, QString* error )
	{
		QString folder;
		for ( const auto location : { QStandardPaths::DownloadLocation, QStandardPaths::DesktopLocation, QStandardPaths::HomeLocation } )
		{
			folder = QStandardPaths::writableLocation( location );
			if ( !folder.isEmpty() && QFileInfo( folder ).isDir() )
			{
				break;
			}
		}
		const QString file  = QDir( folder ).filePath( qs( report::fileName( std::chrono::system_clock::now() ) ) );
		const auto    saved = report::save( std::filesystem::path( ss( file ) ), parts );
		if ( !saved )
		{
			if ( error != nullptr )
			{
				*error = qs( saved.error().message );
			}
			return {};
		}
		log::info( "problem report saved as {}", ss( file ) );
		return file;
	}

	ReportOverlay::ReportOverlay( DaemonClient& client, std::function<std::vector<report::Part>()> extra, QWidget* parent ) :
		CardOverlay( 680, parent ),
		client_( &client ),
		extra_( std::move( extra ) ),
		what_( new QPlainTextEdit() ),
		picture_( new QCheckBox( QStringLiteral( "Include a picture of the latest popup" ) ) ),
		extras_( new QCheckBox( QStringLiteral( "Include the Developer page's trace" ) ) ),
		contents_( new QLabel() ),
		form_( new QWidget() ),
		viewer_( new QWidget() ),
		part_title_( new QLabel() ),
		part_text_( new QPlainTextEdit() ),
		part_image_( new QLabel() ),
		part_image_scroll_( new QScrollArea() ),
		status_( new QLabel() ),
		send_( new QPushButton( QStringLiteral( "Save and open GitHub" ) ) ),
		cancel_( new QPushButton( QStringLiteral( "Cancel" ) ) )
	{
		auto* layout = cardLayout();
		layout->addWidget( cardTitle( QStringLiteral( "Report a problem" ) ) );
		layout->addWidget( wrapped( QStringLiteral( "Lexiglance doesn't send anything by itself. This saves a report on your computer and opens GitHub's issue "
		                                            "form with the main facts filled in, for you to drag the report into. You can check what it holds below." ) ) );

		auto* form_layout = new QVBoxLayout( form_ );
		form_layout->setContentsMargins( 0, 0, 0, 0 );
		form_layout->setSpacing( 8 );
		what_->setPlaceholderText( QStringLiteral( "What happened? What did you do, what did you expect, and what happened instead?" ) );
		what_->setFixedHeight( 96 );
		form_layout->addWidget( what_ );
		picture_->setToolTip( QStringLiteral( "The popup drawn again as it was: the word and the entries it showed, nothing else from your screen" ) );
		form_layout->addWidget( picture_ );
		extras_->setToolTip( QStringLiteral( "Every lookup since the Developer page was opened: the text read, where, in which window, and how long each step took" ) );
		form_layout->addWidget( extras_ );
		contents_->setTextFormat( Qt::RichText );
		contents_->setWordWrap( true );
		form_layout->addWidget( contents_ );
		layout->addWidget( form_ );

		auto* viewer_layout = new QVBoxLayout( viewer_ );
		viewer_layout->setContentsMargins( 0, 0, 0, 0 );
		auto* viewer_header = new QHBoxLayout();
		part_title_->setWordWrap( true );
		viewer_header->addWidget( part_title_, 1 );
		auto* back = new QPushButton( QStringLiteral( "Back" ) );
		viewer_header->addWidget( back );
		viewer_layout->addLayout( viewer_header );
		part_text_->setReadOnly( true );
		part_text_->setLineWrapMode( QPlainTextEdit::NoWrap );
		part_text_->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
		part_text_->setMinimumHeight( 300 );
		viewer_layout->addWidget( part_text_ );
		part_image_->setAlignment( Qt::AlignCenter );
		part_image_scroll_->setWidget( part_image_ );
		part_image_scroll_->setWidgetResizable( true );
		part_image_scroll_->setMinimumHeight( 300 );
		viewer_layout->addWidget( part_image_scroll_ );
		viewer_->hide();
		layout->addWidget( viewer_ );

		auto* footer = new QHBoxLayout();
		status_->setWordWrap( true );
		status_->setTextFormat( Qt::RichText );
		footer->addWidget( status_, 1 );
		footer->addWidget( cancel_ );
		send_->setProperty( "primary", true );
		footer->addWidget( send_ );
		layout->addLayout( footer );

		connect( contents_, &QLabel::linkActivated, this, [this]( const QString& link ) { showPart( link.toInt() ); } );
		connect( back, &QPushButton::clicked, this, [this] { showList(); } );
		connect( picture_, &QCheckBox::toggled, this, [this] { showList(); } );
		connect( extras_, &QCheckBox::toggled, this, [this] { showList(); } );
		connect( cancel_, &QPushButton::clicked, this, [this] { hide(); } );
		connect( send_, &QPushButton::clicked, this, [this] {
			if ( saved_.isEmpty() )
			{
				send();
			}
			else
			{
				hide();
			}
		} );
		connect( status_, &QLabel::linkActivated, this, []( const QString& link ) { QDesktopServices::openUrl( QUrl( link ) ); } );
	}

	void ReportOverlay::refit()
	{
		// The layouts keep the heights they worked out until the changes inside have been laid out, which Qt does later.
		QCoreApplication::sendPostedEvents( nullptr, QEvent::LayoutRequest );
		place();
	}

	void ReportOverlay::open()
	{
		saved_.clear();
		gathered_.reset();
		send_->setText( QStringLiteral( "Save and open GitHub" ) );
		send_->setEnabled( false );
		cancel_->show();
		status_->setText( QStringLiteral( "Collecting the status and the health report..." ) );
		contents_->setText( QStringLiteral( "The report holds: ..." ) );
		form_->show();
		viewer_->hide();
		// Nothing more than what is needed unless ticked, each time.
		picture_->setChecked( false );
		extras_->setChecked( false );
		extras_->hide();
		popUp();
		what_->setFocus();
		gatherReport( *client_, extra_ ? extra_() : std::vector<report::Part>{}, [this]( std::shared_ptr<const Gathered> gathered ) {
			gathered_ = std::move( gathered );
			picture_->setEnabled( !gathered_->picture.empty() );
			extras_->setVisible( !gathered_->extra.empty() );
			status_->setText( gathered_->status ? QString() : QStringLiteral( "Lexiglance isn't running, so the report holds only what can be found without it." ) );
			send_->setEnabled( true );
			showList();
		} );
	}

	void ReportOverlay::showList()
	{
		viewer_->hide();
		form_->setVisible( saved_.isEmpty() );
		if ( !gathered_ )
		{
			return;
		}
		const QString muted = palette().color( QPalette::PlaceholderText ).name();
		QString       html  = QStringLiteral( "<p style=\"margin-bottom: 4px\">The report holds:</p><table cellspacing=\"0\" cellpadding=\"2\">" );
		const auto    parts = gathered_->parts( what_->toPlainText(), picture_->isChecked(), extras_->isChecked() );
		for ( std::size_t i = 0; i < parts.size(); ++i )
		{
			html += QStringLiteral( "<tr><td style=\"padding-right: 12px\">%1</td><td style=\"padding-right: 12px; color: %2\">%3</td><td><a href=\"%4\">View</a></td></tr>" )
			                .arg( qs( parts[i].title ).toHtmlEscaped(), muted, qs( parts[i].name ).toHtmlEscaped() )
			                .arg( i );
		}
		contents_->setText( html + QStringLiteral( "</table>" ) );
		refit();
	}

	void ReportOverlay::showPart( int index )
	{
		if ( !gathered_ )
		{
			return;
		}
		const auto parts = gathered_->parts( what_->toPlainText(), picture_->isChecked(), extras_->isChecked() );
		if ( index < 0 || static_cast<std::size_t>( index ) >= parts.size() )
		{
			return;
		}
		const auto& part  = parts[static_cast<std::size_t>( index )];
		const bool  image = part.name.ends_with( ".png" );
		part_title_->setText( QStringLiteral( "<b>%1</b> · %2" ).arg( qs( part.name ).toHtmlEscaped(), qs( part.title ).toHtmlEscaped() ) );
		part_text_->setVisible( !image );
		part_image_scroll_->setVisible( image );
		if ( image )
		{
			QPixmap pixmap;
			pixmap.loadFromData( reinterpret_cast<const uchar*>( part.contents.data() ), static_cast<uint>( part.contents.size() ), "PNG" );
			part_image_->setPixmap( pixmap );
		}
		else
		{
			part_text_->setPlainText( qs( part.contents ) );
		}
		form_->hide();
		viewer_->show();
		refit();
	}

	void ReportOverlay::send()
	{
		if ( !gathered_ )
		{
			return;
		}
		const QString what  = what_->toPlainText().trimmed();
		const auto    parts = gathered_->parts( what, picture_->isChecked(), extras_->isChecked() );
		QString       error;
		saved_ = saveReport( parts, &error );
		if ( saved_.isEmpty() )
		{
			status_->setText( QStringLiteral( "<span style=\"color: #e0605a\">Couldn't save the report: %1</span>" ).arg( error.toHtmlEscaped() ) );
			return;
		}
		const std::string summary = report::summary( gathered_->input( what ) );
		QGuiApplication::clipboard()->setText( ( what.isEmpty() ? QString() : what + QStringLiteral( "\n\n" ) ) + qs( summary ) );
		const QString folder = QFileInfo( saved_ ).absolutePath();
		QDesktopServices::openUrl( QUrl::fromLocalFile( folder ) );
		QDesktopServices::openUrl( QUrl::fromEncoded( QByteArray::fromStdString( report::issueUrl( ss( what ), summary ) ) ) );
		status_->setText( QStringLiteral( "Saved as <a href=\"%1\">%2</a>. Drag it into the issue form that opened in your browser, then submit it. The summary is on the clipboard too." )
		                          .arg( QUrl::fromLocalFile( folder ).toString(), QDir::toNativeSeparators( saved_ ).toHtmlEscaped() ) );
		cancel_->hide();
		send_->setText( QStringLiteral( "Done" ) );
		form_->hide();
		refit();
	}

} // namespace lexiglance::gui
