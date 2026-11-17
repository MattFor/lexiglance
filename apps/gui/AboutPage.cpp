#include "AboutPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/language/Language.h>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace lexiglance::gui
{

	namespace
	{

		QString authorUrl()
		{
			return QStringLiteral( "https://github.com/MattFor" );
		}

		QString donateUrl()
		{
			return QStringLiteral( "https://tip.mattfor.com" );
		}

		struct Component
		{
			const char* name;
			const char* use;
			const char* licence;
			const char* url;
		};

		constexpr std::array<Component, 11> components{ {
				{ .name = "Qt", .use = "the settings window", .licence = "LGPL 3.0", .url = "https://www.qt.io" },
				{ .name = "Breeze icons", .use = "the icons in this window", .licence = "LGPL 3.0", .url = "https://invent.kde.org/frameworks/breeze-icons" },
				{ .name = "Cairo", .use = "drawing the popup", .licence = "LGPL 2.1 or MPL 1.1", .url = "https://cairographics.org" },
				{ .name = "Pango, HarfBuzz, FriBidi, GLib", .use = "laying out text", .licence = "LGPL 2.1, MIT", .url = "https://pango.gnome.org" },
				{ .name = "libcurl", .use = "pronunciations and AnkiConnect", .licence = "curl licence", .url = "https://curl.se" },
				{ .name = "zlib", .use = "reading dictionary archives", .licence = "zlib licence", .url = "https://zlib.net" },
				{ .name = "ONNX Runtime", .use = "running PaddleOCR, downloaded when you choose it", .licence = "MIT", .url = "https://onnxruntime.ai" },
				{ .name = "PaddleOCR PP-OCRv6 and PP-OCRv5", .use = "text recognition models (PP-OCRv5 for Russian, Ukrainian, Korean and Greek), downloaded when you choose them", .licence = "Apache 2.0", .url = "https://github.com/PaddlePaddle/PaddleOCR" },
				{ .name = "Tesseract", .use = "text recognition, when installed", .licence = "Apache 2.0", .url = "https://github.com/tesseract-ocr/tesseract" },
				{ .name = "OpenCC", .use = "the table from simplified Chinese to Japanese kanji", .licence = "Apache 2.0", .url = "https://github.com/BYVoid/OpenCC" },
				{ .name = "Wikimedia Commons", .use = "pronunciations, fetched when played", .licence = "each recording's own (mostly CC BY-SA)", .url = "https://commons.wikimedia.org" },
		} };

		QString languageNames()
		{
			QStringList names;
			for ( const lang::Language* language : lang::languages() )
			{
				names << qs( language->name() );
			}
			return names.join( QStringLiteral( ", " ) );
		}

		QString link( const QString& url, const QString& text )
		{
			return QStringLiteral( "<a href=\"%1\">%2</a>" ).arg( url.toHtmlEscaped(), text.toHtmlEscaped() );
		}

		QString folder( const std::filesystem::path& path )
		{
			const QString local = qs( path.string() );
			return link( QUrl::fromLocalFile( local ).toString(), local );
		}

		QString platformName()
		{
			return QStringLiteral( "%1 (%2)" ).arg( QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture() );
		}

		QString systemDescription()
		{
			return QStringLiteral( "%1, Qt %2" ).arg( platformName(), QString::fromLatin1( qVersion() ) );
		}

		QString buildName()
		{
			if ( channel == "stable" )
			{
				return QStringLiteral( "stable build" );
			}
			return commit.empty() ? QStringLiteral( "dev build" ) : QStringLiteral( "dev build %1" ).arg( qs( commit ) );
		}

		QPushButton* opener( const QString& text, const QString& url, QWidget* parent )
		{
			auto* button = new QPushButton( text, parent );
			QObject::connect( button, &QPushButton::clicked, button, [url] { QDesktopServices::openUrl( QUrl( url ) ); } );
			return button;
		}

	} // namespace

	AboutPage::AboutPage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		text_( new QTextBrowser() ),
		developer_( new QCheckBox( QStringLiteral( "Developer mode" ) ) )
	{
		auto* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 28, 24, 28, 20 );
		layout->setSpacing( 18 );

		auto* header = new QHBoxLayout();
		header->setSpacing( 20 );
		auto* icon = new QLabel();
		icon->setPixmap( QIcon( QStringLiteral( ":/lexiglance/lexiglance.png" ) ).pixmap( QSize( 80, 80 ), devicePixelRatioF() ) );
		icon->setAlignment( Qt::AlignTop );
		header->addWidget( icon );

		auto* identity = new QVBoxLayout();
		identity->setSpacing( 6 );
		auto* name      = new QLabel( QStringLiteral( "Lexiglance" ) );
		QFont name_font = name->font();
		name_font.setPointSizeF( name_font.pointSizeF() * 2.0 );
		name_font.setBold( true );
		name->setFont( name_font );
		identity->addWidget( name );
		auto* tagline = new QLabel( QStringLiteral( "A system-wide pop-up dictionary. Point at a word in any program and read what it means." ) );
		tagline->setWordWrap( true );
		identity->addWidget( tagline );
		// The version is a link of its own, looking like the rest: seven clicks on it offer developer mode.
		auto* byline = new QLabel( QStringLiteral( "Version <a href=\"lexiglance:version\" style=\"color: %5; text-decoration: none\">%1</a> · %2 · %3 · by %4 · free software under the MIT licence" )
		                                   .arg( qs( version ), buildName(), platformName().toHtmlEscaped(), link( authorUrl(), QStringLiteral( "MattFor" ) ), palette().color( QPalette::WindowText ).name() ) );
		byline->setTextFormat( Qt::RichText );
		connect( byline, &QLabel::linkActivated, this, [this]( const QString& url ) {
			if ( url != QStringLiteral( "lexiglance:version" ) )
			{
				QDesktopServices::openUrl( QUrl( url ) );
				return;
			}
			if ( ++version_clicks_ >= 7 && !developer_->isVisible() )
			{
				developer_->show();
				showSummary( QStringLiteral( "You can now turn on Developer mode below the links." ) );
			}
		} );
		byline->setWordWrap( true );
		identity->addWidget( byline );

		const QString project = qs( project_url );
		auto*         links   = new QHBoxLayout();
		links->setSpacing( 8 );
		links->addWidget( opener( QStringLiteral( "Website" ), project, this ) );
		auto* report = new QPushButton( QStringLiteral( "Report a problem..." ) );
		report->setToolTip( QStringLiteral( "Saves a report on your computer (no secrets, nothing is sent) and opens a GitHub issue filled in for you to attach it to" ) );
		connect( report, &QPushButton::clicked, this, [this] { showPage( QStringLiteral( "report" ) ); } );
		links->addWidget( report );
		links->addWidget( opener( QStringLiteral( "Releases" ), project + QStringLiteral( "/releases" ), this ) );
		links->addStretch( 1 );
		identity->addSpacing( 4 );
		identity->addLayout( links );
		developer_->setToolTip( QStringLiteral( "Adds the Developer page: pipeline trace, capture inspector, simulated lookups and problems, live log and an IPC console" ) );
		developer_->hide();
		identity->addWidget( developer_ );
		connect( developer_, &QCheckBox::toggled, this, [this]( bool on ) {
			// Every page hears of it, the sidebar too.
			auto config      = settings().config();
			config.developer = on;
			settings().replace( std::move( config ) );
			if ( on )
			{
				showPage( QStringLiteral( "developer" ) );
			}
		} );
		header->addLayout( identity, 1 );
		auto* donate = opener( QStringLiteral( "♥  Support Lexiglance" ), donateUrl(), this );
		donate->setObjectName( QStringLiteral( "donateButton" ) );
		donate->setCursor( Qt::PointingHandCursor );
		donate->setToolTip( QStringLiteral( "Lexiglance is free; a tip helps it keep going (%1)" ).arg( donateUrl() ) );
		header->addWidget( donate, 0, Qt::AlignVCenter | Qt::AlignRight );
		layout->addLayout( header );

		text_->setOpenLinks( false );
		text_->setFrameShape( QFrame::NoFrame );
		layout->addWidget( text_, 1 );

		connect( text_, &QTextBrowser::anchorClicked, this, [this]( const QUrl& url ) {
			// A file gone since the page was drawn (a log deleted, the diagnosis zip cleaned away) says so instead of an
			// error from the file manager.
			if ( url.isLocalFile() && !QFileInfo::exists( url.toLocalFile() ) )
			{
				showSummary( QStringLiteral( "%1 no longer exists." ).arg( QDir::toNativeSeparators( url.toLocalFile() ) ) );
				render();
				return;
			}
			QDesktopServices::openUrl( url );
		} );

		daemon_ = QStringLiteral( "not running" );
		render();

		client().onEvent( [this]( std::string_view event, const json::Value& ) {
			if ( event == "status.changed" || event == "dictionaries.changed" )
			{
				activated();
			}
		} );
		client().onConnection( [this]( bool ) { activated(); } );
	}

	void AboutPage::activated()
	{
		client().call( "status", "{}", [this]( const json::Value* status, const QString& error ) {
			if ( status == nullptr )
			{
				daemon_ = error.isEmpty() ? QStringLiteral( "not running" ) : error;
			}
			else
			{
				const QString problem = qs( ( *status )["capture_problem"].asString() );
				daemon_               = QStringLiteral( "%1, %2 backend, text capture: %3%4" )
				                                .arg( qs( ( *status )["version"].asString() ), qs( ( *status )["backend"].asString() ), qs( ( *status )["capture"].asString() ), problem.isEmpty() ? QString() : " (" + problem + ")" );
			}
			render();
		} );
		client().call( "dictionaries.list", "{}", [this]( const json::Value* result, const QString& ) {
			dictionaries_.clear();
			if ( result == nullptr )
			{
				render();
				return;
			}
			for ( const json::Value& d : ( *result )["dictionaries"].items() )
			{
				if ( const auto attribution = d["attribution"].asString(); !attribution.empty() )
				{
					dictionaries_ += QStringLiteral( "<p><b>%1</b><br>%2</p>" )
					                         .arg( qs( d["title"].asString() ).toHtmlEscaped(), qs( attribution ).toHtmlEscaped().replace( '\n', QStringLiteral( "<br>" ) ) );
				}
			}
			render();
		} );
	}

	void AboutPage::refresh()
	{
		const QSignalBlocker blocker( developer_ );
		developer_->setChecked( settings().config().developer );
		developer_->setVisible( settings().config().developer || version_clicks_ >= 7 );
	}

	void AboutPage::render()
	{
		const QString muted = palette().color( QPalette::PlaceholderText ).name();
		const auto    row   = []( const QString& label, const QString& value ) {
			return QStringLiteral( "<tr><td style=\"padding-right: 18px\">%1</td><td>%2</td></tr>" ).arg( label, value );
		};
		// A file that may not be there yet: a link once it is, before that its path greyed and why.
		const auto file = [&]( const QString& path, const QString& absent ) {
			if ( QFileInfo::exists( path ) )
			{
				return link( QUrl::fromLocalFile( path ).toString(), QDir::toNativeSeparators( path ) );
			}
			return QStringLiteral( "<span style=\"color: %1\">%2 (%3)</span>" ).arg( muted, QDir::toNativeSeparators( path ).toHtmlEscaped(), absent.toHtmlEscaped() );
		};

		QString html = QStringLiteral( "<h3>System</h3><table cellspacing=\"3\">" );
		html += row( QStringLiteral( "Lexiglance" ), QStringLiteral( "%1 (%2)" ).arg( qs( version ), buildName() ) );
		html += row( QStringLiteral( "Daemon" ), withLinks( daemon_.toHtmlEscaped() ) );
		html += row( QStringLiteral( "Languages" ), languageNames().toHtmlEscaped() );
		html += row( QStringLiteral( "System" ), systemDescription().toHtmlEscaped() );
		html += QStringLiteral( "</table>" );

		html += QStringLiteral( "<h3>Files</h3><table cellspacing=\"3\">" );
		html += row( QStringLiteral( "Settings" ), folder( paths::configDir() ) );
		html += row( QStringLiteral( "Dictionaries" ), folder( paths::dictionariesDir() ) );
		html += row( QStringLiteral( "OCR models" ), folder( paths::ocrDir() ) );
		std::error_code ec;
		std::filesystem::create_directories( lang::languagesDirectory(), ec );
		html += row( QStringLiteral( "Languages" ), folder( lang::languagesDirectory() ) );
		html += row( QStringLiteral( "Daemon log" ), file( qs( ( paths::stateDir() / "daemon.log" ).string() ), QStringLiteral( "not written yet: Lexiglance hasn't run" ) ) );
		html += row( QStringLiteral( "Application log" ), file( qs( ( paths::stateDir() / "application.log" ).string() ), QStringLiteral( "not written yet" ) ) );
		html += QStringLiteral( "</table>" );

		html += QStringLiteral( "<h3>Licence</h3><p>Lexiglance is free software under the %1. Copyright © 2026 %2.</p>" )
		                .arg( link( qs( project_url ) + QStringLiteral( "/blob/master/LICENSE" ), QStringLiteral( "MIT licence" ) ), link( authorUrl(), QStringLiteral( "MattFor" ) ) );

		html += QStringLiteral( "<h3>Attributions</h3>" );
		html += QStringLiteral( "<p style=\"color: %1\">Dictionaries, programs and data Lexiglance uses, with their licences. The dictionary format is Yomitan's.</p>" ).arg( muted );
		html += QStringLiteral( "<h4>Dictionaries</h4>" );
		html += dictionaries_.isEmpty() ? QStringLiteral( "<p style=\"color: %1\">None of your dictionaries asks for an attribution.</p>" ).arg( muted ) : dictionaries_;
		html += QStringLiteral( "<h4>Software</h4><table cellspacing=\"3\">" );
		for ( const Component& component : components )
		{
			html += QStringLiteral( "<tr><td style=\"padding-right: 18px\">%1</td><td style=\"padding-right: 18px; color: %2\">%3</td><td>%4</td></tr>" )
			                .arg( link( QString::fromLatin1( component.url ), QString::fromLatin1( component.name ) ), muted, QString::fromLatin1( component.use ), QString::fromLatin1( component.licence ) );
		}
		html += QStringLiteral( "</table>" );

		const int scroll = text_->verticalScrollBar() != nullptr ? text_->verticalScrollBar()->value() : 0;
		text_->setHtml( html );
		text_->verticalScrollBar()->setValue( scroll );
	}

} // namespace lexiglance::gui
