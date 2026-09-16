#include "ScanningPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <lexiglance/language/Language.h>
#include <lexiglance/config/Keys.h>

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace lexiglance::gui
{

	namespace
	{

		QSpinBox* spin( int min, int max, const QString& suffix )
		{
			auto* box = new QSpinBox();
			box->setRange( min, max );
			box->setSuffix( suffix );
			return box;
		}

		config::SelectionMode selectionAt( int index )
		{
			switch ( index )
			{
				case 1:
					return config::SelectionMode::Always;
				case 2:
					return config::SelectionMode::WithTrigger;
				default:
					return config::SelectionMode::Off;
			}
		}

		int selectionIndex( config::SelectionMode mode )
		{
			switch ( mode )
			{
				case config::SelectionMode::Always:
					return 1;
				case config::SelectionMode::WithTrigger:
					return 2;
				case config::SelectionMode::Off:
					break;
			}
			return 0;
		}

		QLabel* note( const QString& text )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			label->setEnabled( false );
			return label;
		}

	} // namespace

	ScanningPage::ScanningPage( const Context& context, QWidget* parent ) :
		Page( context, parent ),
		record_( new QPushButton( QIcon::fromTheme( QStringLiteral( "media-record" ) ), QStringLiteral( "Record..." ) ) ),
		record_hint_( new QLabel() ),
		length_( spin( 1, 64, QStringLiteral( " characters" ) ) ),
		delay_( spin( 0, 500, QStringLiteral( " ms" ) ) ),
		threshold_( spin( 0, 50, QStringLiteral( " px" ) ) ),
		hide_empty_( new QCheckBox( QStringLiteral( "Hide the popup when nothing is found under the pointer" ) ) ),
		kanji_( new QCheckBox( QStringLiteral( "Show kanji information when no word matches" ) ) ),
		highlight_( new QCheckBox( QStringLiteral( "Highlight the matched text" ) ) ),
		accessibility_( new QCheckBox( QStringLiteral( "Ask applications to expose their text (accessibility bus)" ) ) ),
		known_languages_( new QCheckBox( QStringLiteral( "Only look up text in a supported language (no popups for English interface text)" ) ) ),
		wheel_( new QCheckBox( QStringLiteral( "Mouse wheel changes the looked-up length while the trigger is held" ) ) ),
		wheel_lock_( new QCheckBox( QStringLiteral( "Also keep the wheel from the window underneath (uses a mouse hook some anti-cheats dislike)" ) ) ),
		selection_( new QComboBox() ),
		ignored_( new QPlainTextEdit() ),
		ocr_( new OcrGroup( context ) )
	{
		auto* scroll  = new QScrollArea();
		auto* content = new QWidget();
		auto* layout  = new QVBoxLayout( content );
		layout->setContentsMargins( 24, 20, 24, 20 );
		layout->setSpacing( 14 );

		auto* trigger_box = new QGroupBox( QStringLiteral( "Trigger" ) );
		auto* trigger     = new QVBoxLayout( trigger_box );
		trigger->addWidget( note( QStringLiteral(
				"Hold these keys together and point at text to look it up. Keys are observed passively: other applications and games "
				"still receive every key press."
		) ) );
		auto* row = new QHBoxLayout();
		for ( std::size_t i = 0; i < keys_.size(); ++i )
		{
			auto* combo = new QComboBox();
			combo->setEditable( true );
			combo->addItem( i == 0 ? QString() : QStringLiteral( "(none)" ) );
			for ( const auto name : config::keyNames() )
			{
				combo->addItem( qs( name ) );
			}
			combo->setMinimumWidth( 130 );
			keys_[i] = combo;
			if ( i > 0 )
			{
				row->addWidget( new QLabel( QStringLiteral( "+" ) ) );
			}
			row->addWidget( combo );
			connect( combo, &QComboBox::currentTextChanged, this, [this] { storeTrigger(); } );
		}
		row->addSpacing( 12 );
		row->addWidget( record_ );
		row->addStretch( 1 );
		trigger->addLayout( row );
		record_hint_->setEnabled( false );
		trigger->addWidget( record_hint_ );
		layout->addWidget( trigger_box );

		auto* behaviour_box = new QGroupBox( QStringLiteral( "Behaviour" ) );
		auto* form          = new QFormLayout( behaviour_box );
		form->addRow( QStringLiteral( "Scan length" ), length_ );
		form->addRow( QStringLiteral( "Rescan interval" ), delay_ );
		form->addRow( QStringLiteral( "Movement threshold" ), threshold_ );
#ifdef Q_OS_WIN
		// Windows has no selection of its own; text hookers copy what they read to the clipboard.
		selection_->addItems( { QStringLiteral( "Off" ), QStringLiteral( "Whenever text is copied" ), QStringLiteral( "When text is copied while the trigger is held" ) } );
		form->addRow( QStringLiteral( "Look up copied text" ), selection_ );
#else
		selection_->addItems( { QStringLiteral( "Off" ), QStringLiteral( "Whenever text is selected" ), QStringLiteral( "When text is selected while the trigger is held" ) } );
		form->addRow( QStringLiteral( "Look up selections" ), selection_ );
#endif
		form->addRow( hide_empty_ );
		form->addRow( kanji_ );
		form->addRow( highlight_ );
		form->addRow( accessibility_ );
		// Fewer languages to tell apart make lookups and OCR faster.
		auto* languages_row = new QHBoxLayout();
		for ( const lang::Language* language : lang::languages() )
		{
			auto* box = new QCheckBox( qs( language->name() ) );
			box->setToolTip( QStringLiteral( "Look up words in %1. Turning off languages you do not read makes telling languages apart and OCR faster." ).arg( qs( language->name() ) ) );
			language_boxes_.emplace_back( box, std::string( language->code() ) );
			languages_row->addWidget( box );
		}
		languages_row->addStretch( 1 );
		form->addRow( QStringLiteral( "Languages" ), languages_row );
		form->addRow( known_languages_ );
		form->addRow( wheel_ );
		form->addRow( wheel_lock_ );
#ifdef Q_OS_WIN
		// Windows applications answer UI Automation without being asked.
		form->setRowVisible( accessibility_, false );
		form->addRow( note( QStringLiteral(
				"Office, browsers, Qt, WPF and WinUI applications and consoles expose their text through UI Automation; OCR reads "
				"everything else. Copied text lookup works with text hookers such as Textractor."
		) ) );
#else
		// X11 grabs the two wheel buttons instead, which needs nothing of the user.
		form->setRowVisible( wheel_lock_, false );
		form->addRow( note( QStringLiteral(
				"GTK, Qt, Firefox and LibreOffice expose text automatically. Chromium based browsers and Electron apps need "
				"--force-renderer-accessibility. Selection lookup works everywhere, including terminals."
		) ) );
#endif
		layout->addWidget( behaviour_box );
		layout->addWidget( ocr_ );

		auto* ignore_box = new QGroupBox( QStringLiteral( "Ignored windows" ) );
		auto* ignore     = new QVBoxLayout( ignore_box );
		ignore->addWidget( note( QStringLiteral(
				"One pattern per line, matched against the window class (name.Class, * and ? allowed), for example steam_app_*. "
				"Lexiglance does nothing at all while such a window is under the pointer."
		) ) );
		ignored_->setPlaceholderText( QStringLiteral( "steam_app_*" ) );
		ignored_->setMaximumHeight( 110 );
		ignore->addWidget( ignored_ );
		layout->addWidget( ignore_box );

		auto* reset_row = new QHBoxLayout();
		auto* reset     = new QPushButton( QStringLiteral( "Reset to defaults..." ) );
		reset->setToolTip( QStringLiteral( "The trigger, how text is scanned and the OCR settings. Downloaded OCR models stay." ) );
		reset_row->addStretch( 1 );
		reset_row->addWidget( reset );
		layout->addLayout( reset_row );
		connect( reset, &QPushButton::clicked, this, [this] {
			if ( !confirm( this, QStringLiteral( "Reset scanning" ), QStringLiteral( "The trigger, how text is scanned and the OCR settings go back to their defaults. Downloaded OCR models stay." ), QStringLiteral( "Reset" ) ) )
			{
				return;
			}
			auto config = settings().config();
			config.scan = config::ScanSettings{};
			settings().replace( std::move( config ) );
		} );
		layout->addStretch( 1 );

		scroll->setWidget( content );
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		auto* outer = new QVBoxLayout( this );
		outer->setContentsMargins( 0, 0, 0, 0 );
		outer->addWidget( scroll );

		const auto commit = [this] {
			if ( !loading_ )
			{
				settings().commit();
			}
		};
		connect( length_, &QSpinBox::valueChanged, this, [this, commit]( int v ) {
			settings().config().scan.max_length = v;
			commit();
		} );
		connect( delay_, &QSpinBox::valueChanged, this, [this, commit]( int v ) {
			settings().config().scan.delay_ms = v;
			commit();
		} );
		connect( threshold_, &QSpinBox::valueChanged, this, [this, commit]( int v ) {
			settings().config().scan.move_threshold = v;
			commit();
		} );
		connect( hide_empty_, &QCheckBox::toggled, this, [this, commit]( bool v ) {
			settings().config().scan.hide_on_no_result = v;
			commit();
		} );
		connect( kanji_, &QCheckBox::toggled, this, [this, commit]( bool v ) {
			settings().config().scan.search_kanji = v;
			commit();
		} );
		connect( highlight_, &QCheckBox::toggled, this, [this, commit]( bool v ) {
			settings().config().scan.highlight = v;
			commit();
		} );
		for ( const auto& [box, code] : language_boxes_ )
		{
			connect( box, &QCheckBox::toggled, this, [this, commit, code]( bool on ) {
				auto& disabled = settings().config().disabled_languages;
				std::erase( disabled, code );
				if ( !on )
				{
					disabled.push_back( code );
				}
				updateLanguageBoxes();
				commit();
			} );
		}
		connect( known_languages_, &QCheckBox::toggled, this, [this, commit]( bool v ) {
			settings().config().scan.known_languages_only = v;
			commit();
		} );
		connect( wheel_, &QCheckBox::toggled, this, [this, commit]( bool v ) {
			settings().config().scan.wheel_length = v;
			wheel_lock_->setEnabled( v );
			commit();
		} );
		connect( wheel_lock_, &QCheckBox::toggled, this, [this, commit]( bool v ) {
			settings().config().scan.wheel_lock = v;
			commit();
		} );
		connect( accessibility_, &QCheckBox::toggled, this, [this, commit]( bool v ) {
			settings().config().scan.accessibility = v;
			commit();
		} );
		connect( selection_, &QComboBox::currentIndexChanged, this, [this, commit]( int index ) {
			settings().config().scan.selection = selectionAt( index );
			commit();
		} );
		connect( ignored_, &QPlainTextEdit::textChanged, this, [this, commit] {
			auto& patterns = settings().config().scan.ignored_windows;
			patterns.clear();
			for ( const QString& line : ignored_->toPlainText().split( '\n', Qt::SkipEmptyParts ) )
			{
				if ( !line.trimmed().isEmpty() )
				{
					patterns.push_back( ss( line.trimmed() ) );
				}
			}
			commit();
		} );
		connect( record_, &QPushButton::clicked, this, [this] { record(); } );

		client().onEvent( [this]( std::string_view name, const json::Value& params ) {
			if ( name != "keys.recorded" || !recording_ )
			{
				return;
			}
			recording_ = false;
			record_->setText( QStringLiteral( "Record..." ) );
			std::vector<std::string> keys;
			for ( const json::Value& key : params["keys"].items() )
			{
				keys.emplace_back( key.asString() );
			}
			if ( keys.empty() )
			{
				return;
			}
			keys.resize( std::min( keys.size(), keys_.size() ) );
			settings().config().scan.trigger = keys;
			record_hint_->setText( QStringLiteral( "Recorded %1." ).arg( chordText( keys ) ) );
			refresh();
			settings().commit();
		} );
	}

	void ScanningPage::record()
	{
		if ( recording_ )
		{
			recording_ = false;
			client().call( "keys.cancel" );
			record_->setText( QStringLiteral( "Record..." ) );
			record_hint_->clear();
			return;
		}
		client().call( "keys.record", "{}", [this]( const json::Value* result, const QString& error ) {
			if ( result == nullptr )
			{
				record_hint_->setText( error );
				return;
			}
			recording_ = true;
			record_->setText( QStringLiteral( "Cancel" ) );
			record_hint_->setText( QStringLiteral( "Press and hold your trigger keys (or mouse button), then release them." ) );
		} );
	}

	void ScanningPage::storeTrigger()
	{
		if ( loading_ )
		{
			return;
		}
		std::vector<std::string> keys;
		for ( QComboBox* combo : keys_ )
		{
			const QString text = combo->currentText().trimmed();
			if ( !text.isEmpty() && text != QStringLiteral( "(none)" ) )
			{
				keys.push_back( ss( text ) );
			}
		}
		if ( const auto chord = config::parseChord( keys ); !chord )
		{
			record_hint_->setText( qs( chord.error().message ) );
			return;
		}
		record_hint_->clear();
		settings().config().scan.trigger = keys;
		settings().commit();
	}

	void ScanningPage::refresh()
	{
		loading_         = true;
		const auto& scan = settings().config().scan;
		for ( std::size_t i = 0; i < keys_.size(); ++i )
		{
			const QSignalBlocker blocker( keys_[i] );
			QString              text = i == 0 ? QString() : QStringLiteral( "(none)" );
			if ( i < scan.trigger.size() )
			{
				text = qs( config::displayName( scan.trigger[i] ) );
			}
			keys_[i]->setCurrentText( text );
		}
		length_->setValue( scan.max_length );
		delay_->setValue( scan.delay_ms );
		threshold_->setValue( scan.move_threshold );
		hide_empty_->setChecked( scan.hide_on_no_result );
		kanji_->setChecked( scan.search_kanji );
		highlight_->setChecked( scan.highlight );
		accessibility_->setChecked( scan.accessibility );
		known_languages_->setChecked( scan.known_languages_only );
		for ( const auto& [box, code] : language_boxes_ )
		{
			const QSignalBlocker blocker( box );
			box->setChecked( !std::ranges::contains( settings().config().disabled_languages, code ) );
		}
		updateLanguageBoxes();
		wheel_->setChecked( scan.wheel_length );
		wheel_lock_->setChecked( scan.wheel_lock );
		wheel_lock_->setEnabled( scan.wheel_length );
		selection_->setCurrentIndex( selectionIndex( scan.selection ) );

		QStringList patterns;
		for ( const auto& pattern : scan.ignored_windows )
		{
			patterns << qs( pattern );
		}
		if ( ignored_->toPlainText() != patterns.join( '\n' ) )
		{
			const QSignalBlocker blocker( ignored_ );
			ignored_->setPlainText( patterns.join( '\n' ) );
		}
		ocr_->refresh();
		loading_ = false;
	}

	void ScanningPage::updateLanguageBoxes()
	{
		const auto on = std::ranges::count_if( language_boxes_, []( const auto& entry ) { return entry.first->isChecked(); } );
		for ( const auto& [box, code] : language_boxes_ )
		{
			box->setEnabled( on > 1 || !box->isChecked() );
		}
	}

} // namespace lexiglance::gui
