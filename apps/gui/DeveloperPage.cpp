#include "DeveloperPage.h"

#include "DaemonClient.h"
#include "ReportOverlay.h"
#include "Settings.h"
#include "UpdateGroup.h"

#include <lexiglance/core/Version.h>

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCompleter>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace lexiglance::gui
{

	namespace
	{

		constexpr std::array<std::string_view, 5> stage_names{ "capture", "lookup", "translate", "render", "show" };
		constexpr std::array<std::string_view, 5> log_levels{ "trace", "debug", "info", "warn", "error" };
		constexpr int                             first_stage_column = 9;
		constexpr std::size_t                     kept_rows          = 500;
		constexpr std::size_t                     kept_log           = 5000;

		QLabel* heading( const QString& text )
		{
			auto* label = new QLabel( text );
			QFont font  = label->font();
			font.setPointSizeF( font.pointSizeF() * 1.9 );
			font.setBold( true );
			label->setFont( font );
			return label;
		}

		QLabel* note( const QString& text )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			label->setEnabled( false );
			return label;
		}

		QSpinBox* coordinate()
		{
			auto* box = new QSpinBox();
			box->setRange( -100000, 100000 );
			return box;
		}

		QPlainTextEdit* monospace()
		{
			auto* text = new QPlainTextEdit();
			text->setReadOnly( true );
			text->setLineWrapMode( QPlainTextEdit::NoWrap );
			text->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
			return text;
		}

		QString milliseconds( double ms )
		{
			return QString::number( ms, 'f', ms < 10.0 ? 2 : 1 );
		}

		int levelIndex( std::string_view name )
		{
			for ( std::size_t i = 0; i < log_levels.size(); ++i )
			{
				if ( log_levels[i] == name )
				{
					return static_cast<int>( i );
				}
			}
			return 2;
		}

		QString pretty( const json::Value& value )
		{
			const QByteArray compact = QByteArray::fromStdString( json::serialize( value ) );
			const auto       parsed  = QJsonDocument::fromJson( compact );
			return parsed.isNull() ? QString::fromUtf8( compact ) : QString::fromUtf8( parsed.toJson( QJsonDocument::Indented ) ).trimmed();
		}

		// The next patch version after this one, for a simulated update.
		QString nextVersion()
		{
			QStringList parts = qs( version ).split( QLatin1Char( '.' ) );
			if ( parts.size() == 3 )
			{
				parts[2] = QString::number( parts[2].toInt() + 1 );
			}
			return parts.join( QLatin1Char( '.' ) );
		}

	} // namespace

	DeveloperPage::DeveloperPage( Context context, Actions actions, QWidget* parent ) :
		Page( std::move( context ), parent ),
		actions_( std::move( actions ) ),
		pages_( new QStackedWidget() ),
		note_( new QLabel() ),
		trace_( new QTreeWidget() ),
		percentiles_( new QLabel() ),
		trace_paused_( new QCheckBox( QStringLiteral( "Pause" ) ) ),
		capture_x_( coordinate() ),
		capture_y_( coordinate() ),
		capture_picture_( new QCheckBox( QStringLiteral( "OCR picture" ) ) ),
		capture_button_( new QPushButton( QStringLiteral( "Capture there" ) ) ),
		capture_later_( new QPushButton( QStringLiteral( "At the pointer in 3 s" ) ) ),
		capture_text_( monospace() ),
		capture_image_( new QLabel() ),
		scan_x_( coordinate() ),
		scan_y_( coordinate() ),
		scan_sentence_( new QCheckBox( QStringLiteral( "With the sentence key" ) ) ),
		lookup_text_( new QLineEdit() ),
		changes_version_( new QLineEdit( qs( version ) ) ),
		update_version_( new QLineEdit( nextVersion() ) ),
		log_filter_( new QComboBox() ),
		log_level_( new QComboBox() ),
		log_( monospace() ),
		console_method_( new QLineEdit( QStringLiteral( "status" ) ) ),
		console_params_( new QPlainTextEdit( QStringLiteral( "{}" ) ) ),
		console_events_( new QCheckBox( QStringLiteral( "Show events" ) ) ),
		console_output_( monospace() )
	{
		auto* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 28, 22, 28, 18 );
		layout->setSpacing( 10 );

		auto* header = new QHBoxLayout();
		header->addWidget( heading( QStringLiteral( "Developer" ) ) );
		header->addStretch( 1 );
		auto* bundle = new QPushButton( QStringLiteral( "Export diagnostics" ) );
		bundle->setToolTip( QStringLiteral( "Saves the problem report, with the trace below but without a picture of the popup, to your downloads and opens the folder" ) );
		header->addWidget( bundle );
		auto* off = new QPushButton( QStringLiteral( "Turn developer mode off" ) );
		header->addWidget( off );
		layout->addLayout( header );
		layout->addWidget( note( QStringLiteral( "Why Lexiglance does what it does. Nothing here sends input to other programs: scans and lookups go through Lexiglance's own pipeline, as if the trigger were held." ) ) );

		auto* tabs  = new QHBoxLayout();
		auto* group = new QButtonGroup( this );
		int   index = 0;
		for ( const QString& name : { QStringLiteral( "Trace" ), QStringLiteral( "Capture" ), QStringLiteral( "Simulate" ), QStringLiteral( "Log" ), QStringLiteral( "Console" ) } )
		{
			auto* pill = new QPushButton( name );
			pill->setCheckable( true );
			pill->setProperty( "pill", true );
			pill->setCursor( Qt::PointingHandCursor );
			pill->setChecked( index == 0 );
			group->addButton( pill, index++ );
			tabs->addWidget( pill );
		}
		tabs->addStretch( 1 );
		layout->addLayout( tabs );
		pages_->addWidget( tracePage() );
		pages_->addWidget( capturePage() );
		pages_->addWidget( simulatePage() );
		pages_->addWidget( logPage() );
		pages_->addWidget( consolePage() );
		layout->addWidget( pages_, 1 );
		note_->setWordWrap( true );
		note_->setTextFormat( Qt::RichText );
		note_->setOpenExternalLinks( true );
		layout->addWidget( note_ );

		connect( group, &QButtonGroup::idClicked, pages_, &QStackedWidget::setCurrentIndex );
		connect( bundle, &QPushButton::clicked, this, [this] { exportBundle(); } );
		connect( off, &QPushButton::clicked, this, [this] {
			// Every page hears of it, the sidebar too.
			auto config      = settings().config();
			config.developer = false;
			settings().replace( std::move( config ) );
			showPage( QStringLiteral( "about" ) );
		} );

		client().onEvent( [this]( std::string_view name, const json::Value& params ) { onEvent( name, params ); } );
		client().onConnection( [this]( bool connected ) {
			if ( connected && isVisible() )
			{
				subscribe( true );
			}
		} );
	}

	QWidget* DeveloperPage::tracePage()
	{
		auto* page   = new QWidget();
		auto* layout = new QVBoxLayout( page );
		layout->setContentsMargins( 0, 4, 0, 0 );
		auto* row = new QHBoxLayout();
		percentiles_->setWordWrap( true );
		percentiles_->setText( QStringLiteral( "Every lookup from now on, newest first: hold the trigger over text, or use Simulate." ) );
		row->addWidget( percentiles_, 1 );
		row->addWidget( trace_paused_ );
		auto* clear = new QPushButton( QStringLiteral( "Clear" ) );
		row->addWidget( clear );
		layout->addLayout( row );

		trace_->setHeaderLabels( { QStringLiteral( "#" ), QStringLiteral( "Time" ), QStringLiteral( "Kind" ), QStringLiteral( "Window" ), QStringLiteral( "Source" ), QStringLiteral( "Text" ), QStringLiteral( "Offset" ), QStringLiteral( "Conf." ), QStringLiteral( "Found" ), QStringLiteral( "Capture ms" ), QStringLiteral( "Lookup" ), QStringLiteral( "Translate" ), QStringLiteral( "Render" ), QStringLiteral( "Show" ) } );
		trace_->setRootIsDecorated( false );
		trace_->setUniformRowHeights( true );
		trace_->setAlternatingRowColors( true );
		trace_->header()->setStretchLastSection( false );
		trace_->header()->setSectionResizeMode( QHeaderView::ResizeToContents );
		trace_->header()->setSectionResizeMode( 5, QHeaderView::Stretch );
		layout->addWidget( trace_, 1 );
		connect( clear, &QPushButton::clicked, this, [this] {
			trace_->clear();
			rows_.clear();
			trace_lines_.clear();
		} );
		return page;
	}

	QWidget* DeveloperPage::capturePage()
	{
		auto* page   = new QWidget();
		auto* layout = new QVBoxLayout( page );
		layout->setContentsMargins( 0, 4, 0, 0 );
		auto* row = new QHBoxLayout();
		row->addWidget( new QLabel( QStringLiteral( "x" ) ) );
		row->addWidget( capture_x_ );
		row->addWidget( new QLabel( QStringLiteral( "y" ) ) );
		row->addWidget( capture_y_ );
		row->addWidget( capture_picture_ );
		row->addWidget( capture_button_ );
		row->addWidget( capture_later_ );
		row->addStretch( 1 );
		layout->addLayout( row );
		layout->addWidget( note( QStringLiteral( "What the screen reads at a point, in screen pixels: the text, its offsets, the box of the character there (blue) and of its lines (orange). For OCR, the picture it read, with the lines it found (green: read, grey: not needed) and the pointer (red)." ) ) );
		capture_picture_->setChecked( true );
		capture_text_->setPlaceholderText( QStringLiteral( "Nothing captured yet." ) );
		layout->addWidget( capture_text_, 1 );
		auto* scroll = new QScrollArea();
		capture_image_->setAlignment( Qt::AlignLeft | Qt::AlignTop );
		scroll->setWidget( capture_image_ );
		scroll->setWidgetResizable( true );
		layout->addWidget( scroll, 1 );
		connect( capture_button_, &QPushButton::clicked, this, [this] { capture(); } );
		connect( capture_later_, &QPushButton::clicked, this, [this] { pickPoint( capture_later_, capture_x_, capture_y_, [this] { capture(); } ); } );
		return page;
	}

	QWidget* DeveloperPage::simulatePage()
	{
		auto* scroll = new QScrollArea();
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		auto* page   = new QWidget();
		auto* layout = new QVBoxLayout( page );
		layout->setContentsMargins( 0, 0, 8, 0 );
		scroll->setWidget( page );

		auto* lookups      = new QGroupBox( QStringLiteral( "Lookups" ) );
		auto* lookups_form = new QFormLayout( lookups );
		auto* point        = new QHBoxLayout();
		point->addWidget( new QLabel( QStringLiteral( "x" ) ) );
		point->addWidget( scan_x_ );
		point->addWidget( new QLabel( QStringLiteral( "y" ) ) );
		point->addWidget( scan_y_ );
		auto* pick = new QPushButton( QStringLiteral( "Pick in 3 s" ) );
		pick->setToolTip( QStringLiteral( "Uses the pointer position three seconds from now" ) );
		point->addWidget( pick );
		point->addStretch( 1 );
		lookups_form->addRow( QStringLiteral( "Point" ), point );
		auto* scan_row = new QHBoxLayout();
		auto* scan     = new QPushButton( QStringLiteral( "Scan there" ) );
		scan->setToolTip( QStringLiteral( "Does what holding the trigger there would: read, look up, highlight, show the popup" ) );
		scan_row->addWidget( scan );
		scan_row->addWidget( scan_sentence_ );
		scan_row->addStretch( 1 );
		lookups_form->addRow( QString(), scan_row );
		auto* lookup_row = new QHBoxLayout();
		lookup_text_->setPlaceholderText( QStringLiteral( "Any text, for example 食べた" ) );
		lookup_row->addWidget( lookup_text_, 1 );
		auto* lookup = new QPushButton( QStringLiteral( "Look up there" ) );
		lookup->setToolTip( QStringLiteral( "Sends the text through the pipeline as a selection there: looks it up, translates it as set and shows the real popup" ) );
		lookup_row->addWidget( lookup );
		lookups_form->addRow( QStringLiteral( "Text" ), lookup_row );
		auto* popup_row = new QHBoxLayout();
		auto* more      = new QPushButton( QStringLiteral( "Wheel +1" ) );
		auto* fewer     = new QPushButton( QStringLiteral( "Wheel −1" ) );
		auto* release   = new QPushButton( QStringLiteral( "Let go of the trigger" ) );
		auto* hide      = new QPushButton( QStringLiteral( "Hide the popup" ) );
		for ( auto* button : { more, fewer, release, hide } )
		{
			popup_row->addWidget( button );
		}
		popup_row->addStretch( 1 );
		lookups_form->addRow( QStringLiteral( "Popup" ), popup_row );
		layout->addWidget( lookups );

		auto* problems        = new QGroupBox( QStringLiteral( "Health problems" ) );
		auto* problems_layout = new QVBoxLayout( problems );
		problems_layout->addWidget( note( QStringLiteral( "Fake problems, to see how the settings window shows them: in Overview -> Health and the status, until you untick them or the daemon restarts." ) ) );
		auto*                                                    problem_row = new QHBoxLayout();
		const std::array<std::pair<const char*, const char*>, 5> made_up{ {
				{ "info", "Notice" },
				{ "warning", "Warning (fix: open Scanning)" },
				{ "error", "Error (fix: restart)" },
				{ "ocr", "OCR missing" },
				{ "permissions", "macOS permissions refused" },
		} };
		for ( const auto& [name, label] : made_up )
		{
			auto* box = new QCheckBox( QString::fromLatin1( label ) );
			box->setProperty( "problem", QString::fromLatin1( name ) );
			problems_.push_back( box );
			problem_row->addWidget( box );
			connect( box, &QCheckBox::toggled, this, [this] { simulateProblems(); } );
		}
		problem_row->addStretch( 1 );
		problems_layout->addLayout( problem_row );
		layout->addWidget( problems );

		auto* states        = new QGroupBox( QStringLiteral( "Settings window" ) );
		auto* states_layout = new QVBoxLayout( states );
		states_layout->addWidget( note( QStringLiteral( "States to try the window in. An update shows on Overview -> Updates as if a check had found that version, or as if you had just updated to it; nothing is downloaded." ) ) );
		auto* states_form = new QFormLayout();
		states_layout->addLayout( states_form );
		auto* setup_row = new QHBoxLayout();
		auto* setup     = new QPushButton( QStringLiteral( "Show the setup wizard" ) );
		setup_row->addWidget( setup );
		setup_row->addStretch( 1 );
		states_form->addRow( QStringLiteral( "First start" ), setup_row );
		auto* changes_row = new QHBoxLayout();
		changes_row->addWidget( changes_version_ );
		auto* changes = new QPushButton( QStringLiteral( "Show what's new" ) );
		changes_row->addWidget( changes );
		changes_row->addStretch( 1 );
		states_form->addRow( QStringLiteral( "Version" ), changes_row );
		auto* update_row = new QHBoxLayout();
		update_row->addWidget( update_version_ );
		auto* available = new QPushButton( QStringLiteral( "Update available" ) );
		auto* installed = new QPushButton( QStringLiteral( "Update installed" ) );
		update_row->addWidget( available );
		update_row->addWidget( installed );
		update_row->addStretch( 1 );
		states_form->addRow( QStringLiteral( "Update" ), update_row );
		layout->addWidget( states );
		layout->addStretch( 1 );

		// Both pages' points are the same one.
		connect( scan_x_, &QSpinBox::valueChanged, capture_x_, &QSpinBox::setValue );
		connect( scan_y_, &QSpinBox::valueChanged, capture_y_, &QSpinBox::setValue );
		connect( capture_x_, &QSpinBox::valueChanged, scan_x_, &QSpinBox::setValue );
		connect( capture_y_, &QSpinBox::valueChanged, scan_y_, &QSpinBox::setValue );
		connect( pick, &QPushButton::clicked, this, [this, pick] { pickPoint( pick, scan_x_, scan_y_, {} ); } );
		connect( scan, &QPushButton::clicked, this, [this] {
			json::Writer params;
			params.beginObject().field( "x", scan_x_->value() ).field( "y", scan_y_->value() ).field( "sentence", scan_sentence_->isChecked() ).endObject();
			ask( "debug.scan", params.take(), QStringLiteral( "Scanned at %1,%2. The Trace shows the result." ).arg( scan_x_->value() ).arg( scan_y_->value() ) );
		} );
		const auto look_up = [this] {
			json::Writer params;
			params.beginObject().field( "text", ss( lookup_text_->text() ) ).field( "x", scan_x_->value() ).field( "y", scan_y_->value() ).endObject();
			ask( "debug.lookup", params.take(), QStringLiteral( "Looked up «%1» at %2,%3." ).arg( lookup_text_->text() ).arg( scan_x_->value() ).arg( scan_y_->value() ) );
		};
		connect( lookup, &QPushButton::clicked, this, look_up );
		connect( lookup_text_, &QLineEdit::returnPressed, this, look_up );
		connect( more, &QPushButton::clicked, this, [this] { ask( "debug.wheel", "{\"delta\":1}", QStringLiteral( "One more character." ) ); } );
		connect( fewer, &QPushButton::clicked, this, [this] { ask( "debug.wheel", "{\"delta\":-1}", QStringLiteral( "One character fewer." ) ); } );
		connect( release, &QPushButton::clicked, this, [this] { ask( "debug.release", "{}", QStringLiteral( "The trigger was let go." ) ); } );
		connect( hide, &QPushButton::clicked, this, [this] { ask( "popup.hide", "{}", QStringLiteral( "The popup was hidden." ) ); } );
		connect( setup, &QPushButton::clicked, this, [this] {
			if ( actions_.show_setup )
			{
				actions_.show_setup();
			}
		} );
		connect( changes, &QPushButton::clicked, this, [this] {
			if ( actions_.show_changes )
			{
				actions_.show_changes( changes_version_->text().trimmed() );
			}
		} );
		const auto simulate_update = [this]( bool done ) {
			for ( QWidget* window : QApplication::topLevelWidgets() )
			{
				// UpdateGroup has no Q_OBJECT for findChildren to know it by.
				for ( auto* group : window->findChildren<QGroupBox*>() )
				{
					if ( auto* updates = dynamic_cast<UpdateGroup*>( group ) )
					{
						updates->simulate( done ? qs( version ) : update_version_->text().trimmed(), done );
					}
				}
			}
			showPage( QStringLiteral( "overview" ) );
			// After an update the window shows what changed, once.
			if ( done && actions_.show_changes )
			{
				actions_.show_changes( qs( version ) );
			}
		};
		connect( available, &QPushButton::clicked, this, [simulate_update] { simulate_update( false ); } );
		connect( installed, &QPushButton::clicked, this, [simulate_update] { simulate_update( true ); } );
		return scroll;
	}

	QWidget* DeveloperPage::logPage()
	{
		auto* page   = new QWidget();
		auto* layout = new QVBoxLayout( page );
		layout->setContentsMargins( 0, 4, 0, 0 );
		auto* row = new QHBoxLayout();
		row->addWidget( new QLabel( QStringLiteral( "Show" ) ) );
		log_filter_->addItems( { QStringLiteral( "Everything" ), QStringLiteral( "Debug and up" ), QStringLiteral( "Info and up" ), QStringLiteral( "Warnings and errors" ), QStringLiteral( "Errors" ) } );
		row->addWidget( log_filter_ );
		row->addSpacing( 16 );
		row->addWidget( new QLabel( QStringLiteral( "The daemon logs" ) ) );
		for ( const auto level : log_levels )
		{
			log_level_->addItem( qs( level ) );
		}
		log_level_->setToolTip( QStringLiteral( "What the daemon writes to its log from now on (saved in the settings)" ) );
		row->addWidget( log_level_ );
		row->addStretch( 1 );
		auto* copy  = new QPushButton( QStringLiteral( "Copy" ) );
		auto* save  = new QPushButton( QStringLiteral( "Save..." ) );
		auto* clear = new QPushButton( QStringLiteral( "Clear" ) );
		row->addWidget( copy );
		row->addWidget( save );
		row->addWidget( clear );
		layout->addLayout( row );
		log_->setMaximumBlockCount( static_cast<int>( kept_log ) );
		layout->addWidget( log_, 1 );

		connect( log_filter_, &QComboBox::currentIndexChanged, this, [this] { showLog(); } );
		connect( log_level_, &QComboBox::currentIndexChanged, this, [this]( int index ) {
			if ( index >= 0 && settings().loaded() && settings().config().log_level != log_levels[static_cast<std::size_t>( index )] )
			{
				settings().config().log_level = std::string( log_levels[static_cast<std::size_t>( index )] );
				settings().commit();
			}
		} );
		connect( copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText( log_->toPlainText() ); } );
		connect( save, &QPushButton::clicked, this, [this] {
			const QString file = QFileDialog::getSaveFileName( this, QStringLiteral( "Save the log" ), QDir::homePath() + QStringLiteral( "/lexiglance-daemon.log" ) );
			QFile         out( file );
			if ( !file.isEmpty() && out.open( QIODevice::WriteOnly | QIODevice::Text ) )
			{
				out.write( log_->toPlainText().toUtf8() + '\n' );
			}
		} );
		connect( clear, &QPushButton::clicked, this, [this] {
			log_lines_.clear();
			log_->clear();
		} );
		return page;
	}

	QWidget* DeveloperPage::consolePage()
	{
		auto* page   = new QWidget();
		auto* layout = new QVBoxLayout( page );
		layout->setContentsMargins( 0, 4, 0, 0 );
		auto* row = new QHBoxLayout();
		row->addWidget( new QLabel( QStringLiteral( "Method" ) ) );
		auto* methods = new QCompleter(
				{ QStringLiteral( "status" ),
				  QStringLiteral( "config.get" ),
				  QStringLiteral( "health" ),
				  QStringLiteral( "dictionaries.list" ),
				  QStringLiteral( "lookup" ),
				  QStringLiteral( "translate" ),
				  QStringLiteral( "popup.show" ),
				  QStringLiteral( "popup.hide" ),
				  QStringLiteral( "popup.preview" ),
				  QStringLiteral( "stats" ),
				  QStringLiteral( "debug.capture" ),
				  QStringLiteral( "debug.pointer" ),
				  QStringLiteral( "debug.scan" ),
				  QStringLiteral( "debug.lookup" ),
				  QStringLiteral( "debug.wheel" ),
				  QStringLiteral( "debug.release" ),
				  QStringLiteral( "debug.simulate" ),
				  QStringLiteral( "debug.log" ) },
				this
		);
		console_method_->setCompleter( methods );
		row->addWidget( console_method_, 1 );
		auto* send = new QPushButton( QStringLiteral( "Send" ) );
		send->setProperty( "primary", true );
		row->addWidget( send );
		row->addWidget( console_events_ );
		auto* clear = new QPushButton( QStringLiteral( "Clear" ) );
		row->addWidget( clear );
		layout->addLayout( row );
		console_params_->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
		console_params_->setFixedHeight( 70 );
		console_params_->setToolTip( QStringLiteral( "The parameters, as JSON (docs/ipc.md lists every method)" ) );
		layout->addWidget( console_params_ );
		layout->addWidget( console_output_, 1 );

		const auto send_request = [this] {
			const std::string method = ss( console_method_->text().trimmed() );
			const std::string params = ss( console_params_->toPlainText().trimmed() );
			if ( !json::Document::parse( params.empty() ? std::string( "{}" ) : params ) )
			{
				console_output_->appendPlainText( QStringLiteral( "! the parameters aren't valid JSON" ) );
				return;
			}
			console_output_->appendPlainText( QStringLiteral( "→ %1 %2" ).arg( qs( method ), qs( params ) ) );
			client().call(
					method,
					params.empty() ? std::string( "{}" ) : params,
					[this]( const json::Value* result, const QString& error ) {
						// Pictures are long: their base64 is cut short here.
						QString text = result != nullptr ? pretty( *result ) : QStringLiteral( "! " ) + error;
						if ( text.size() > 20000 )
						{
							text = text.left( 20000 ) + QStringLiteral( "\n... (%1 characters more)" ).arg( text.size() - 20000 );
						}
						console_output_->appendPlainText( QStringLiteral( "← " ) + text );
					},
					60000
			);
		};
		connect( send, &QPushButton::clicked, this, send_request );
		connect( console_method_, &QLineEdit::returnPressed, this, send_request );
		connect( clear, &QPushButton::clicked, console_output_, &QPlainTextEdit::clear );
		return page;
	}

	void DeveloperPage::refresh()
	{
		const QSignalBlocker blocker( log_level_ );
		log_level_->setCurrentIndex( levelIndex( settings().config().log_level ) );
		if ( settings().config().developer )
		{
			return;
		}
		// Developer mode turned off: what the page gathered (text read, window names) goes, and no made-up problem stays
		// behind in the health report.
		trace_->clear();
		rows_.clear();
		trace_lines_.clear();
		log_lines_.clear();
		log_->clear();
		capture_text_->clear();
		capture_image_->clear();
		if ( std::ranges::any_of( problems_, &QCheckBox::isChecked ) )
		{
			for ( QCheckBox* box : problems_ )
			{
				const QSignalBlocker quiet( box );
				box->setChecked( false );
			}
			simulateProblems();
		}
	}

	void DeveloperPage::showEvent( QShowEvent* event )
	{
		Page::showEvent( event );
		subscribe( true );
		client().call( "status", "{}", [this]( const json::Value* status, const QString& ) {
			if ( status != nullptr )
			{
				onEvent( "status.changed", *status );
			}
		} );
	}

	void DeveloperPage::hideEvent( QHideEvent* event )
	{
		Page::hideEvent( event );
		subscribe( false );
	}

	void DeveloperPage::subscribe( bool on )
	{
		subscribed_ = false;
		if ( !on )
		{
			client().call( "events.subscribe", R"({"events":[]})" );
			return;
		}
		// Refused while the daemon has not yet heard that developer mode is on: asked again when its settings change.
		client().call( "events.subscribe", R"({"events":["trace","log"]})", [this]( const json::Value* result, const QString& error ) {
			subscribed_ = result != nullptr;
			if ( !subscribed_ )
			{
				note_->setText( error.toHtmlEscaped() );
			}
		} );
		// The log up to now, then line by line.
		client().call( "debug.log", "{\"lines\":1000}", [this]( const json::Value* result, const QString& ) {
			if ( result == nullptr )
			{
				return;
			}
			log_lines_.clear();
			for ( const json::Value& line : ( *result )["lines"].items() )
			{
				const QString text = qs( line.asString() );
				// "[2026-11-17 20:14:30.123] [info] ..."
				const auto open  = text.indexOf( QStringLiteral( "] [" ) );
				const auto close = open < 0 ? -1 : text.indexOf( QLatin1Char( ']' ), open + 3 );
				addLog( close < 0 ? QStringLiteral( "info" ) : text.mid( open + 3, close - open - 3 ), text );
			}
			showLog();
		} );
	}

	void DeveloperPage::onEvent( std::string_view name, const json::Value& params )
	{
		if ( name == "trace" )
		{
			addTrace( params );
		}
		else if ( name == "log.line" )
		{
			const QString line = qs( params["line"].asString() );
			addLog( qs( params["level"].asString() ), line );
			const int min = log_filter_->currentIndex();
			if ( levelIndex( params["level"].asString() ) >= min )
			{
				log_->appendPlainText( line );
			}
		}
		else if ( name == "config.changed" && isVisible() && !subscribed_ )
		{
			subscribe( true );
		}
		else if ( name == "status.changed" )
		{
			for ( QCheckBox* box : problems_ )
			{
				const bool           on = std::ranges::any_of( params["simulated"].items(), [&]( const json::Value& item ) { return qs( item.asString() ) == box->property( "problem" ).toString(); } );
				const QSignalBlocker blocker( box );
				box->setChecked( on );
			}
		}
		if ( console_events_->isChecked() && name != "log.line" )
		{
			console_output_->appendPlainText( QStringLiteral( "event %1 %2" ).arg( qs( name ), qs( json::serialize( params ) ).left( 2000 ) ) );
		}
	}

	void DeveloperPage::addTrace( const json::Value& event )
	{
		if ( trace_paused_->isChecked() )
		{
			return;
		}
		trace_lines_.push_back( json::serialize( event ) );
		if ( trace_lines_.size() > 2000 )
		{
			trace_lines_.pop_front();
		}
		const auto id  = event["id"].asInt();
		const auto row = rows_.find( id );
		// A late stage (a translation finishing) of a lookup older than every row kept has no row any more.
		QTreeWidgetItem* item = row != rows_.end() ? row->second : nullptr;
		if ( item == nullptr && ( rows_.size() < kept_rows || id > rows_.begin()->first ) )
		{
			item = new QTreeWidgetItem();
			item->setText( 0, QString::number( id ) );
			item->setText( 1, qs( event["time"].asString() ) );
			trace_->insertTopLevelItem( 0, item );
			rows_.emplace( id, item );
			// The oldest go, never the one just made: rows are kept by the order their lookups were made in.
			while ( rows_.size() > kept_rows )
			{
				delete rows_.begin()->second;
				rows_.erase( rows_.begin() );
			}
		}
		if ( const auto kind = event["kind"].asString(); item != nullptr && !kind.empty() )
		{
			item->setText( 2, qs( kind ) );
			item->setText( 3, qs( event["window"].asString() ) );
			item->setText( 4, qs( event["source"].asString() ) );
			const QString text = qs( event["text"].asString() );
			item->setText( 5, text.isEmpty() && !event["unread"].asString().empty() ? QStringLiteral( "(not looked up) " ) + qs( event["unread"].asString() ) : text );
			item->setToolTip( 5, QStringLiteral( "At %1,%2 · %3 characters, %4 matched · language %5 · %6 characters before the pointer" ).arg( event["x"].asInt() ).arg( event["y"].asInt() ).arg( event["characters"].asInt() ).arg( event["matched"].asInt() ).arg( qs( event["language"].asString( "-" ) ) ).arg( event["rewound"].asInt() ) );
			item->setText( 6, QString::number( event["offset"].asInt() ) );
			item->setText( 7, QString::number( event["confidence"].asDouble(), 'f', 0 ) );
			item->setText( 8, QString::number( event["found"].asInt() ) );
		}
		for ( std::size_t stage = 0; stage < stage_names.size(); ++stage )
		{
			const json::Value& times = event["stages"][stage_names[stage]];
			if ( times.isObject() )
			{
				if ( item != nullptr )
				{
					item->setText( first_stage_column + static_cast<int>( stage ), milliseconds( times["ms"].asDouble() ) );
				}
				latest_[stage] = { times["p50"].asDouble(), times["p95"].asDouble() };
				timed_[stage]  = true;
			}
		}
		QStringList parts;
		for ( std::size_t stage = 0; stage < stage_names.size(); ++stage )
		{
			if ( timed_[stage] )
			{
				parts << QStringLiteral( "<b>%1</b> %2 / %3" ).arg( qs( stage_names[stage] ), milliseconds( latest_[stage].first ), milliseconds( latest_[stage].second ) );
			}
		}
		percentiles_->setText( QStringLiteral( "p50 / p95 in ms over the latest lookups: " ) + parts.join( QStringLiteral( " · " ) ) );
	}

	void DeveloperPage::addLog( const QString& level, const QString& line )
	{
		log_lines_.emplace_back( levelIndex( ss( level ) ), line );
		if ( log_lines_.size() > kept_log )
		{
			log_lines_.pop_front();
		}
	}

	void DeveloperPage::showLog()
	{
		const int   min = log_filter_->currentIndex();
		QStringList shown;
		for ( const auto& [level, line] : log_lines_ )
		{
			if ( level >= min )
			{
				shown << line;
			}
		}
		log_->setPlainText( shown.join( QLatin1Char( '\n' ) ) );
		log_->verticalScrollBar()->setValue( log_->verticalScrollBar()->maximum() );
	}

	void DeveloperPage::pickPoint( QPushButton* button, QSpinBox* x, QSpinBox* y, std::function<void()> then )
	{
		const QString text = button->text();
		button->setEnabled( false );
		for ( int left = 3; left > 0; --left )
		{
			QTimer::singleShot( ( 3 - left ) * 1000, button, [button, left] { button->setText( QStringLiteral( "Point now... %1" ).arg( left ) ); } );
		}
		QTimer::singleShot( 3000, this, [this, button, x, y, text, then = std::move( then )] {
			button->setText( text );
			button->setEnabled( true );
			client().call( "debug.pointer", "{}", [this, x, y, then]( const json::Value* result, const QString& error ) {
				if ( result == nullptr )
				{
					note_->setText( QStringLiteral( "No pointer position: %1" ).arg( error.toHtmlEscaped() ) );
					return;
				}
				x->setValue( static_cast<int>( ( *result )["x"].asInt() ) );
				y->setValue( static_cast<int>( ( *result )["y"].asInt() ) );
				if ( then )
				{
					then();
				}
			} );
		} );
	}

	void DeveloperPage::capture()
	{
		json::Writer params;
		params.beginObject().field( "x", capture_x_->value() ).field( "y", capture_y_->value() ).field( "image", capture_picture_->isChecked() ).endObject();
		capture_button_->setEnabled( false );
		capture_text_->setPlainText( QStringLiteral( "Capturing at %1,%2..." ).arg( capture_x_->value() ).arg( capture_y_->value() ) );
		client().call(
				"debug.capture",
				params.take(),
				[this]( const json::Value* result, const QString& error ) {
					capture_button_->setEnabled( true );
					if ( result == nullptr )
					{
						capture_text_->setPlainText( QStringLiteral( "No capture: %1" ).arg( error ) );
						capture_image_->clear();
						return;
					}
					showCapture( *result );
				},
				20000
		);
	}

	void DeveloperPage::showCapture( const json::Value& result )
	{
		const auto rect = []( const json::Value& r ) { return QStringLiteral( "%1,%2 %3×%4" ).arg( r["x"].asInt() ).arg( r["y"].asInt() ).arg( r["width"].asInt() ).arg( r["height"].asInt() ); };
		QString    text;
		text += QStringLiteral( "text        «%1»\n" ).arg( qs( result["text"].asString() ) );
		text += QStringLiteral( "source      %1 in %2 ms, confidence %3\n" ).arg( qs( result["source"].asString( "-" ) ), QString::number( result["took_ms"].asDouble(), 'f', 2 ), QString::number( result["confidence"].asDouble(), 'f', 1 ) );
		text += QStringLiteral( "window      %1\n" ).arg( qs( result["window"].asString( "-" ) ) );
		text += QStringLiteral( "offsets     %1 in the source, %2 in the sentence, %3 characters before the pointer\n" ).arg( result["offset"].asInt() ).arg( result["sentence_offset"].asInt() ).arg( result["rewound"].asInt() );
		text += QStringLiteral( "sentence    «%1»\n" ).arg( qs( result["sentence"].asString() ) );
		if ( result["character"].isObject() )
		{
			text += QStringLiteral( "character   %1\n" ).arg( rect( result["character"] ) );
		}
		for ( const json::Value& line : result["lines"].items() )
		{
			text += QStringLiteral( "line        %1\n" ).arg( rect( line ) );
		}
		for ( const json::Value& line : result["ocr_lines"].items() )
		{
			text += QStringLiteral( "ocr %1 %2 «%3»\n" ).arg( line["loose"].asBool() ? QStringLiteral( "loose" ) : QStringLiteral( "line " ), rect( line ), qs( line["text"].asString() ) );
		}
		capture_text_->setPlainText( text );
		const auto png = result["image"]["png"].asString();
		QPixmap    picture;
		if ( !png.empty() && picture.loadFromData( QByteArray::fromBase64( QByteArray( png.data(), static_cast<qsizetype>( png.size() ) ) ), "PNG" ) )
		{
			capture_image_->setPixmap( picture );
		}
		else
		{
			capture_image_->setText( result["ocr_lines"].isArray() ? QStringLiteral( "No picture requested." ) : QStringLiteral( "No picture: the text didn't come from OCR." ) );
		}
	}

	void DeveloperPage::ask( std::string_view method, const std::string& params, const QString& done )
	{
		client().call( method, params, [this, done]( const json::Value* result, const QString& error ) { note_->setText( result != nullptr ? done.toHtmlEscaped() : QStringLiteral( "<span style=\"color: #e0605a\">%1</span>" ).arg( error.toHtmlEscaped() ) ); } );
	}

	void DeveloperPage::simulateProblems()
	{
		json::Writer params;
		params.beginObject().key( "problems" ).beginArray();
		for ( const QCheckBox* box : problems_ )
		{
			if ( box->isChecked() )
			{
				params.value( ss( box->property( "problem" ).toString() ) );
			}
		}
		params.endArray().endObject();
		ask( "debug.simulate", params.take(), QStringLiteral( "Overview -> Health now shows the ticked problems as if they were real." ) );
	}

	std::vector<report::Part> DeveloperPage::reportParts() const
	{
		if ( trace_lines_.empty() )
		{
			return {};
		}
		std::string lines;
		for ( const auto& line : trace_lines_ )
		{
			lines.append( line ).push_back( '\n' );
		}
		return { { .name = "traces.jsonl", .title = std::format( "The pipeline trace of the Developer page ({} events)", trace_lines_.size() ), .contents = report::withoutHome( std::move( lines ) ) } };
	}

	void DeveloperPage::exportBundle()
	{
		note_->setText( QStringLiteral( "Gathering the status, the health report and the logs..." ) );
		gatherReport( client(), reportParts(), [this]( const std::shared_ptr<const Gathered>& gathered ) {
			QString error;
			// The trace is what this is for; the popup's picture only goes where someone ticks it (Report a problem).
			const QString file = saveReport( gathered->parts( QString(), false, true ), &error );
			if ( file.isEmpty() )
			{
				note_->setText( QStringLiteral( "<span style=\"color: #e0605a\">Couldn't save: %1</span>" ).arg( error.toHtmlEscaped() ) );
				return;
			}
			const QString folder = QFileInfo( file ).absolutePath();
			note_->setText( QStringLiteral( "Saved as <a href=\"%1\">%2</a>." ).arg( QUrl::fromLocalFile( folder ).toString(), QDir::toNativeSeparators( file ).toHtmlEscaped() ) );
			QDesktopServices::openUrl( QUrl::fromLocalFile( folder ) );
		} );
	}

} // namespace lexiglance::gui
