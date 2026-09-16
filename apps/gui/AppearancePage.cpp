#include "AppearancePage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>
#include <vector>

namespace lexiglance::gui
{

	namespace
	{

		QSpinBox* spin( int min, int max, const QString& suffix = QString(), const QString& special = QString() )
		{
			auto* box = new QSpinBox();
			box->setRange( min, max );
			box->setSuffix( suffix );
			if ( !special.isEmpty() )
			{
				box->setSpecialValueText( special );
			}
			return box;
		}

		// Sized to a short entry, so a long one does not widen the settings column past its scroll area.
		QComboBox* combo( const QStringList& items )
		{
			auto* box = new QComboBox();
			box->addItems( items );
			box->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
			box->setMinimumContentsLength( 12 );
			return box;
		}

		int themeIndex( config::Theme theme )
		{
			switch ( theme )
			{
				case config::Theme::Dark:
					return 1;
				case config::Theme::Light:
					return 2;
				case config::Theme::Auto:
					break;
			}
			return 0;
		}

		config::Theme themeAt( int index )
		{
			switch ( index )
			{
				case 1:
					return config::Theme::Dark;
				case 2:
					return config::Theme::Light;
				default:
					return config::Theme::Auto;
			}
		}

		QColor parseColor( const std::string& hex )
		{
			QColor color( qs( hex ).left( 7 ) );
			if ( hex.size() == 9 )
			{
				color.setAlpha( qs( hex ).mid( 7, 2 ).toInt( nullptr, 16 ) );
			}
			return color;
		}

		std::string formatColor( const QColor& color )
		{
			return ss( color.name( QColor::HexRgb ) + QStringLiteral( "%1" ).arg( color.alpha(), 2, 16, QLatin1Char( '0' ) ) );
		}

		QIcon swatch( const QColor& color )
		{
			QPixmap image( 28, 16 );
			image.fill( Qt::transparent );
			QPainter painter( &image );
			painter.setRenderHint( QPainter::Antialiasing );
			painter.setPen( QPen( QColor( 128, 128, 128 ), 1 ) );
			painter.setBrush( color );
			painter.drawRoundedRect( QRectF( 0.5, 0.5, 27, 15 ), 3, 3 );
			return { image };
		}

		QPixmap decode( const json::Value& result )
		{
			QPixmap image;
			image.loadFromData( QByteArray::fromBase64( QByteArray::fromStdString( std::string( result["png"].asString() ) ) ), "PNG" );
			image.setDevicePixelRatio( std::max( 0.5, result["scale"].asDouble( 1.0 ) ) );
			return image;
		}

		QGroupBox* group( const QString& title, QLayout* layout )
		{
			auto* box = new QGroupBox( title );
			box->setLayout( layout );
			return box;
		}

		QLabel* note( const QString& text )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			label->setEnabled( false );
			return label;
		}

	} // namespace

	AppearancePage::AppearancePage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		themes_( new QToolButton() ),
		save_theme_( new QPushButton( QStringLiteral( "Save as theme..." ) ) ),
		design_( combo( { QStringLiteral( "Friendly" ), QStringLiteral( "Classic (Yomitan)" ), QStringLiteral( "Compact" ) } ) ),
		scheme_( combo( { QStringLiteral( "Default" ), QStringLiteral( "Paper" ), QStringLiteral( "Nord" ), QStringLiteral( "Sakura" ), QStringLiteral( "Matcha" ), QStringLiteral( "Midnight" ), QStringLiteral( "High contrast" ) } ) ),
		theme_( combo( { QStringLiteral( "Follow the desktop" ), QStringLiteral( "Dark" ), QStringLiteral( "Light" ) } ) ),
#ifdef Q_OS_WIN
		default_font_( new QCheckBox( QStringLiteral( "Default (Yu Gothic UI)" ) ) ),
#else
		default_font_( new QCheckBox( QStringLiteral( "Default (Noto Sans CJK JP)" ) ) ),
#endif
		font_( new QFontComboBox() ),
		font_size_( spin( 6, 72, QStringLiteral( " px" ) ) ),
		headword_size_( spin( 0, 120, QStringLiteral( " px" ), QStringLiteral( "Automatic" ) ) ),
		furigana_size_( spin( 0, 72, QStringLiteral( " px" ), QStringLiteral( "Automatic" ) ) ),
		scale_( new QDoubleSpinBox() ),
		width_( spin( 200, 2000, QStringLiteral( " px" ) ) ),
		height_( spin( 100, 2000, QStringLiteral( " px" ) ) ),
		corner_radius_( spin( 0, 32, QStringLiteral( " px" ), QStringLiteral( "Square" ) ) ),
		border_width_( spin( 0, 8, QStringLiteral( " px" ), QStringLiteral( "None" ) ) ),
		padding_( spin( 0, 40, QStringLiteral( " px" ) ) ),
		opacity_( spin( 30, 100, QStringLiteral( " %" ) ) ),
		placement_( combo( { QStringLiteral( "Below the text" ), QStringLiteral( "Above the text" ) } ) ),
		offset_x_( spin( -500, 500, QStringLiteral( " px" ) ) ),
		offset_y_( spin( -500, 500, QStringLiteral( " px" ) ) ),
		select_button_( combo( { QStringLiteral( "Right button" ), QStringLiteral( "Middle button" ) } ) ),
		furigana_( new QCheckBox( QStringLiteral( "Furigana above the headword" ) ) ),
		reading_( new QCheckBox( QStringLiteral( "The reading next to the headword" ) ) ),
		inflection_( new QCheckBox( QStringLiteral( "How a conjugated word was formed" ) ) ),
		tags_( new QCheckBox( QStringLiteral( "Word classes and tags" ) ) ),
		frequencies_( new QCheckBox( QStringLiteral( "Frequency ranks" ) ) ),
		pitch_( new QCheckBox( QStringLiteral( "Pitch accent" ) ) ),
		dictionary_( new QCheckBox( QStringLiteral( "Dictionary names" ) ) ),
		buttons_( new QCheckBox( QStringLiteral( "Audio and Anki buttons" ) ) ),
		button_size_( spin( 0, 96, QStringLiteral( " px" ), QStringLiteral( "Automatic" ) ) ),
		kanji_( new QCheckBox( QStringLiteral( "Kanji entries when no word matches" ) ) ),
		max_senses_( spin( 0, 50, QString(), QStringLiteral( "All" ) ) ),
		results_( spin( 1, 200 ) ),
		dictionaries_( spin( 0, 64, QString(), QStringLiteral( "All" ) ) ),
		highlight_presets_( new QToolButton() ),
		highlight_style_( combo( { QStringLiteral( "Underline" ), QStringLiteral( "Outline" ), QStringLiteral( "Highlighter (filled box)" ), QStringLiteral( "Double underline" ), QStringLiteral( "Dotted underline" ), QStringLiteral( "Wavy underline" ), QStringLiteral( "Corner brackets" ) } ) ),
		highlight_( new QPushButton() ),
		highlight_auto_( new QCheckBox( QStringLiteral( "Choose the colour for the background automatically" ) ) ),
		highlight_thickness_( spin( 1, 12, QStringLiteral( " px" ) ) ),
		highlight_radius_( spin( 0, 32, QStringLiteral( " px" ), QStringLiteral( "Square" ) ) ),
		highlight_padding_x_( spin( -16, 16, QStringLiteral( " px" ) ) ),
		highlight_padding_y_( spin( -16, 16, QStringLiteral( " px" ) ) ),
		compositor_( combo( { QStringLiteral( "Automatic (follow the compositor)" ), QStringLiteral( "Compositor: translucent" ), QStringLiteral( "No compositor: highlighter" ) } ) ),
		preview_text_( new QLineEdit() ),
		highlight_preview_( new QLabel() ),
		preview_( new QLabel() ),
		preview_scroll_( new QScrollArea() ),
		preview_timer_( new QTimer( this ) )
	{
		colours_ = { { { .button = new QPushButton(), .reset = new QToolButton(), .field = &config::PopupSettings::background_color, .name = QStringLiteral( "Background" ) },
			           { .button = new QPushButton(), .reset = new QToolButton(), .field = &config::PopupSettings::text_color, .name = QStringLiteral( "Text" ) },
			           { .button = new QPushButton(), .reset = new QToolButton(), .field = &config::PopupSettings::accent_color, .name = QStringLiteral( "Accent (furigana, numbers)" ) },
			           { .button = new QPushButton(), .reset = new QToolButton(), .field = &config::PopupSettings::border_color, .name = QStringLiteral( "Border" ) } } };

		scale_->setRange( 0.0, 4.0 );
		scale_->setSingleStep( 0.25 );
		scale_->setDecimals( 2 );
		scale_->setSpecialValueText( QStringLiteral( "Automatic" ) );
		scale_->setSuffix( QStringLiteral( "x" ) );
		// Font combo boxes size themselves to the longest installed font name otherwise.
		font_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
		font_->setMinimumContentsLength( 14 );

		auto* settings_column = new QWidget();
		auto* column          = new QVBoxLayout( settings_column );
		column->setContentsMargins( 24, 20, 12, 20 );
		column->setSpacing( 14 );

		// Theme: the whole look at once, or its parts.
		themes_->setText( QStringLiteral( "Themes" ) );
		themes_->setPopupMode( QToolButton::InstantPopup );
		themes_->setToolButtonStyle( Qt::ToolButtonTextOnly );
		auto* theme_menu = new QMenu( themes_ );
		themes_->setMenu( theme_menu );
		connect( theme_menu, &QMenu::aboutToShow, this, [this, theme_menu] { fillThemeMenu( theme_menu ); } );
		auto* reset_look = new QPushButton( QStringLiteral( "Reset to defaults..." ) );
		reset_look->setToolTip( QStringLiteral( "The friendly design, the default colours, window and highlight. Themes you saved are kept." ) );
		connect( reset_look, &QPushButton::clicked, this, [this] {
			if ( !confirm( this, QStringLiteral( "Reset the appearance" ), QStringLiteral( "The popup's look and the highlight go back to their defaults. Themes you saved are kept." ), QStringLiteral( "Reset" ) ) )
			{
				return;
			}
			auto config  = settings().config();
			config.popup = config::PopupSettings{};
			settings().replace( std::move( config ) );
		} );
		auto* theme_row = new QHBoxLayout();
		theme_row->addWidget( themes_ );
		theme_row->addWidget( save_theme_ );
		theme_row->addStretch( 1 );
		theme_row->addWidget( reset_look );
		auto* look = new QFormLayout();
		look->addRow( theme_row );
		design_->setItemData( 0, QStringLiteral( "Large furigana above the kanji, plain words instead of codes, numbered senses" ), Qt::ToolTipRole );
		design_->setItemData( 1, QStringLiteral( "Yomitan's look: coloured tags, small furigana" ), Qt::ToolTipRole );
		design_->setItemData( 2, QStringLiteral( "The friendly layout, closer together" ), Qt::ToolTipRole );
		look->addRow( QStringLiteral( "Design" ), design_ );
		look->addRow( QStringLiteral( "Colour scheme" ), scheme_ );
		look->addRow( QStringLiteral( "Dark or light" ), theme_ );
		for ( auto& colour : colours_ )
		{
			colour.reset->setText( QStringLiteral( "✕" ) );
			colour.reset->setToolTip( QStringLiteral( "Use the scheme's colour" ) );
			auto* row = new QHBoxLayout();
			row->addWidget( colour.button, 1 );
			row->addWidget( colour.reset );
			look->addRow( colour.name, row );
			connect( colour.button, &QPushButton::clicked, this, [this, field = colour.field, name = colour.name] {
				auto&        popup   = settings().config().popup;
				const QColor current = ( popup.*field ).empty() ? QColor( 128, 128, 128 ) : parseColor( popup.*field );
				const QColor chosen  = QColorDialog::getColor( current, this, name );
				if ( chosen.isValid() )
				{
					popup.*field = ss( chosen.name( QColor::HexRgb ) );
					updateColorButtons();
					settings().commit();
					preview_timer_->start();
				}
			} );
			connect( colour.reset, &QToolButton::clicked, this, [this, field = colour.field] {
				( settings().config().popup.*field ).clear();
				updateColorButtons();
				settings().commit();
				preview_timer_->start();
			} );
		}
		look->addRow( note( QStringLiteral( "Themes set all of this at once. Save your own look as a theme to keep it, share it or edit the file by hand (Themes -> Open the themes folder)." ) ) );
		column->addWidget( group( QStringLiteral( "Theme" ), look ) );

		auto* text = new QFormLayout();
		text->addRow( QStringLiteral( "Font" ), default_font_ );
		text->addRow( QString(), font_ );
		text->addRow( QStringLiteral( "Font size" ), font_size_ );
		headword_size_->setToolTip( QStringLiteral( "Automatic follows the design: large in the friendly one." ) );
		text->addRow( QStringLiteral( "Headword size" ), headword_size_ );
		furigana_size_->setToolTip( QStringLiteral( "The readings above the kanji of the headword. Automatic follows the design." ) );
		text->addRow( QStringLiteral( "Furigana size" ), furigana_size_ );
		text->addRow( QStringLiteral( "Scale" ), scale_ );
		column->addWidget( group( QStringLiteral( "Text" ), text ) );

		auto* window = new QFormLayout();
		window->addRow( QStringLiteral( "Width" ), width_ );
		window->addRow( QStringLiteral( "Maximum height" ), height_ );
		window->addRow( QStringLiteral( "Corner rounding" ), corner_radius_ );
		window->addRow( QStringLiteral( "Border" ), border_width_ );
		window->addRow( QStringLiteral( "Inner margin" ), padding_ );
		opacity_->setToolTip( QStringLiteral( "How much of what is behind the popup shows through its background (needs a compositor)." ) );
		window->addRow( QStringLiteral( "Opacity" ), opacity_ );
		window->addRow( QStringLiteral( "Opens" ), placement_ );
		window->addRow( QStringLiteral( "Horizontal offset" ), offset_x_ );
		window->addRow( QStringLiteral( "Vertical offset" ), offset_y_ );
		select_button_->setToolTip( QStringLiteral( "Drag with this button over the popup to select text; releasing copies it. The left button copies an entry's word." ) );
		window->addRow( QStringLiteral( "Select text with" ), select_button_ );
		window->addRow( note( QStringLiteral( "Rounded corners and opacity need a compositor; without one the popup is square and solid." ) ) );
		column->addWidget( group( QStringLiteral( "Window" ), window ) );

		auto* shown = new QFormLayout();
		for ( QCheckBox* box : { furigana_, reading_, inflection_, tags_, frequencies_, pitch_, dictionary_, buttons_, kanji_ } )
		{
			shown->addRow( box );
		}
		button_size_->setToolTip( QStringLiteral( "The size of the speaker and Anki buttons in the popup; automatic matches the headword. The middle mouse button plays the pronunciation wherever the pointer is over the popup." ) );
		shown->addRow( QStringLiteral( "Button size" ), button_size_ );
		max_senses_->setToolTip( QStringLiteral( "Definitions shown for each dictionary of an entry; the rest are counted (\"+ 3 more\")." ) );
		shown->addRow( QStringLiteral( "Definitions per dictionary" ), max_senses_ );
		shown->addRow( QStringLiteral( "Maximum results" ), results_ );
		dictionaries_->setToolTip( QStringLiteral( "Only the first dictionaries in the priority list that have the word are shown; the next one is used when one has nothing." ) );
		shown->addRow( QStringLiteral( "Dictionaries shown" ), dictionaries_ );
		column->addWidget( group( QStringLiteral( "Show" ), shown ) );

		highlight_presets_->setText( QStringLiteral( "Ready-made highlights" ) );
		highlight_presets_->setPopupMode( QToolButton::InstantPopup );
		highlight_presets_->setToolButtonStyle( Qt::ToolButtonTextOnly );
		auto* highlight_menu = new QMenu( highlight_presets_ );
		highlight_presets_->setMenu( highlight_menu );
		connect( highlight_menu, &QMenu::aboutToShow, this, [this, highlight_menu] { fillHighlightMenu( highlight_menu ); } );
		auto* highlight = new QFormLayout();
		highlight->addRow( highlight_presets_ );
		highlight->addRow( QStringLiteral( "Style" ), highlight_style_ );
		highlight->addRow( QStringLiteral( "Colour" ), highlight_ );
		highlight->addRow( highlight_auto_ );
		highlight->addRow( QStringLiteral( "Line thickness" ), highlight_thickness_ );
		highlight->addRow( QStringLiteral( "Corner rounding" ), highlight_radius_ );
		// Applications report the box of a word differently; below zero the mark is pulled in over one that is too generous.
		const QString room = QStringLiteral( "How far the mark reaches past the text. Below zero it is pulled in, for applications that report a box larger than the word looks." );
		highlight_padding_x_->setToolTip( room );
		highlight_padding_y_->setToolTip( room );
		highlight->addRow( QStringLiteral( "Room left and right" ), highlight_padding_x_ );
		highlight->addRow( QStringLiteral( "Room above and below" ), highlight_padding_y_ );
		compositor_->setToolTip( QStringLiteral( "With a compositor the highlight is translucent and the popup has rounded corners; without one the highlight marks the background around the glyphs." ) );
		highlight->addRow( QStringLiteral( "Transparency" ), compositor_ );
		highlight->addRow( note( QStringLiteral( "Lines use the colour as it is; a highlighter tints the text with it, its opacity being the strength (also when the colour is chosen automatically)." ) ) );
		column->addWidget( group( QStringLiteral( "Highlight" ), highlight ) );
		column->addStretch( 1 );

		auto* scroll = new QScrollArea();
		scroll->setWidget( settings_column );
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		scroll->setMinimumWidth( 400 );
		scroll->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );

		auto* preview_column = new QVBoxLayout();
		preview_column->setContentsMargins( 12, 20, 12, 20 );
		preview_column->addWidget( new QLabel( QStringLiteral( "<b>Live preview</b>" ) ) );
		preview_text_->setPlaceholderText( QStringLiteral( "Preview word (empty: one from your dictionaries)" ) );
		preview_column->addWidget( preview_text_ );
		highlight_preview_->setAlignment( Qt::AlignLeft | Qt::AlignTop );
		highlight_preview_->setToolTip( QStringLiteral( "The highlight over text on a light and on a dark background" ) );
		preview_column->addWidget( highlight_preview_ );
		preview_->setAlignment( Qt::AlignLeft | Qt::AlignTop );
		preview_->setMinimumSize( 300, 200 );
		preview_scroll_->setWidget( preview_ );
		preview_scroll_->setWidgetResizable( true );
		preview_scroll_->setFrameShape( QFrame::NoFrame );
		preview_scroll_->setMinimumWidth( 440 );
		preview_column->addWidget( preview_scroll_, 1 );

		auto* outer = new QHBoxLayout( this );
		outer->setContentsMargins( 0, 0, 0, 0 );
		outer->addWidget( scroll, 1 );
		outer->addLayout( preview_column, 0 );

		preview_timer_->setSingleShot( true );
		preview_timer_->setInterval( 150 );
		connect( preview_timer_, &QTimer::timeout, this, [this] { updatePreview(); } );
		connect( preview_text_, &QLineEdit::textChanged, preview_timer_, qOverload<>( &QTimer::start ) );

		const auto hook = [this]( auto* widget, auto signal ) { connect( widget, signal, this, [this] { changed(); } ); };
		for ( QComboBox* box : { design_, scheme_, theme_, placement_, select_button_, highlight_style_, compositor_ } )
		{
			hook( box, &QComboBox::currentIndexChanged );
		}
		for ( QCheckBox* box : { default_font_, furigana_, reading_, inflection_, tags_, frequencies_, pitch_, dictionary_, buttons_, kanji_, highlight_auto_ } )
		{
			hook( box, &QCheckBox::toggled );
		}
		for ( QSpinBox* box : { font_size_, headword_size_, furigana_size_, width_, height_, corner_radius_, border_width_, padding_, opacity_, offset_x_, offset_y_, max_senses_, results_, dictionaries_, button_size_, highlight_thickness_, highlight_radius_, highlight_padding_x_, highlight_padding_y_ } )
		{
			hook( box, &QSpinBox::valueChanged );
		}
		hook( font_, &QFontComboBox::currentFontChanged );
		hook( scale_, &QDoubleSpinBox::valueChanged );

		connect( save_theme_, &QPushButton::clicked, this, [this] { saveTheme(); } );
		connect( highlight_, &QPushButton::clicked, this, [this] {
			const QColor chosen = QColorDialog::getColor( parseColor( settings().config().popup.highlight_color ), this, QStringLiteral( "Highlight colour" ), QColorDialog::ShowAlphaChannel );
			if ( chosen.isValid() )
			{
				settings().config().popup.highlight_color = formatColor( chosen );
				updateColorButtons();
				settings().commit();
				preview_timer_->start();
			}
		} );
	}

	void AppearancePage::activated()
	{
		updatePreview();
	}

	void AppearancePage::updateColorButtons()
	{
		const auto& popup = settings().config().popup;
		highlight_->setIcon( swatch( parseColor( popup.highlight_color ) ) );
		highlight_->setText( qs( popup.highlight_color ) );
		for ( const auto& colour : colours_ )
		{
			const std::string& value = popup.*( colour.field );
			colour.button->setIcon( value.empty() ? QIcon() : swatch( parseColor( value ) ) );
			colour.button->setText( value.empty() ? QStringLiteral( "The scheme's" ) : qs( value ) );
			colour.reset->setEnabled( !value.empty() );
		}
	}

	void AppearancePage::refresh()
	{
		loading_          = true;
		const auto& popup = settings().config().popup;

		std::vector<std::unique_ptr<QSignalBlocker>> blockers;
		for ( QObject* widget : std::initializer_list<QObject*>{ design_, scheme_, theme_, default_font_, font_, font_size_, headword_size_, furigana_size_, scale_, width_, height_, corner_radius_, border_width_, padding_, opacity_, placement_, offset_x_, offset_y_, select_button_, furigana_, reading_, inflection_, tags_, frequencies_, pitch_, dictionary_, buttons_, button_size_, kanji_, max_senses_, results_, dictionaries_, highlight_style_, highlight_auto_, highlight_thickness_, highlight_radius_, highlight_padding_x_, highlight_padding_y_, compositor_ } )
		{
			blockers.push_back( std::make_unique<QSignalBlocker>( widget ) );
		}

		design_->setCurrentIndex( static_cast<int>( popup.design ) );
		scheme_->setCurrentIndex( static_cast<int>( popup.scheme ) );
		theme_->setCurrentIndex( themeIndex( popup.theme ) );
		default_font_->setChecked( popup.font_family.empty() );
		font_->setEnabled( !popup.font_family.empty() );
		if ( !popup.font_family.empty() )
		{
			font_->setCurrentFont( QFont( qs( popup.font_family ) ) );
		}
		font_size_->setValue( popup.font_size );
		headword_size_->setValue( popup.headword_size );
		furigana_size_->setValue( popup.furigana_size );
		scale_->setValue( popup.scale );
		width_->setValue( popup.width );
		height_->setValue( popup.max_height );
		corner_radius_->setValue( popup.corner_radius );
		border_width_->setValue( popup.border_width );
		padding_->setValue( popup.padding );
		opacity_->setValue( popup.opacity );
		placement_->setCurrentIndex( popup.placement == config::PopupPlacement::AboveText ? 1 : 0 );
		offset_x_->setValue( popup.offset_x );
		offset_y_->setValue( popup.offset_y );
		select_button_->setCurrentIndex( popup.select_button == config::MouseButton::Middle ? 1 : 0 );
		furigana_->setChecked( popup.show_furigana );
		reading_->setChecked( popup.show_reading );
		inflection_->setChecked( popup.show_inflection );
		tags_->setChecked( popup.show_tags );
		frequencies_->setChecked( popup.show_frequencies );
		pitch_->setChecked( popup.show_pitch );
		dictionary_->setChecked( popup.show_dictionary );
		buttons_->setChecked( popup.show_buttons );
		button_size_->setValue( popup.button_size );
		button_size_->setEnabled( popup.show_buttons );
		kanji_->setChecked( popup.show_kanji );
		max_senses_->setValue( popup.max_senses );
		results_->setValue( popup.max_results );
		dictionaries_->setValue( popup.max_dictionaries );
		highlight_style_->setCurrentIndex( static_cast<int>( popup.highlight_style ) );
		highlight_auto_->setChecked( popup.highlight_auto );
		highlight_thickness_->setValue( popup.highlight_thickness );
		highlight_thickness_->setEnabled( popup.highlight_style != config::HighlightStyle::Fill );
		highlight_radius_->setValue( popup.highlight_radius );
		highlight_padding_x_->setValue( popup.highlight_padding_x );
		highlight_padding_y_->setValue( popup.highlight_padding_y );
		compositor_->setCurrentIndex( static_cast<int>( popup.compositor ) );
		updateColorButtons();
		loading_ = false;
		preview_timer_->start();
	}

	void AppearancePage::changed()
	{
		if ( loading_ )
		{
			return;
		}
		auto& popup               = settings().config().popup;
		popup.design              = static_cast<config::PopupDesign>( std::clamp( design_->currentIndex(), 0, 2 ) );
		popup.scheme              = static_cast<config::ColorScheme>( std::clamp( scheme_->currentIndex(), 0, 6 ) );
		popup.theme               = themeAt( theme_->currentIndex() );
		popup.font_family         = default_font_->isChecked() ? std::string() : ss( font_->currentFont().family() );
		popup.font_size           = font_size_->value();
		popup.headword_size       = headword_size_->value();
		popup.furigana_size       = furigana_size_->value();
		popup.scale               = scale_->value();
		popup.width               = width_->value();
		popup.max_height          = height_->value();
		popup.corner_radius       = corner_radius_->value();
		popup.border_width        = border_width_->value();
		popup.padding             = padding_->value();
		popup.opacity             = opacity_->value();
		popup.placement           = placement_->currentIndex() == 1 ? config::PopupPlacement::AboveText : config::PopupPlacement::BelowText;
		popup.offset_x            = offset_x_->value();
		popup.offset_y            = offset_y_->value();
		popup.select_button       = select_button_->currentIndex() == 1 ? config::MouseButton::Middle : config::MouseButton::Right;
		popup.show_furigana       = furigana_->isChecked();
		popup.show_reading        = reading_->isChecked();
		popup.show_inflection     = inflection_->isChecked();
		popup.show_tags           = tags_->isChecked();
		popup.show_frequencies    = frequencies_->isChecked();
		popup.show_pitch          = pitch_->isChecked();
		popup.show_dictionary     = dictionary_->isChecked();
		popup.show_buttons        = buttons_->isChecked();
		popup.button_size         = button_size_->value();
		popup.show_kanji          = kanji_->isChecked();
		popup.max_senses          = max_senses_->value();
		popup.max_results         = results_->value();
		popup.max_dictionaries    = dictionaries_->value();
		popup.highlight_style     = static_cast<config::HighlightStyle>( std::clamp( highlight_style_->currentIndex(), 0, 6 ) );
		popup.highlight_auto      = highlight_auto_->isChecked();
		popup.highlight_thickness = highlight_thickness_->value();
		popup.highlight_radius    = highlight_radius_->value();
		popup.highlight_padding_x = highlight_padding_x_->value();
		popup.highlight_padding_y = highlight_padding_y_->value();
		popup.compositor          = static_cast<config::Compositor>( std::clamp( compositor_->currentIndex(), 0, 2 ) );
		highlight_thickness_->setEnabled( popup.highlight_style != config::HighlightStyle::Fill );
		button_size_->setEnabled( popup.show_buttons );
		font_->setEnabled( !default_font_->isChecked() );
		settings().commit();
		preview_timer_->start();
	}

	void AppearancePage::fillThemeMenu( QMenu* menu )
	{
		menu->clear();
		menu->addSection( QStringLiteral( "Built in" ) );
		for ( const auto& theme : themes::builtIn() )
		{
			auto* action = menu->addAction( theme.name, this, [this, theme] { applyTheme( theme ); } );
			action->setToolTip( theme.description );
		}
		menu->addSection( QStringLiteral( "Yours" ) );
		const auto installed = themes::installed();
		if ( installed.empty() )
		{
			menu->addAction( QStringLiteral( "None yet: save the current look as a theme" ) )->setEnabled( false );
		}
		for ( const auto& theme : installed )
		{
			menu->addAction( theme.name, this, [this, theme] { applyTheme( theme ); } );
		}
		menu->addSeparator();
		menu->addAction( QStringLiteral( "Save the current look as a theme..." ), this, [this] { saveTheme(); } );
		menu->addAction( QStringLiteral( "Import a theme file..." ), this, [this] {
			const QString file = QFileDialog::getOpenFileName( this, QStringLiteral( "Import a theme" ), QDir::homePath(), QStringLiteral( "Lexiglance themes (*.json)" ) );
			if ( file.isEmpty() )
			{
				return;
			}
			QString error;
			if ( const QString path = themes::import( file, &error ); !path.isEmpty() )
			{
				QMessageBox::information( this, QStringLiteral( "Import a theme" ), QStringLiteral( "The theme is now in the Themes menu." ) );
			}
			else
			{
				QMessageBox::warning( this, QStringLiteral( "Import a theme" ), error );
			}
		} );
		menu->addAction( QStringLiteral( "Open the themes folder" ), this, [] {
			QDir().mkpath( themes::directory() );
			QDesktopServices::openUrl( QUrl::fromLocalFile( themes::directory() ) );
		} );
		if ( !installed.empty() )
		{
			auto* remove = menu->addMenu( QStringLiteral( "Delete a theme" ) );
			for ( const auto& theme : installed )
			{
				remove->addAction( theme.name, this, [this, theme] {
					if ( QMessageBox::question( this, QStringLiteral( "Delete a theme" ), QStringLiteral( "Delete the theme \"%1\"?" ).arg( theme.name ) ) == QMessageBox::Yes )
					{
						QFile::remove( theme.path );
					}
				} );
			}
		}
	}

	void AppearancePage::fillHighlightMenu( QMenu* menu )
	{
		menu->clear();
		for ( const auto& preset : themes::highlights() )
		{
			auto* action = menu->addAction( preset.name, this, [this, preset] { applyTheme( preset ); } );
			action->setToolTip( preset.description );
		}
	}

	void AppearancePage::applyTheme( const themes::Theme& theme )
	{
		QString error;
		if ( !themes::apply( theme, settings().config().popup, &error ) )
		{
			QMessageBox::warning( this, QStringLiteral( "Theme" ), QStringLiteral( "\"%1\" could not be used: %2" ).arg( theme.name, error ) );
			return;
		}
		settings().commit();
		refresh();
	}

	void AppearancePage::saveTheme()
	{
		bool          ok   = false;
		const QString name = QInputDialog::getText( this, QStringLiteral( "Save as theme" ), QStringLiteral( "Name of the theme:" ), QLineEdit::Normal, QStringLiteral( "My theme" ), &ok );
		if ( !ok || name.trimmed().isEmpty() )
		{
			return;
		}
		QString       error;
		const QString path = themes::save( name, settings().config().popup, &error );
		if ( path.isEmpty() )
		{
			QMessageBox::warning( this, QStringLiteral( "Save as theme" ), error );
			return;
		}
		QMessageBox::information( this, QStringLiteral( "Save as theme" ), QStringLiteral( "Saved as %1.\nIt is in the Themes menu; the file can be shared or edited by hand." ).arg( QDir::toNativeSeparators( path ) ) );
	}

	void AppearancePage::updatePreview()
	{
		if ( !isVisible() )
		{
			return;
		}
		// The whole popup is drawn so the preview uses the column to the bottom; a line marks where the real one ends.
		auto      config        = settings().config();
		const int cut           = config.popup.max_height;
		config.popup.max_height = 2000;
		json::Writer params;
		params.beginObject().field( "text", ss( preview_text_->text() ) ).key( "config" ).raw( config.toJson( false ) ).endObject();
		const std::string request = params.take();
		client().call( "popup.preview", request, [this, cut]( const json::Value* result, const QString& error ) {
			if ( result == nullptr )
			{
				preview_->setPixmap( {} );
				preview_->setText( error );
				return;
			}
			QPixmap      image = decode( *result );
			const QSizeF size  = image.deviceIndependentSize();
			if ( size.height() > cut + 1 )
			{
				QPainter painter( &image );
				painter.setRenderHint( QPainter::Antialiasing );
				painter.setPen( QPen( QColor( 53, 120, 210 ), 1.5, Qt::DashLine ) );
				painter.drawLine( QPointF( 0, cut ), QPointF( size.width(), cut ) );
				QFont font = painter.font();
				font.setPointSizeF( font.pointSizeF() * 0.85 );
				painter.setFont( font );
				painter.drawText( QRectF( 0, cut + 2, size.width() - 8, 20 ), Qt::AlignRight | Qt::AlignTop, QStringLiteral( "the popup ends here (maximum height)" ) );
			}
			preview_->setPixmap( image );
			// The column is as wide as the popup and its scroll bar, so nothing is left over beside it, and the word to
			// preview ends where the previews do.
			const int width = static_cast<int>( image.deviceIndependentSize().width() );
			preview_scroll_->setFixedWidth( width + preview_scroll_->verticalScrollBar()->sizeHint().width() );
			preview_text_->setMaximumWidth( width );
		} );
		client().call( "highlight.preview", request, [this]( const json::Value* result, const QString& ) {
			highlight_preview_->setPixmap( result != nullptr ? decode( *result ) : QPixmap() );
		} );
	}

} // namespace lexiglance::gui
