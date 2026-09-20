#include "TranslationPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Json.h>

#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <iterator>

namespace lexiglance::gui
{

	namespace
	{

		// The table's columns: the language is ticked to be translated, and one of the two weights columns to say which
		// ones it translates with.
		constexpr int language_column = 0;
		constexpr int model_column    = 1;
		constexpr int compact_column  = 2;
		constexpr int full_column     = 3;
		constexpr int status_column   = 4;

		// The language's index in `languages_`, kept on its row.
		constexpr int row_role = Qt::UserRole;

		constexpr translate::Precision precisionOfColumn( int column )
		{
			return column == full_column ? translate::Precision::Full : translate::Precision::Compact;
		}

		QString precisionLabel( translate::Precision precision )
		{
			return precision == translate::Precision::Full ? QStringLiteral( "Full precision" ) : QStringLiteral( "Compact" );
		}

		QLabel* note( const QString& text )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			label->setEnabled( false );
			return label;
		}

		// How a key is kept in the configuration: its canonical name when it is one key ("Shift_L"), else as offered.
		std::string stored( std::string_view name )
		{
			const auto group = config::parseKey( name );
			return group && group->size() == 1 ? std::string( config::keyName( group->front() ) ) : std::string( name );
		}

		// The other languages translated with the same model.
		QStringList sharing( const lang::Language& language, const std::vector<const lang::Language*>& all )
		{
			QStringList names;
			for ( const lang::Language* other : all )
			{
				if ( other != &language && other->translationModel().directory() == language.translationModel().directory() )
				{
					names << qs( other->name() );
				}
			}
			return names;
		}

	} // namespace

	TranslationPage::TranslationPage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		installer_( new translation_install::Installer( &client(), this ) ),
		enabled_( new QCheckBox( QStringLiteral( "Translate sentences into English" ) ) ),
		selections_( new QCheckBox( QStringLiteral( "Also selected text of more than a word (when selections are looked up)" ) ) ),
		key_( new QComboBox() ),
		precision_( new QComboBox() ),
		table_( new QTreeWidget() ),
		details_( new QTextBrowser() ),
		download_( new QPushButton( QIcon::fromTheme( QStringLiteral( "folder-download" ) ), QString() ) ),
		again_( new QPushButton( QIcon::fromTheme( QStringLiteral( "view-refresh" ) ), QStringLiteral( "Download again" ) ) ),
		remove_( new QPushButton( QIcon::fromTheme( QStringLiteral( "edit-delete" ) ), QStringLiteral( "Remove" ) ) ),
		activity_( new QLabel() ),
		progress_( new QProgressBar() ),
		test_text_( new QLineEdit() ),
		test_language_( new QComboBox() ),
		test_button_( new QPushButton( QStringLiteral( "Translate" ) ) ),
		test_result_( new QLabel() ),
		poll_( new QTimer( this ) )
	{
		auto* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 24, 20, 24, 20 );
		layout->setSpacing( 12 );

		auto* settings_box    = new QGroupBox( QStringLiteral( "Translation" ) );
		auto* settings_layout = new QVBoxLayout( settings_box );
		settings_layout->addWidget( enabled_ );
		settings_layout->addWidget( selections_ );
		auto* form = new QFormLayout();
		form->addRow( QStringLiteral( "Sentence key (with the trigger)" ), key_ );
		settings_layout->addLayout( form );
		layout->addWidget( settings_box );

		auto* models_box    = new QGroupBox( QStringLiteral( "Models" ) );
		auto* models_layout = new QVBoxLayout( models_box );
		models_layout->addWidget( note( QStringLiteral( "Tick the languages to translate, and the weights each one translates with. Both kinds can be downloaded at once, and a language "
		                                                "translates with the other until the ticked ones are here. Select rows to download them again, or to remove what they do not use." ) ) );
		precision_->addItem( QStringLiteral( "Compact: about 115 MB each" ), QString::fromUtf8( translate::precisionName( translate::Precision::Compact ) ) );
		precision_->addItem( QStringLiteral( "Full precision: about 440 MB each, faster and a little more accurate" ), QString::fromUtf8( translate::precisionName( translate::Precision::Full ) ) );
		precision_->setToolTip( QStringLiteral( "Sets every language below at once; each can then be ticked on its own. Compact models have their weights rounded to eight bits: a "
		                                        "quarter of the download and about half the memory. Full precision ones translate about twice as fast on most processors, a little "
		                                        "more accurately, but take about twice the memory while loaded (up to a gigabyte)." ) );
		auto* precision_form = new QFormLayout();
		precision_form->addRow( QStringLiteral( "All languages" ), precision_ );
		models_layout->addLayout( precision_form );
		auto* toolbar = new QHBoxLayout();
		download_->setProperty( "primary", true );
		toolbar->addWidget( download_ );
		toolbar->addStretch( 1 );
		toolbar->addWidget( again_ );
		toolbar->addWidget( remove_ );
		models_layout->addLayout( toolbar );

		table_->setColumnCount( 5 );
		table_->setHeaderLabels( { QStringLiteral( "Language" ), QStringLiteral( "Model" ), QStringLiteral( "Compact" ), QStringLiteral( "Full precision" ), QStringLiteral( "State" ) } );
		table_->setRootIsDecorated( false );
		table_->setAlternatingRowColors( true );
		table_->setUniformRowHeights( true );
		table_->setSelectionMode( QAbstractItemView::ExtendedSelection );
		table_->header()->setSectionResizeMode( language_column, QHeaderView::Stretch );
		for ( const int column : { model_column, compact_column, full_column, status_column } )
		{
			table_->header()->setSectionResizeMode( column, QHeaderView::ResizeToContents );
		}
		for ( const lang::Language* language : lang::languages() )
		{
			if ( language->translationModel().empty() )
			{
				continue;
			}
			const auto row = static_cast<int>( languages_.size() );
			languages_.push_back( language );
			auto* item = new QTreeWidgetItem( table_ );
			item->setText( language_column, QStringLiteral( "%1 → English" ).arg( qs( language->name() ) ) );
			item->setFlags( item->flags() | Qt::ItemIsUserCheckable );
			item->setText( model_column, qs( language->translationModel().directory() ) );
			item->setCheckState( language_column, Qt::Checked );
			// The two kinds of weights on the same line: the ticked one is what this language translates with.
			item->setCheckState( compact_column, Qt::Checked );
			item->setCheckState( full_column, Qt::Unchecked );
			item->setData( language_column, row_role, row );
		}
		models_layout->addWidget( table_, 1 );
		details_->setOpenExternalLinks( true );
		details_->setMaximumHeight( 120 );
		models_layout->addWidget( details_ );
		auto* status = new QHBoxLayout();
		activity_->setWordWrap( true );
		status->addWidget( activity_, 1 );
		status->addWidget( progress_, 1 );
		models_layout->addLayout( status );
		progress_->hide();
		activity_->hide();
		layout->addWidget( models_box, 1 );

		auto* test_box    = new QGroupBox( QStringLiteral( "Try it" ) );
		auto* test_layout = new QVBoxLayout( test_box );
		auto* test_row    = new QHBoxLayout();
		test_text_->setPlaceholderText( QStringLiteral( "Type or paste a sentence" ) );
		test_text_->setClearButtonEnabled( true );
		test_language_->addItem( QStringLiteral( "Language: automatic" ), QString() );
		for ( const lang::Language* language : languages_ )
		{
			test_language_->addItem( qs( language->name() ), qs( language->code() ) );
		}
		test_row->addWidget( test_text_, 1 );
		test_row->addWidget( test_language_ );
		test_row->addWidget( test_button_ );
		test_layout->addLayout( test_row );
		test_result_->setWordWrap( true );
		test_result_->setTextInteractionFlags( Qt::TextSelectableByMouse );
		test_layout->addWidget( test_result_ );
		layout->addWidget( test_box );

		connect( enabled_, &QCheckBox::toggled, this, [this] { store(); } );
		connect( selections_, &QCheckBox::toggled, this, [this] { store(); } );
		connect( key_, &QComboBox::currentIndexChanged, this, [this] { store(); } );
		connect( precision_, &QComboBox::currentIndexChanged, this, [this] { applyPrecisionToAll(); } );
		connect( table_, &QTreeWidget::itemChanged, this, [this]( QTreeWidgetItem* item, int column ) { ticked( item, column ); } );
		connect( table_, &QTreeWidget::itemSelectionChanged, this, [this] {
			updateButtons();
			showDetails();
		} );
		connect( table_, &QTreeWidget::currentItemChanged, this, [this] { showDetails(); } );
		connect( download_, &QPushButton::clicked, this, [this] { download( missing(), false ); } );
		connect( again_, &QPushButton::clicked, this, [this] {
			const auto chosen = selected();
			download( chosen, translation_install::bytes( chosen ) == 0 );
		} );
		connect( remove_, &QPushButton::clicked, this, [this] { removeSelected(); } );
		connect( test_button_, &QPushButton::clicked, this, [this] { translate(); } );
		connect( test_text_, &QLineEdit::returnPressed, this, [this] { translate(); } );

		poll_->setInterval( 2000 );
		connect( poll_, &QTimer::timeout, this, [this] { updateRows(); } );
		table_->setCurrentItem( table_->topLevelItem( 0 ) );
	}

	void TranslationPage::refresh()
	{
		loading_           = true;
		const auto& config = settings().config().translation;
		{
			const QSignalBlocker a( enabled_ );
			const QSignalBlocker b( selections_ );
			const QSignalBlocker c( table_ );
			const QSignalBlocker d( precision_ );
			enabled_->setChecked( config.enabled );
			precision_->setCurrentIndex( std::max( 0, precision_->findData( qs( config.model ) ) ) );
			selections_->setChecked( config.selections );
			selections_->setEnabled( config.enabled );
			key_->setEnabled( config.enabled );
			fillKeys();
			for ( std::size_t row = 0; row < languages_.size(); ++row )
			{
				auto* item = table_->topLevelItem( static_cast<int>( row ) );
				item->setCheckState( language_column, config.translates( languages_[row]->code() ) ? Qt::Checked : Qt::Unchecked );
				const auto chosen = precisionOf( *languages_[row] );
				item->setCheckState( compact_column, chosen == translate::Precision::Compact ? Qt::Checked : Qt::Unchecked );
				item->setCheckState( full_column, chosen == translate::Precision::Full ? Qt::Checked : Qt::Unchecked );
			}
		}
		loading_ = false;
		updateRows();
	}

	void TranslationPage::activated()
	{
		updateRows();
		poll_->start();
	}

	void TranslationPage::hideEvent( QHideEvent* event )
	{
		poll_->stop();
		Page::hideEvent( event );
	}

	void TranslationPage::changeEvent( QEvent* event )
	{
		Page::changeEvent( event );
		// The window's colours (links, greyed rows) came after the page was made, or the theme changed.
		if ( event->type() == QEvent::PaletteChange )
		{
			details_html_.clear();
			updateRows();
		}
	}

	void TranslationPage::fillKeys()
	{
		const QSignalBlocker blocker( key_ );
		const auto&          config = settings().config();
		key_->clear();
		key_->addItem( QStringLiteral( "None" ), QString() );
		// Keys that can be held with the trigger: not the mouse, and none of the trigger's own.
		const auto chord   = config::parseChord( config.scan.trigger ).value_or( config::KeyChord{} );
		const auto current = config::parseKey( config.translation.sentence_key );
		for ( const std::string_view name : config::keyNames() )
		{
			const auto group = config::parseKey( name );
			if ( !group || group->empty() || std::ranges::any_of( *group, config::isMouseButton ) || config::extraKey( name, chord ).empty() )
			{
				continue;
			}
			key_->addItem( qs( config::displayName( name ) ), qs( stored( name ) ) );
			if ( current && *current == *group )
			{
				key_->setCurrentIndex( key_->count() - 1 );
			}
		}
	}

	const lang::Language* TranslationPage::languageOf( const QTreeWidgetItem* item ) const
	{
		if ( item == nullptr )
		{
			return nullptr;
		}
		const int row = item->data( language_column, row_role ).toInt();
		return row >= 0 && static_cast<std::size_t>( row ) < languages_.size() ? languages_[static_cast<std::size_t>( row )] : nullptr;
	}

	void TranslationPage::ticked( QTreeWidgetItem* item, int column )
	{
		if ( loading_ || languageOf( item ) == nullptr )
		{
			return;
		}
		// The two kinds of weights are a choice of one: ticking either unticks the other, and neither can be unticked.
		if ( column == compact_column || column == full_column )
		{
			const QSignalBlocker blocker( table_ );
			const int            other = column == compact_column ? full_column : compact_column;
			if ( item->checkState( column ) != Qt::Checked )
			{
				item->setCheckState( column, Qt::Checked );
				return;
			}
			item->setCheckState( other, Qt::Unchecked );
		}
		else if ( column != language_column )
		{
			return;
		}
		store();
	}

	void TranslationPage::applyPrecisionToAll()
	{
		if ( loading_ )
		{
			return;
		}
		const auto wanted = translate::precisionNamed( ss( precision_->currentData().toString() ) );
		{
			const QSignalBlocker blocker( table_ );
			for ( int row = 0; row < table_->topLevelItemCount(); ++row )
			{
				QTreeWidgetItem* item = table_->topLevelItem( row );
				item->setCheckState( compact_column, wanted == translate::Precision::Compact ? Qt::Checked : Qt::Unchecked );
				item->setCheckState( full_column, wanted == translate::Precision::Full ? Qt::Checked : Qt::Unchecked );
			}
		}
		store();
	}

	void TranslationPage::store()
	{
		if ( loading_ )
		{
			return;
		}
		auto& translation        = settings().config().translation;
		translation.enabled      = enabled_->isChecked();
		translation.selections   = selections_->isChecked();
		translation.sentence_key = ss( key_->currentData().toString() );
		translation.model        = ss( precision_->currentData().toString() );
		translation.disabled_languages.clear();
		translation.models.clear();
		for ( std::size_t row = 0; row < languages_.size(); ++row )
		{
			QTreeWidgetItem* item = table_->topLevelItem( static_cast<int>( row ) );
			if ( item->checkState( language_column ) != Qt::Checked )
			{
				translation.disabled_languages.emplace_back( languages_[row]->code() );
			}
			const auto chosen = item->checkState( full_column ) == Qt::Checked ? translate::Precision::Full : translate::Precision::Compact;
			translation.setModelFor( languages_[row]->code(), translate::precisionName( chosen ) );
		}
		selections_->setEnabled( translation.enabled );
		key_->setEnabled( translation.enabled );
		settings().commit();
		updateRows();
	}

	translate::Precision TranslationPage::precisionOf( const lang::Language& language ) const
	{
		return translation_install::precisionFor( language, settings().config().translation );
	}

	void TranslationPage::updateRows()
	{
		const auto&  off   = settings().config().disabled_languages;
		const QColor muted = palette().color( QPalette::Disabled, QPalette::Text );
		const QColor plain = palette().color( QPalette::Text );
		for ( std::size_t row = 0; row < languages_.size(); ++row )
		{
			const lang::Language& language  = *languages_[row];
			auto*                 item      = table_->topLevelItem( static_cast<int>( row ) );
			const bool            unused    = std::ranges::contains( off, language.code() );
			const auto            precision = precisionOf( language );
			const bool            busy      = std::ranges::contains( downloading_, &language );
			QString               state;
			if ( busy )
			{
				state = QStringLiteral( "Downloading…" );
			}
			else if ( const auto here = translation_install::present( language, precision ); !here )
			{
				state = QStringLiteral( "Not downloaded" );
			}
			else
			{
				// The other precision translates until the ticked one is downloaded.
				state = *here == precision ? QStringLiteral( "Ready" ) : *here == translate::Precision::Full ? QStringLiteral( "Ready (full precision)" )
				                                                                                             : QStringLiteral( "Ready (compact)" );
			}
			// A language turned off in Scanning is never read, so its model is not needed either.
			if ( unused )
			{
				state += QStringLiteral( " (the language is off in Scanning)" );
			}
			const QSignalBlocker blocker( table_ );
			if ( item->text( status_column ) != state )
			{
				item->setText( status_column, state );
			}
			// Each kind of weights: its size, plain when it is downloaded and greyed while it is not.
			for ( const int column : { compact_column, full_column } )
			{
				const auto    weights = precisionOfColumn( column );
				const bool    here    = translation_install::installed( language, weights );
				const QString size    = translation_install::megabytes( translation_install::size( language.translationModel(), weights ) );
				if ( item->text( column ) != size )
				{
					item->setText( column, size );
				}
				item->setForeground( column, unused || !here ? muted : plain );
				item->setToolTip( column, QStringLiteral( "%1 weights: %2. Tick them to translate %3 with them." ).arg( precisionLabel( weights ), here ? QStringLiteral( "downloaded" ) : QStringLiteral( "not downloaded" ), qs( language.name() ) ) );
			}
			for ( const int column : { language_column, model_column, status_column } )
			{
				item->setForeground( column, unused ? muted : plain );
			}
		}
		updateButtons();
		showDetails();
	}

	void TranslationPage::updateButtons()
	{
		const bool busy   = installer_->busy();
		const auto needed = translation_install::bytes( missing() );
		const auto chosen = selected();
		const auto absent = translation_install::bytes( chosen );
		download_->setText( needed > 0 ? QStringLiteral( "Download ticked models (%1)" ).arg( translation_install::megabytes( needed ) ) : QStringLiteral( "All ticked models downloaded" ) );
		download_->setEnabled( !busy && needed > 0 );
		// The selected models: those not here yet, or all of them afresh when they all are.
		again_->setText( absent > 0 ? QStringLiteral( "Download selected (%1)" ).arg( translation_install::megabytes( absent ) ) : QStringLiteral( "Download again" ) );
		again_->setEnabled( !busy && !chosen.empty() );
		again_->setToolTip( absent > 0 ? QStringLiteral( "Downloads the selected models that are not here yet" ) : QStringLiteral( "Downloads the selected models afresh, replacing what is here" ) );

		// Remove says what it would delete: the weights a language has but does not translate with, else its model.
		const auto    deletable = removable();
		std::uint64_t bytes     = 0;
		bool          spare     = !deletable.empty();
		for ( const Removal& model : deletable )
		{
			spare = spare && model.precision.has_value();
			for ( const translate::Precision weights : { translate::Precision::Compact, translate::Precision::Full } )
			{
				if ( model.precision.value_or( weights ) == weights && translation_install::installed( *model.language, weights ) )
				{
					bytes += translation_install::size( model.language->translationModel(), weights );
				}
			}
		}
		remove_->setText( deletable.empty() ? QStringLiteral( "Remove" ) : spare ? QStringLiteral( "Remove the weights not in use (%1)" ).arg( translation_install::megabytes( bytes ) )
		                                                                         : QStringLiteral( "Remove %1 (%2)" ).arg( deletable.size() == 1 ? QStringLiteral( "the model" ) : QStringLiteral( "the models" ), translation_install::megabytes( bytes ) ) );
		remove_->setEnabled( !busy && !deletable.empty() );
		remove_->setToolTip( spare ? QStringLiteral( "Deletes the kind of weights the selected languages are not translating with; what they do translate with stays" ) : QStringLiteral( "Deletes the selected models from this computer; they can be downloaded again" ) );
	}

	void TranslationPage::showDetails()
	{
		const lang::Language* current = languageOf( table_->currentItem() );
		if ( current == nullptr )
		{
			details_->clear();
			details_html_.clear();
			return;
		}
		const lang::Language& language  = *current;
		const auto&           model     = language.translationModel();
		const auto            precision = precisionOf( language );
		const auto            other     = translate::otherPrecision( precision );
		QString               html      = QStringLiteral( "<b>%1 → English</b>: %2 weights, %3." )
		                                          .arg( qs( language.name() ).toHtmlEscaped(), precisionLabel( precision ).toLower(), translation_install::installed( language, precision ) ? QStringLiteral( "downloaded" ) : QStringLiteral( "not downloaded" ) );
		if ( translation_install::installed( language, other ) )
		{
			html += translation_install::installed( language, precision )
			              ? QStringLiteral( " The %1 ones are here too, and can be removed." ).arg( precisionLabel( other ).toLower() )
			              : QStringLiteral( " The %1 ones translate until they are." ).arg( precisionLabel( other ).toLower() );
		}
		html += QStringLiteral( "<br>An <a href=\"https://github.com/Helsinki-NLP/Opus-MT\">OPUS-MT</a> model of the University of Helsinki (CC BY 4.0), "
		                        "as <a href=\"https://huggingface.co/%1\">%1</a> converts it for ONNX Runtime, revision %2." )
		                .arg( qs( model.repository ).toHtmlEscaped(), qs( model.revision.substr( 0, 7 ) ) );
		if ( const QStringList others = sharing( language, languages_ ); !others.isEmpty() )
		{
			html += QStringLiteral( " The same model translates %1." ).arg( others.join( QStringLiteral( ", " ) ).toHtmlEscaped() );
		}
		html += QStringLiteral( "<br>Kept in %1" ).arg( QDir::toNativeSeparators( translation_install::folder( language ) ).toHtmlEscaped() );
		if ( html != details_html_ )
		{
			details_html_ = html;
			details_->setHtml( html );
		}
	}

	std::vector<translation_install::Wanted> TranslationPage::selected() const
	{
		std::vector<translation_install::Wanted> out;
		for ( const QTreeWidgetItem* item : table_->selectedItems() )
		{
			if ( const lang::Language* language = languageOf( item ); language != nullptr )
			{
				out.push_back( { .language = language, .precision = precisionOf( *language ) } );
			}
		}
		return out;
	}

	std::vector<translation_install::Wanted> TranslationPage::missing() const
	{
		const auto&                              off = settings().config().disabled_languages;
		std::vector<translation_install::Wanted> out;
		for ( std::size_t row = 0; row < languages_.size(); ++row )
		{
			const lang::Language& language  = *languages_[row];
			const auto            precision = precisionOf( language );
			if ( table_->topLevelItem( static_cast<int>( row ) )->checkState( language_column ) == Qt::Checked && !std::ranges::contains( off, language.code() ) &&
			     !translation_install::installed( language, precision ) )
			{
				out.push_back( { .language = &language, .precision = precision } );
			}
		}
		return out;
	}

	std::vector<TranslationPage::Removal> TranslationPage::removable() const
	{
		std::vector<Removal> out;
		for ( const translation_install::Wanted& model : translation_install::distinct( selected() ) )
		{
			const lang::Language& language = *model.language;
			const auto            spare    = translate::otherPrecision( model.precision );
			// Both kinds are here: the one this language does not translate with goes, and its model stays working.
			if ( translation_install::installed( language, model.precision ) && translation_install::installed( language, spare ) )
			{
				out.push_back( { .language = &language, .precision = spare } );
			}
			else if ( QDir( translation_install::folder( language ) ).exists() )
			{
				out.push_back( { .language = &language, .precision = std::nullopt } );
			}
		}
		return out;
	}

	void TranslationPage::download( std::vector<translation_install::Wanted> models, bool again )
	{
		if ( models.empty() || installer_->busy() )
		{
			return;
		}
		// Every language of a model being downloaded shows it.
		downloading_.clear();
		for ( const lang::Language* language : languages_ )
		{
			if ( std::ranges::any_of( models, [&]( const translation_install::Wanted& chosen ) { return chosen.language->translationModel().directory() == language->translationModel().directory(); } ) )
			{
				downloading_.push_back( language );
			}
		}
		activity_->setText( QStringLiteral( "Downloading %1 (%2)..." ).arg( again ? QStringLiteral( "again" ) : QStringLiteral( "the models" ), translation_install::megabytes( translation_install::bytes( models, again ) ) ) );
		activity_->show();
		progress_->setRange( 0, 0 );
		progress_->show();
		installer_->install(
				std::move( models ),
				again,
				[this]( qint64 received, qint64 total ) { showProgress( progress_, received, total ); },
				[this]( const QString& error ) {
					downloading_.clear();
					progress_->hide();
					activity_->setText( error.isEmpty() ? QStringLiteral( "The models are ready." ) : QStringLiteral( "Download failed: %1" ).arg( error ) );
					updateRows();
				}
		);
		updateRows();
	}

	void TranslationPage::removeSelected()
	{
		QStringList problems;
		QStringList removed;
		for ( const Removal& model : removable() )
		{
			const lang::Language& language = *model.language;
			if ( const QString problem = translation_install::remove( language, model.precision ); !problem.isEmpty() )
			{
				problems << problem;
				continue;
			}
			removed << QStringLiteral( "%1 (%2)" ).arg( qs( language.translationModel().directory() ), model.precision ? precisionLabel( *model.precision ).toLower() : QStringLiteral( "all weights" ) );
		}
		// The daemon lets go of models it has loaded, so what it translates next is what is here.
		client().call( "translation.reload" );
		activity_->setText( !problems.isEmpty() ? problems.join( '\n' ) : QStringLiteral( "Removed %1." ).arg( removed.join( QStringLiteral( ", " ) ) ) );
		activity_->show();
		updateRows();
	}

	void TranslationPage::translate()
	{
		const QString text = test_text_->text().trimmed();
		if ( text.isEmpty() )
		{
			return;
		}
		json::Writer params;
		params.beginObject().field( "text", ss( text ) );
		if ( const QString code = test_language_->currentData().toString(); !code.isEmpty() )
		{
			params.field( "language", ss( code ) );
		}
		params.endObject();
		test_button_->setEnabled( false );
		test_result_->setText( QStringLiteral( "Translating..." ) );
		const auto started = std::chrono::steady_clock::now();
		client().call(
				"translate",
				params.take(),
				[this, started]( const json::Value* result, const QString& error ) {
					test_button_->setEnabled( true );
					if ( result == nullptr )
					{
						test_result_->setText( QStringLiteral( "No translation: %1" ).arg( error ) );
						return;
					}
					const auto        took     = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();
					const auto* const language = lang::findLanguage( ( *result )["language"].asString() );
					test_result_->setText( QStringLiteral( "<b>%1</b><br><span style=\"color: %2\">%3, %4 ms</span>" )
			                                       .arg( qs( ( *result )["translation"].asString() ).toHtmlEscaped(), palette().color( QPalette::Disabled, QPalette::Text ).name(), language != nullptr ? qs( language->name() ) : QString(), QString::number( took ) ) );
				},
				60000
		);
	}

} // namespace lexiglance::gui
