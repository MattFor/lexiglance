#include "DictionariesPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/language/Language.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QMimeData>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace lexiglance::gui
{

	namespace
	{

		struct Recommendation
		{
			QString name;
			QString category;
			QString description;
			QString homepage;
			QString download;
			QString title;
			QString title_prefix;

			[[nodiscard]] bool matches( const QString& installed ) const
			{
				return ( !title.isEmpty() && installed == title ) || ( !title_prefix.isEmpty() && installed.startsWith( title_prefix ) );
			}
		};

		std::vector<Recommendation> recommendations( const std::string& language )
		{
			std::vector<Recommendation> out;
			const auto*                 found = lang::findLanguage( language );
			if ( found == nullptr )
			{
				return out;
			}
			for ( const lang::Recommendation& item : found->recommendedDictionaries() )
			{
				out.push_back(
						{
								.name         = qs( item.name ),
								.category     = qs( item.category ),
								.description  = qs( item.description ),
								.homepage     = qs( item.homepage ),
								.download     = qs( item.download ),
								.title        = qs( item.title ),
								.title_prefix = qs( item.title_prefix ),
						}
				);
			}
			return out;
		}

		QString kindOf( const InstalledDictionary& d )
		{
			QStringList kinds;
			if ( d.terms > 0 )
			{
				kinds << QStringLiteral( "Terms" );
			}
			if ( d.kanji > 0 )
			{
				kinds << QStringLiteral( "Kanji" );
			}
			if ( d.meta > 0 )
			{
				kinds << QStringLiteral( "Frequency / pitch" );
			}
			return kinds.join( QStringLiteral( ", " ) );
		}

		QString downloadPath( const QString& name )
		{
			QString safe = name;
			safe.replace( QRegularExpression( QStringLiteral( "[^A-Za-z0-9._-]" ) ), QStringLiteral( "_" ) );
			return qs( ( paths::cacheDir() / "downloads" ).string() ) + "/" + safe + ".zip";
		}

		// A disabled dictionary's row is shown in the disabled text colour.
		void styleRow( QTreeWidgetItem* item, bool enabled, const QColor& disabled )
		{
			for ( int column = 0; column < item->columnCount(); ++column )
			{
				if ( enabled )
				{
					item->setData( column, Qt::ForegroundRole, QVariant() );
				}
				else
				{
					item->setForeground( column, disabled );
				}
			}
		}

		// The priority list: rows are dragged to another place. Files dragged in from elsewhere are left to the page,
		// which imports them.
		class DictionaryTree final : public QTreeWidget
		{
		public:
			std::function<void()> reordered;

		protected:
			void dragEnterEvent( QDragEnterEvent* event ) override
			{
				if ( event->source() == this )
				{
					QTreeWidget::dragEnterEvent( event );
				}
				else
				{
					event->ignore();
				}
			}

			void dragMoveEvent( QDragMoveEvent* event ) override
			{
				if ( event->source() == this )
				{
					QTreeWidget::dragMoveEvent( event );
				}
				else
				{
					event->ignore();
				}
			}

			void dropEvent( QDropEvent* event ) override
			{
				if ( event->source() != this )
				{
					event->ignore();
					return;
				}
				QTreeWidget::dropEvent( event );
				if ( reordered )
				{
					// After the view has finished moving the row.
					QTimer::singleShot( 0, this, [this] { reordered(); } );
				}
			}
		};

	} // namespace

	DictionariesPage::DictionariesPage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		downloader_( new Downloader( this ) ),
		table_( new DictionaryTree() ),
		details_( new QTextBrowser() ),
		progress_( new QProgressBar() ),
		activity_( new QLabel() ),
		remove_( new QPushButton( QIcon::fromTheme( QStringLiteral( "edit-delete" ) ), QStringLiteral( "Remove" ) ) ),
		up_( new QPushButton( QIcon::fromTheme( QStringLiteral( "go-up" ) ), QString() ) ),
		down_( new QPushButton( QIcon::fromTheme( QStringLiteral( "go-down" ) ), QString() ) ),
		sort_( new QPushButton( QIcon::fromTheme( QStringLiteral( "view-sort-ascending" ) ), QStringLiteral( "Sort automatically..." ) ) )
	{
		setAcceptDrops( true );
		auto* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 24, 20, 24, 20 );
		layout->setSpacing( 10 );

		auto* intro = new QLabel( QStringLiteral( "Lexiglance reads Yomitan dictionaries. Results are shown in the order below: drag a dictionary up or down to change it, "
		                                          "or let Sort automatically put word dictionaries first, then names, kanji and frequency lists. Drag .zip files here to import them." ) );
		intro->setWordWrap( true );
		layout->addWidget( intro );

		auto* toolbar     = new QHBoxLayout();
		auto* recommended = new QPushButton( QIcon::fromTheme( QStringLiteral( "folder-download" ) ), QStringLiteral( "Get recommended dictionaries..." ) );
		auto* import      = new QPushButton( QIcon::fromTheme( QStringLiteral( "document-open" ) ), QStringLiteral( "Import from file..." ) );
		auto* updates     = new QPushButton( QIcon::fromTheme( QStringLiteral( "view-refresh" ) ), QStringLiteral( "Check for updates" ) );
		up_->setToolTip( QStringLiteral( "Higher priority" ) );
		down_->setToolTip( QStringLiteral( "Lower priority" ) );
		toolbar->addWidget( recommended );
		toolbar->addWidget( import );
		toolbar->addWidget( updates );
		toolbar->addStretch( 1 );
		sort_->setToolTip( QStringLiteral( "Word dictionaries first (the best-known ones leading), then names, grammar, specialised and monolingual ones, kanji, frequency and pitch lists" ) );
		toolbar->addWidget( sort_ );
		toolbar->addWidget( up_ );
		toolbar->addWidget( down_ );
		toolbar->addWidget( remove_ );
		layout->addLayout( toolbar );

		table_->setColumnCount( 5 );
		table_->setHeaderLabels(
				{ QStringLiteral( "Dictionary" ), QStringLiteral( "Type" ), QStringLiteral( "Entries" ), QStringLiteral( "Size" ), QStringLiteral( "Revision" ) }
		);
		table_->setRootIsDecorated( false );
		table_->setAlternatingRowColors( true );
		table_->setUniformRowHeights( true );
		// Rows move by dragging; they are never dropped into one another.
		table_->setSelectionMode( QAbstractItemView::SingleSelection );
		table_->setDragEnabled( true );
		table_->setAcceptDrops( true );
		table_->viewport()->setAcceptDrops( true );
		table_->setDropIndicatorShown( true );
		table_->setDragDropMode( QAbstractItemView::InternalMove );
		table_->setDefaultDropAction( Qt::MoveAction );
		if ( auto* tree = dynamic_cast<DictionaryTree*>( table_ ) )
		{
			tree->reordered = [this] { reorder(); };
		}
		table_->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
		for ( int column = 1; column < 5; ++column )
		{
			table_->header()->setSectionResizeMode( column, QHeaderView::ResizeToContents );
		}
		layout->addWidget( table_, 3 );

		details_->setOpenExternalLinks( true );
		details_->setMaximumHeight( 150 );
		layout->addWidget( details_, 1 );

		auto* status = new QHBoxLayout();
		status->addWidget( activity_, 1 );
		status->addWidget( progress_, 1 );
		layout->addLayout( status );
		progress_->hide();
		activity_->hide();

		connect( recommended, &QPushButton::clicked, this, [this] { showRecommended(); } );
		connect( import, &QPushButton::clicked, this, [this] {
			const auto files = QFileDialog::getOpenFileNames(
					this,
					QStringLiteral( "Import Yomitan dictionaries" ),
					QStandardPaths::writableLocation( QStandardPaths::DownloadLocation ),
					QStringLiteral( "Yomitan dictionaries (*.zip)" )
			);
			importFiles( files );
		} );
		connect( updates, &QPushButton::clicked, this, [this] { checkUpdates(); } );
		connect( sort_, &QPushButton::clicked, this, [this] { sortAutomatically(); } );
		connect( up_, &QPushButton::clicked, this, [this] { move( -1 ); } );
		connect( down_, &QPushButton::clicked, this, [this] { move( 1 ); } );
		connect( remove_, &QPushButton::clicked, this, [this] { removeSelected(); } );
		connect( table_, &QTreeWidget::currentItemChanged, this, [this] { showDetails(); } );
		connect( table_, &QTreeWidget::itemChanged, this, [this]( QTreeWidgetItem* item, int column ) {
			if ( rebuilding_ || column != 0 )
			{
				return;
			}
			const int row   = table_->indexOfTopLevelItem( item );
			auto&     prefs = settings().config().dictionaries;
			if ( row >= 0 && static_cast<std::size_t>( row ) < prefs.size() )
			{
				const bool enabled                             = item->checkState( 0 ) == Qt::Checked;
				prefs[static_cast<std::size_t>( row )].enabled = enabled;
				settings().commit();
				if ( static_cast<std::size_t>( row ) < installed_.size() )
				{
					installed_[static_cast<std::size_t>( row )].enabled = enabled;
				}
				// Restyling changes the item again; the guard keeps that from coming back here.
				rebuilding_ = true;
				styleRow( item, enabled, palette().color( QPalette::Disabled, QPalette::Text ) );
				rebuilding_ = false;
				updateSummary();
			}
		} );

		client().onEvent( [this]( std::string_view name, const json::Value& params ) {
			if ( name == "import.started" )
			{
				setBusy( QStringLiteral( "Importing %1..." ).arg( QFileInfo( qs( params["path"].asString() ) ).fileName() ), 0, 0 );
			}
			else if ( name == "import.progress" )
			{
				const auto total = static_cast<int>( params["total"].asInt() );
				setBusy( QStringLiteral( "Importing (%1)..." ).arg( qs( params["stage"].asString() ) ), static_cast<int>( params["done"].asInt() ), total );
			}
			else if ( name == "import.finished" )
			{
				if ( params["ok"].asBool() )
				{
					setBusy( QStringLiteral( "Installed %1 (%2 entries) in %3 s" )
					                 .arg( qs( params["title"].asString() ) )
					                 .arg( params["terms"].asInt() + params["meta"].asInt() + params["kanji"].asInt() )
					                 .arg( params["seconds"].asDouble(), 0, 'f', 1 ),
					         -1,
					         0 );
				}
				else
				{
					setBusy( QStringLiteral( "Import failed: %1" ).arg( qs( params["error"].asString() ) ), -1, 0 );
				}
				startNextDownload();
			}
			else if ( name == "dictionaries.changed" )
			{
				reload();
			}
		} );
		client().onConnection( [this]( bool connected ) {
			if ( connected )
			{
				reload();
			}
		} );
	}

	void DictionariesPage::activated()
	{
		reload();
	}

	void DictionariesPage::refresh()
	{
		reload();
	}

	void DictionariesPage::updateSummary()
	{
		std::uint64_t on_disk = 0;
		const auto    enabled = std::ranges::count_if( installed_, [&]( const auto& d ) {
            on_disk += d.size;
            return d.enabled;
        } );
		showSummary( QStringLiteral( "%1 of %2 dictionaries enabled · %3 on disk" ).arg( enabled ).arg( installed_.size() ).arg( formatBytes( on_disk ) ) );
	}

	void DictionariesPage::reload()
	{
		client().call( "dictionaries.list", "{}", [this]( const json::Value* result, const QString& ) {
			if ( result == nullptr )
			{
				return;
			}
			installed_.clear();
			for ( const json::Value& d : ( *result )["dictionaries"].items() )
			{
				installed_.push_back(
						{
								.title        = qs( d["title"].asString() ),
								.revision     = qs( d["revision"].asString() ),
								.description  = qs( d["description"].asString() ),
								.attribution  = qs( d["attribution"].asString() ),
								.url          = qs( d["url"].asString() ),
								.index_url    = qs( d["index_url"].asString() ),
								.download_url = qs( d["download_url"].asString() ),
								.kind         = qs( d["kind"].asString() ),
								.enabled      = d["enabled"].asBool( true ),
								.updatable    = d["updatable"].asBool(),
								.terms        = d["terms"].asInt(),
								.meta         = d["meta"].asInt(),
								.kanji        = d["kanji"].asInt(),
								.size         = static_cast<std::uint64_t>( d["size"].asInt() ),
						}
				);
			}
			rebuildTable();
		} );
	}

	void DictionariesPage::rebuildTable()
	{
		rebuilding_       = true;
		const int current = table_->indexOfTopLevelItem( table_->currentItem() );
		table_->clear();
		for ( const auto& d : installed_ )
		{
			auto* item = new QTreeWidgetItem( table_ );
			item->setFlags( ( item->flags() | Qt::ItemIsDragEnabled ) & ~Qt::ItemIsDropEnabled );
			item->setText( 0, d.title );
			item->setCheckState( 0, d.enabled ? Qt::Checked : Qt::Unchecked );
			item->setText( 1, d.kind.isEmpty() ? kindOf( d ) : d.kind );
			item->setToolTip( 1, kindOf( d ) );
			item->setText( 2, QLocale().toString( static_cast<qlonglong>( d.terms ) + d.meta + d.kanji ) );
			item->setText( 3, formatBytes( d.size ) );
			item->setText( 4, d.revision );
			item->setToolTip( 0, d.description );
			styleRow( item, d.enabled, palette().color( QPalette::Disabled, QPalette::Text ) );
		}
		updateSummary();
		if ( current >= 0 && current < table_->topLevelItemCount() )
		{
			table_->setCurrentItem( table_->topLevelItem( current ) );
		}
		if ( table_->currentItem() == nullptr && table_->topLevelItemCount() > 0 )
		{
			table_->setCurrentItem( table_->topLevelItem( 0 ) );
		}
		rebuilding_ = false;

		if ( installed_.empty() )
		{
			QStringList languages;
			for ( const lang::Language* language : lang::languages() )
			{
				if ( !recommendations( std::string( language->code() ) ).empty() )
				{
					languages << qs( language->name() );
				}
			}
			details_->setHtml( QStringLiteral( "<h3>No dictionaries installed</h3><p>Click <b>Get recommended dictionaries</b> to download a good starter set for %1 in one go.</p>" )
			                           .arg( languages.join( QStringLiteral( " or " ) ) ) );
		}
		showDetails();
	}

	void DictionariesPage::showDetails()
	{
		const int row = table_->indexOfTopLevelItem( table_->currentItem() );
		remove_->setEnabled( row >= 0 );
		up_->setEnabled( row > 0 );
		down_->setEnabled( row >= 0 && row + 1 < table_->topLevelItemCount() );
		if ( row < 0 || static_cast<std::size_t>( row ) >= installed_.size() )
		{
			return;
		}
		const auto& d    = installed_[static_cast<std::size_t>( row )];
		QString     html = QStringLiteral( "<b>%1</b>" ).arg( d.title.toHtmlEscaped() );
		if ( !d.url.isEmpty() )
		{
			html += QStringLiteral( " &nbsp; <a href=\"%1\">%1</a>" ).arg( d.url.toHtmlEscaped() );
		}
		if ( !d.description.isEmpty() )
		{
			html += "<p>" + d.description.toHtmlEscaped().replace( '\n', QStringLiteral( "<br>" ) ) + "</p>";
		}
		if ( !d.attribution.isEmpty() )
		{
			html += "<p><small>" + d.attribution.toHtmlEscaped().replace( '\n', QStringLiteral( "<br>" ) ) + "</small></p>";
		}
		details_->setHtml( html );
	}

	void DictionariesPage::setBusy( const QString& text, int value, int maximum )
	{
		activity_->setVisible( !text.isEmpty() );
		activity_->setText( text );
		if ( value < 0 )
		{
			progress_->hide();
			return;
		}
		progress_->show();
		progress_->setRange( 0, maximum );
		progress_->setValue( value );
	}

	void DictionariesPage::importFiles( const QStringList& files )
	{
		for ( const QString& file : files )
		{
			json::Writer params;
			params.beginObject().field( "path", ss( file ) ).endObject();
			client().call( "dictionaries.import", params.take(), [this]( const json::Value*, const QString& error ) {
				if ( !error.isEmpty() )
				{
					QMessageBox::warning( this, QStringLiteral( "Import" ), error );
				}
			} );
		}
	}

	void DictionariesPage::dragEnterEvent( QDragEnterEvent* event )
	{
		if ( event->mimeData()->hasUrls() )
		{
			event->acceptProposedAction();
		}
	}

	void DictionariesPage::dropEvent( QDropEvent* event )
	{
		QStringList files;
		for ( const QUrl& url : event->mimeData()->urls() )
		{
			if ( url.isLocalFile() )
			{
				files << url.toLocalFile();
			}
		}
		importFiles( files );
	}

	void DictionariesPage::move( int delta )
	{
		const int row    = table_->indexOfTopLevelItem( table_->currentItem() );
		auto&     prefs  = settings().config().dictionaries;
		const int target = row + delta;
		if ( row < 0 || target < 0 || static_cast<std::size_t>( target ) >= prefs.size() || static_cast<std::size_t>( row ) >= installed_.size() )
		{
			return;
		}
		std::swap( prefs[static_cast<std::size_t>( row )], prefs[static_cast<std::size_t>( target )] );
		std::swap( installed_[static_cast<std::size_t>( row )], installed_[static_cast<std::size_t>( target )] );
		settings().commit();
		rebuildTable();
		table_->setCurrentItem( table_->topLevelItem( target ) );
	}

	void DictionariesPage::reorder()
	{
		auto&                                     current = settings().config().dictionaries;
		std::vector<InstalledDictionary>          installed;
		std::vector<config::DictionaryPreference> preferences;
		for ( int row = 0; row < table_->topLevelItemCount(); ++row )
		{
			const QString title = table_->topLevelItem( row )->text( 0 );
			if ( const auto it = std::ranges::find( installed_, title, &InstalledDictionary::title ); it != installed_.end() )
			{
				installed.push_back( *it );
			}
			if ( const auto it = std::ranges::find( current, ss( title ), &config::DictionaryPreference::title ); it != current.end() )
			{
				preferences.push_back( *it );
			}
		}
		// Preferences without a row (not installed right now) stay, at the end.
		for ( const auto& preference : current )
		{
			if ( std::ranges::find( preferences, preference.title, &config::DictionaryPreference::title ) == preferences.end() )
			{
				preferences.push_back( preference );
			}
		}
		const QString selected = table_->currentItem() != nullptr ? table_->currentItem()->text( 0 ) : QString();
		installed_             = std::move( installed );
		current                = std::move( preferences );
		settings().commit();
		rebuildTable();
		for ( int row = 0; row < table_->topLevelItemCount(); ++row )
		{
			if ( table_->topLevelItem( row )->text( 0 ) == selected )
			{
				table_->setCurrentItem( table_->topLevelItem( row ) );
			}
		}
	}

	void DictionariesPage::sortAutomatically()
	{
		client().call( "dictionaries.sort", R"({"dry_run":true})", [this]( const json::Value* result, const QString& error ) {
			if ( result == nullptr )
			{
				QMessageBox::warning( this, QStringLiteral( "Sort automatically" ), error );
				return;
			}
			QString list;
			int     number = 0;
			for ( const json::Value& entry : ( *result )["order"].items() )
			{
				list += QStringLiteral( "%1. %2  —  %3\n" ).arg( ++number ).arg( qs( entry["title"].asString() ), qs( entry["kind"].asString() ) );
			}
			QMessageBox question( QMessageBox::Question, QStringLiteral( "Sort automatically" ), QStringLiteral( "Put the dictionaries in this order? Which are enabled does not change." ), QMessageBox::Yes | QMessageBox::Cancel, this );
			question.setDetailedText( list );
			question.setInformativeText( list.section( QLatin1Char( '\n' ), 0, 7 ) + ( number > 8 ? QStringLiteral( "..." ) : QString() ) );
			if ( question.exec() != QMessageBox::Yes )
			{
				return;
			}
			client().call( "dictionaries.sort", "{}", [this]( const json::Value* applied, const QString& apply_error ) {
				if ( applied == nullptr )
				{
					QMessageBox::warning( this, QStringLiteral( "Sort automatically" ), apply_error );
					return;
				}
				settings().load();
				reload();
			} );
		} );
	}

	void DictionariesPage::removeSelected()
	{
		const int row = table_->indexOfTopLevelItem( table_->currentItem() );
		if ( row < 0 || static_cast<std::size_t>( row ) >= installed_.size() )
		{
			return;
		}
		const QString title = installed_[static_cast<std::size_t>( row )].title;
		if ( QMessageBox::question( this, QStringLiteral( "Remove dictionary" ), QStringLiteral( "Remove \"%1\"?" ).arg( title ) ) != QMessageBox::Yes )
		{
			return;
		}
		json::Writer params;
		params.beginObject().field( "title", ss( title ) ).endObject();
		client().call( "dictionaries.remove", params.take(), [this]( const json::Value*, const QString& error ) {
			if ( !error.isEmpty() )
			{
				QMessageBox::warning( this, QStringLiteral( "Remove dictionary" ), error );
			}
		} );
	}

	void DictionariesPage::enqueueDownload( Download download )
	{
		downloads_.push_back( std::move( download ) );
		if ( !downloading_ )
		{
			startNextDownload();
		}
	}

	// Downloads run one at a time; each is imported (in the daemon) before the next starts.
	void DictionariesPage::startNextDownload()
	{
		if ( downloads_.empty() )
		{
			downloading_ = false;
			return;
		}
		downloading_        = true;
		const Download next = downloads_.front();
		downloads_.pop_front();
		const QString target = downloadPath( next.name );

		setBusy( QStringLiteral( "Downloading %1..." ).arg( next.name ), 0, 0 );
		downloader_->download(
				next.url,
				target,
				[this, name = next.name]( qint64 received, qint64 total ) {
					if ( total > 0 )
					{
						setBusy( QStringLiteral( "Downloading %1 (%2 of %3)..." )
				                         .arg( name, formatBytes( static_cast<std::uint64_t>( received ) ), formatBytes( static_cast<std::uint64_t>( total ) ) ),
				                 static_cast<int>( received / 1024 ),
				                 static_cast<int>( total / 1024 ) );
					}
					else
					{
						setBusy( QStringLiteral( "Downloading %1 (%2)..." ).arg( name, formatBytes( static_cast<std::uint64_t>( received ) ) ), 0, 0 );
					}
				},
				[this, target, next]( const QString& error ) {
					if ( !error.isEmpty() )
					{
						setBusy( QStringLiteral( "Download of %1 failed: %2" ).arg( next.name, error ), -1, 0 );
						startNextDownload();
						return;
					}
					json::Writer params;
					params.beginObject().field( "path", ss( target ) ).field( "delete_source", true ).field( "replace", true ).field( "replaces", ss( next.replaces ) ).endObject();
					client().call( "dictionaries.import", params.take(), [this, next]( const json::Value*, const QString& import_error ) {
						if ( !import_error.isEmpty() )
						{
							setBusy( QStringLiteral( "Import of %1 failed: %2" ).arg( next.name, import_error ), -1, 0 );
							startNextDownload();
						}
					} );
				}
		);
	}

	void DictionariesPage::showRecommended()
	{
		QDialog dialog( this );
		dialog.setWindowTitle( QStringLiteral( "Recommended dictionaries" ) );
		dialog.resize( 760, 480 );
		auto* layout = new QVBoxLayout( &dialog );
		auto* intro  = new QLabel( QStringLiteral( "Select the dictionaries to download and install. They are fetched from their authors' official releases." ) );
		intro->setWordWrap( true );
		layout->addWidget( intro );

		auto* language = new QComboBox();
		for ( const lang::Language* l : lang::languages() )
		{
			if ( !recommendations( std::string( l->code() ) ).empty() )
			{
				language->addItem( qs( l->name() ), qs( l->code() ) );
			}
		}
		auto* language_row = new QHBoxLayout();
		language_row->addWidget( new QLabel( QStringLiteral( "Language:" ) ) );
		language_row->addWidget( language );
		language_row->addStretch( 1 );
		layout->addLayout( language_row );

		auto* tree = new QTreeWidget();
		tree->setHeaderLabels( { QStringLiteral( "Dictionary" ), QStringLiteral( "Type" ), QStringLiteral( "Description" ) } );
		tree->setRootIsDecorated( false );
		tree->setWordWrap( true );
		tree->header()->setSectionResizeMode( 2, QHeaderView::Stretch );

		const auto                  installed = [this]( const Recommendation& r ) { return std::ranges::any_of( installed_, [&]( const InstalledDictionary& d ) { return r.matches( d.title ); } ); };
		std::vector<Recommendation> list;
		const auto                  fill = [&list, &installed, tree, language] {
            tree->clear();
            list = recommendations( ss( language->currentData().toString() ) );
            for ( const auto& r : list )
            {
                const bool done = installed( r );
                auto*      item = new QTreeWidgetItem( tree );
                item->setText( 0, done ? r.name + QStringLiteral( "  ✓" ) : r.name );
                item->setText( 1, r.category );
                item->setText( 2, r.description );
                item->setToolTip( 2, r.description + "\n\n" + r.homepage );
                item->setCheckState( 0, done ? Qt::Unchecked : Qt::Checked );
                if ( done )
                {
                    item->setFlags( item->flags() & ~Qt::ItemIsEnabled );
                    item->setToolTip( 0, QStringLiteral( "Already installed" ) );
                }
            }
            tree->resizeColumnToContents( 0 );
            tree->resizeColumnToContents( 1 );
		};
		// The preferred language first, unless its dictionaries are all installed: then the next one that has some left.
		const int preferred = std::max( 0, language->findData( qs( settings().config().language ) ) );
		int       start     = preferred;
		for ( int i = 0; i < language->count(); ++i )
		{
			if ( const int candidate = ( preferred + i ) % language->count(); !std::ranges::all_of( recommendations( ss( language->itemData( candidate ).toString() ) ), installed ) )
			{
				start = candidate;
				break;
			}
		}
		language->setCurrentIndex( start );
		fill();
		connect( language, &QComboBox::currentIndexChanged, &dialog, fill );
		layout->addWidget( tree, 1 );

		auto* buttons = new QDialogButtonBox( QDialogButtonBox::Cancel );
		buttons->addButton( QStringLiteral( "Install selected" ), QDialogButtonBox::AcceptRole );
		layout->addWidget( buttons );
		connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
		connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );

		if ( dialog.exec() != QDialog::Accepted )
		{
			return;
		}
		for ( int i = 0; i < tree->topLevelItemCount(); ++i )
		{
			if ( tree->topLevelItem( i )->checkState( 0 ) == Qt::Checked && ( tree->topLevelItem( i )->flags() & Qt::ItemIsEnabled ) != 0 )
			{
				const auto& r = list[static_cast<std::size_t>( i )];
				enqueueDownload( { .name = r.name, .url = QUrl( r.download ) } );
			}
		}
	}

	void DictionariesPage::checkUpdates()
	{
		std::vector<InstalledDictionary> candidates;
		std::ranges::copy_if( installed_, std::back_inserter( candidates ), []( const InstalledDictionary& d ) { return d.updatable && !d.index_url.isEmpty(); } );
		if ( candidates.empty() )
		{
			setBusy( QStringLiteral( "None of the installed dictionaries can be updated automatically." ), -1, 0 );
			return;
		}

		setBusy( QStringLiteral( "Checking %1 dictionaries for updates..." ).arg( candidates.size() ), 0, 0 );
		auto remaining = std::make_shared<std::size_t>( candidates.size() );
		auto updates   = std::make_shared<std::vector<Download>>();
		for ( const auto& d : candidates )
		{
			downloader_->fetch( QUrl( d.index_url ), [this, d, remaining, updates]( const QByteArray& body, const QString& error ) {
				if ( error.isEmpty() )
				{
					if ( auto index = json::Document::parse( body.toStdString() ) )
					{
						const QString revision = qs( index->root()["revision"].asString() );
						const QString download = qs( index->root()["downloadUrl"].asString() );
						if ( !revision.isEmpty() && revision != d.revision )
						{
							updates->push_back( { .name = shortTitle( d.title ), .url = QUrl( download.isEmpty() ? d.download_url : download ), .replaces = d.title } );
						}
					}
				}
				if ( --*remaining > 0 )
				{
					return;
				}
				if ( updates->empty() )
				{
					setBusy( QStringLiteral( "All dictionaries are up to date." ), -1, 0 );
					return;
				}
				QStringList names;
				for ( const auto& u : *updates )
				{
					names << u.name;
				}
				setBusy( QString(), -1, 0 );
				if ( QMessageBox::question(
							 this,
							 QStringLiteral( "Dictionary updates" ),
							 QStringLiteral( "Updates are available for:\n\n%1\n\nDownload and install them now?" ).arg( names.join( '\n' ) )
					 ) == QMessageBox::Yes )
				{
					for ( auto& u : *updates )
					{
						enqueueDownload( std::move( u ) );
					}
				}
			} );
		}
	}

} // namespace lexiglance::gui
