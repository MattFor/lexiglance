#include "AboutPage.h"

#include "DaemonClient.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/language/Language.h>

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <array>

namespace lexiglance::gui
{

	namespace
	{

		const QString author_url = QStringLiteral( "https://github.com/MattFor" );

		struct Component
		{
			const char* name;
			const char* use;
			const char* licence;
			const char* url;
		};

		// What Lexiglance is built on or downloads, and the licence of each.
		constexpr std::array<Component, 11> components{ {
				{ .name = "Qt", .use = "the settings application", .licence = "LGPL 3.0", .url = "https://www.qt.io" },
				{ .name = "Breeze icons", .use = "the icons of this window", .licence = "LGPL 3.0", .url = "https://invent.kde.org/frameworks/breeze-icons" },
				{ .name = "Cairo", .use = "drawing the popup", .licence = "LGPL 2.1 or MPL 1.1", .url = "https://cairographics.org" },
				{ .name = "Pango, HarfBuzz, FriBidi, GLib", .use = "laying out text", .licence = "LGPL 2.1, MIT", .url = "https://pango.gnome.org" },
				{ .name = "libcurl", .use = "pronunciations and AnkiConnect", .licence = "curl licence", .url = "https://curl.se" },
				{ .name = "zlib", .use = "reading dictionary archives", .licence = "zlib licence", .url = "https://zlib.net" },
				{ .name = "ONNX Runtime", .use = "running PaddleOCR, downloaded when chosen", .licence = "MIT", .url = "https://onnxruntime.ai" },
				{ .name = "PaddleOCR PP-OCRv6 and PP-OCRv5", .use = "text recognition models (PP-OCRv5 for Russian, Ukrainian, Korean and Greek), downloaded when chosen", .licence = "Apache 2.0", .url = "https://github.com/PaddlePaddle/PaddleOCR" },
				{ .name = "Tesseract", .use = "text recognition, when installed", .licence = "Apache 2.0", .url = "https://github.com/tesseract-ocr/tesseract" },
				{ .name = "OpenCC", .use = "the table of simplified to Japanese kanji", .licence = "Apache 2.0", .url = "https://github.com/BYVoid/OpenCC" },
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

		QString systemDescription()
		{
			return QStringLiteral( "%1 (%2), Qt %3" ).arg( QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), QString::fromLatin1( qVersion() ) );
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
		text_( new QTextBrowser() )
	{
		auto* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 28, 24, 28, 20 );
		layout->setSpacing( 18 );

		// Who and what: the icon, the name and version, the author, and where to go from here.
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
		auto* tagline = new QLabel( QStringLiteral( "A system-wide pop-up dictionary: point at a word in any application and read what it means." ) );
		tagline->setWordWrap( true );
		identity->addWidget( tagline );
		auto* byline = new QLabel( QStringLiteral( "Version %1 · by %2 · free software under the MIT licence" ).arg( qs( version ), link( author_url, QStringLiteral( "MattFor" ) ) ) );
		byline->setTextFormat( Qt::RichText );
		byline->setOpenExternalLinks( true );
		identity->addWidget( byline );

		const QString project = qs( project_url );
		auto*         links   = new QHBoxLayout();
		links->setSpacing( 8 );
		links->addWidget( opener( QStringLiteral( "Website" ), project, this ) );
		links->addWidget( opener( QStringLiteral( "Report a problem" ), project + QStringLiteral( "/issues" ), this ) );
		links->addWidget( opener( QStringLiteral( "Releases" ), project + QStringLiteral( "/releases" ), this ) );
		auto* copy = new QPushButton( QStringLiteral( "Copy system information" ) );
		copy->setToolTip( QStringLiteral( "The versions of Lexiglance, its daemon, Qt and the system, for a bug report" ) );
		links->addWidget( copy );
		links->addStretch( 1 );
		identity->addSpacing( 4 );
		identity->addLayout( links );
		header->addLayout( identity, 1 );
		layout->addLayout( header );

		text_->setOpenLinks( false );
		text_->setFrameShape( QFrame::NoFrame );
		layout->addWidget( text_, 1 );

		connect( text_, &QTextBrowser::anchorClicked, this, []( const QUrl& url ) { QDesktopServices::openUrl( url ); } );
		connect( copy, &QPushButton::clicked, this, [this, copy] {
			QGuiApplication::clipboard()->setText( systemInformation() );
			copy->setText( QStringLiteral( "Copied ✓" ) );
			QTimer::singleShot( 1500, copy, [copy] { copy->setText( QStringLiteral( "Copy system information" ) ); } );
		} );

		daemon_ = QStringLiteral( "not running" );
		render();
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
				daemon_ = QStringLiteral( "%1, %2 backend, text capture: %3" )
				                  .arg( qs( ( *status )["version"].asString() ), qs( ( *status )["backend"].asString() ), qs( ( *status )["capture"].asString() ) );
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

	void AboutPage::render()
	{
		const QString muted = palette().color( QPalette::PlaceholderText ).name();
		const auto    row   = []( const QString& label, const QString& value ) {
            return QStringLiteral( "<tr><td style=\"padding-right: 18px\">%1</td><td>%2</td></tr>" ).arg( label, value );
		};

		QString html = QStringLiteral( "<h3>System</h3><table cellspacing=\"3\">" );
		html += row( QStringLiteral( "Lexiglance" ), qs( version ) );
		html += row( QStringLiteral( "Daemon" ), daemon_.toHtmlEscaped() );
		html += row( QStringLiteral( "Languages" ), languageNames().toHtmlEscaped() );
		html += row( QStringLiteral( "System" ), systemDescription().toHtmlEscaped() );
		html += QStringLiteral( "</table>" );

		html += QStringLiteral( "<h3>Files</h3><table cellspacing=\"3\">" );
		html += row( QStringLiteral( "Settings" ), folder( paths::configDir() ) );
		html += row( QStringLiteral( "Dictionaries" ), folder( paths::dictionariesDir() ) );
		html += row( QStringLiteral( "OCR models" ), folder( paths::ocrDir() ) );
		// Language files of one's own go here (docs/languages.md).
		std::error_code ec;
		std::filesystem::create_directories( lang::languagesDirectory(), ec );
		html += row( QStringLiteral( "Languages" ), folder( lang::languagesDirectory() ) );
		html += row( QStringLiteral( "Log" ), folder( paths::stateDir() ) );
		html += QStringLiteral( "</table>" );

		html += QStringLiteral( "<h3>Licence</h3><p>Lexiglance is free software under the %1. Copyright © 2026 %2.</p>" )
		                .arg( link( qs( project_url ) + QStringLiteral( "/blob/master/LICENSE" ), QStringLiteral( "MIT licence" ) ), link( author_url, QStringLiteral( "MattFor" ) ) );

		html += QStringLiteral( "<h3>Attributions</h3>" );
		html += QStringLiteral( "<p style=\"color: %1\">Dictionaries, programs and data Lexiglance uses, with their licences. The dictionary format is Yomitan's.</p>" ).arg( muted );
		html += QStringLiteral( "<h4>Dictionaries</h4>" );
		html += dictionaries_.isEmpty() ? QStringLiteral( "<p style=\"color: %1\">No installed dictionary asks for an attribution.</p>" ).arg( muted ) : dictionaries_;
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

	QString AboutPage::systemInformation() const
	{
		return QStringLiteral( "Lexiglance %1\nDaemon: %2\nSystem: %3\nSettings: %4\n" ).arg( qs( version ), daemon_, systemDescription(), qs( paths::configDir().string() ) );
	}

} // namespace lexiglance::gui
