#include "StatisticsCharts.h"

#include <QHelpEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace lexiglance::gui
{

	namespace
	{

		// A round step for the value axis: 1, 2 or 5 times a power of ten, so the lines fall on plain numbers.
		std::int64_t roundStep( double rough )
		{
			if ( rough <= 1.0 )
			{
				return 1;
			}
			const double magnitude = std::pow( 10.0, std::floor( std::log10( rough ) ) );
			for ( const double multiple : { 1.0, 2.0, 5.0, 10.0 } )
			{
				if ( multiple * magnitude >= rough )
				{
					return static_cast<std::int64_t>( multiple * magnitude );
				}
			}
			return static_cast<std::int64_t>( 10.0 * magnitude );
		}

		// Short enough for an axis: 950, 12k, 1.2M.
		QString compact( std::int64_t value )
		{
			if ( value >= 1'000'000 )
			{
				return QLocale().toString( static_cast<double>( value ) / 1'000'000.0, 'f', value >= 10'000'000 ? 0 : 1 ) + QStringLiteral( "M" );
			}
			if ( value >= 10'000 )
			{
				return QLocale().toString( static_cast<double>( value ) / 1'000.0, 'f', 0 ) + QStringLiteral( "k" );
			}
			return QLocale().toString( static_cast<qlonglong>( value ) );
		}

		QString dayTitle( const Period& period )
		{
			const QLocale locale;
			if ( period.first == period.last )
			{
				return locale.toString( period.first, QStringLiteral( "dddd d MMMM yyyy" ) );
			}
			return QStringLiteral( "%1 – %2" ).arg( locale.toString( period.first, QStringLiteral( "d MMM" ) ), locale.toString( period.last, QStringLiteral( "d MMM yyyy" ) ) );
		}

		QColor withAlpha( QColor colour, int alpha )
		{
			colour.setAlpha( alpha );
			return colour;
		}

	} // namespace

	QString figureName( Figure figure )
	{
		switch ( figure )
		{
			case Figure::Lookups:
				return QStringLiteral( "Lookups" );
			case Figure::Found:
				return QStringLiteral( "Words found" );
			case Figure::Characters:
				return QStringLiteral( "Characters read" );
			case Figure::Popups:
				return QStringLiteral( "Popups shown" );
			case Figure::Audio:
				return QStringLiteral( "Pronunciations played" );
			case Figure::Anki:
				return QStringLiteral( "Anki notes added" );
			case Figure::Translations:
				return QStringLiteral( "Translations" );
			case Figure::TranslatedCharacters:
				return QStringLiteral( "Characters translated" );
		}
		return {};
	}

	const char* figureKey( Figure figure )
	{
		switch ( figure )
		{
			case Figure::Lookups:
				return "lookups";
			case Figure::Found:
				return "found";
			case Figure::Characters:
				return "characters";
			case Figure::Popups:
				return "popups";
			case Figure::Audio:
				return "audio";
			case Figure::Anki:
				return "anki";
			case Figure::Translations:
				return "translations";
			case Figure::TranslatedCharacters:
				return "translated_characters";
		}
		return "";
	}

	// --- ActivityChart ---

	ActivityChart::ActivityChart( QWidget* parent ) :
		QWidget( parent )
	{
		setMouseTracking( true );
		setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );
	}

	void ActivityChart::setPeriods( std::vector<Period> periods, Figure figure, Style style )
	{
		periods_ = std::move( periods );
		figure_  = figure;
		style_   = style;
		hover_   = std::min( hover_, static_cast<int>( periods_.size() ) - 1 );
		update();
	}

	QSize ActivityChart::sizeHint() const
	{
		return { 600, 240 };
	}

	QSize ActivityChart::minimumSizeHint() const
	{
		return { 240, 200 };
	}

	QRectF ActivityChart::plot() const
	{
		const QFontMetrics metrics( font() );
		const double       left = metrics.horizontalAdvance( QStringLiteral( "0000k" ) ) + 10.0;
		return { left, 10.0, std::max( 10.0, width() - left - 8.0 ), std::max( 10.0, height() - 10.0 - ( metrics.height() + 10.0 ) ) };
	}

	int ActivityChart::periodAt( double x ) const
	{
		const QRectF area = plot();
		if ( periods_.empty() || x < area.left() || x >= area.right() )
		{
			return -1;
		}
		const double slot = area.width() / static_cast<double>( periods_.size() );
		return std::clamp( static_cast<int>( ( x - area.left() ) / slot ), 0, static_cast<int>( periods_.size() ) - 1 );
	}

	void ActivityChart::mouseMoveEvent( QMouseEvent* event )
	{
		const int at = periodAt( event->position().x() );
		if ( at != hover_ )
		{
			hover_ = at;
			update();
		}
		QWidget::mouseMoveEvent( event );
	}

	void ActivityChart::leaveEvent( QEvent* event )
	{
		hover_ = -1;
		update();
		QWidget::leaveEvent( event );
	}

	void ActivityChart::paintEvent( QPaintEvent* /*event*/ )
	{
		QPainter painter( this );
		painter.setRenderHint( QPainter::Antialiasing );
		const QRectF       area   = plot();
		const QColor       text   = palette().color( QPalette::Text );
		const QColor       muted  = palette().color( QPalette::PlaceholderText );
		const QColor       accent = palette().color( QPalette::Highlight );
		const QFontMetrics metrics( font() );

		std::int64_t largest = 0;
		for ( const Period& period : periods_ )
		{
			largest = std::max( largest, period[figure_] );
		}
		if ( periods_.empty() || largest == 0 )
		{
			painter.setPen( muted );
			painter.drawText( rect(), Qt::AlignCenter, QStringLiteral( "Nothing counted in this time yet" ) );
			return;
		}

		// The value axis: a few lines at round numbers, from nothing to at least the largest.
		const std::int64_t step = roundStep( static_cast<double>( largest ) / 4.0 );
		const std::int64_t top  = step * ( ( largest + step - 1 ) / step );
		const auto         y    = [&]( double value ) { return area.bottom() - ( value / static_cast<double>( top ) ) * area.height(); };
		for ( std::int64_t value = 0; value <= top; value += step )
		{
			const double line = y( static_cast<double>( value ) );
			painter.setPen( QPen( withAlpha( muted, value == 0 ? 140 : 50 ), 1.0 ) );
			painter.drawLine( QPointF( area.left(), line ), QPointF( area.right(), line ) );
			painter.setPen( muted );
			painter.drawText( QRectF( 0, line - metrics.height() / 2.0, area.left() - 8.0, metrics.height() ), Qt::AlignRight | Qt::AlignVCenter, compact( value ) );
		}

		const double slot = area.width() / static_cast<double>( periods_.size() );
		if ( hover_ >= 0 )
		{
			painter.fillRect( QRectF( area.left() + slot * hover_, area.top(), slot, area.height() ), withAlpha( text, 18 ) );
		}
		if ( style_ == Style::Bars )
		{
			const double bar = std::max( 1.0, std::min( slot * 0.72, slot - 1.0 ) );
			for ( std::size_t i = 0; i < periods_.size(); ++i )
			{
				const auto value = static_cast<double>( periods_[i][figure_] );
				if ( value <= 0 )
				{
					continue;
				}
				const QRectF shape( area.left() + slot * static_cast<double>( i ) + ( slot - bar ) / 2.0, y( value ), bar, area.bottom() - y( value ) );
				QPainterPath path;
				const double radius = std::min( 4.0, bar / 3.0 );
				path.addRoundedRect( shape.adjusted( 0, 0, 0, radius ), radius, radius );
				painter.save();
				painter.setClipRect( QRectF( area.left(), area.top(), area.width(), area.height() ) );
				painter.fillPath( path, static_cast<int>( i ) == hover_ ? accent.lighter( 125 ) : accent );
				painter.restore();
			}
		}
		else
		{
			QPainterPath line;
			QPainterPath fill;
			for ( std::size_t i = 0; i < periods_.size(); ++i )
			{
				const QPointF point( area.left() + slot * ( static_cast<double>( i ) + 0.5 ), y( static_cast<double>( periods_[i][figure_] ) ) );
				if ( i == 0 )
				{
					line.moveTo( point );
					fill.moveTo( point.x(), area.bottom() );
				}
				else
				{
					line.lineTo( point );
				}
				fill.lineTo( point );
				if ( i + 1 == periods_.size() )
				{
					fill.lineTo( point.x(), area.bottom() );
				}
			}
			fill.closeSubpath();
			painter.fillPath( fill, withAlpha( accent, 45 ) );
			painter.setPen( QPen( accent, 2.0 ) );
			painter.drawPath( line );
			if ( periods_.size() <= 45 || hover_ >= 0 )
			{
				painter.setBrush( accent );
				painter.setPen( QPen( palette().color( QPalette::Base ), 1.5 ) );
				for ( std::size_t i = 0; i < periods_.size(); ++i )
				{
					const bool hovered = static_cast<int>( i ) == hover_;
					if ( periods_.size() > 45 && !hovered )
					{
						continue;
					}
					const double r = hovered ? 5.0 : 3.0;
					painter.drawEllipse( QPointF( area.left() + slot * ( static_cast<double>( i ) + 0.5 ), y( static_cast<double>( periods_[i][figure_] ) ) ), r, r );
				}
			}
		}

		// Dates under the axis, as many as fit side by side, the last one always.
		painter.setPen( muted );
		const QLocale locale;
		const double  label_width = metrics.horizontalAdvance( QStringLiteral( "30 Sep" ) ) + 16.0;
		const int     every       = std::max( 1, static_cast<int>( std::ceil( label_width / slot ) ) );
		for ( int i = static_cast<int>( periods_.size() ) - 1; i >= 0; i -= every )
		{
			const double centre = area.left() + slot * ( i + 0.5 );
			const QRectF box( centre - label_width / 2.0, area.bottom() + 4.0, label_width, metrics.height() + 4.0 );
			painter.drawText( box, Qt::AlignHCenter | Qt::AlignTop, locale.toString( periods_[static_cast<std::size_t>( i )].first, QStringLiteral( "d MMM" ) ) );
		}

		if ( hover_ >= 0 )
		{
			paintCard( painter, periods_[static_cast<std::size_t>( hover_ )], area.left() + slot * ( hover_ + 0.5 ) );
		}
	}

	void ActivityChart::paintCard( QPainter& painter, const Period& period, double x ) const
	{
		QFont bold = font();
		bold.setBold( true );
		const QFontMetrics metrics( font() );
		const QFontMetrics bold_metrics( bold );
		const QString      title = dayTitle( period );

		double name_width  = 0;
		double value_width = 0;
		for ( std::size_t i = 0; i < figure_count; ++i )
		{
			name_width  = std::max( name_width, static_cast<double>( bold_metrics.horizontalAdvance( figureName( static_cast<Figure>( i ) ) ) ) );
			value_width = std::max( value_width, static_cast<double>( bold_metrics.horizontalAdvance( QLocale().toString( static_cast<qlonglong>( period.values[i] ) ) ) ) );
		}
		const double line    = metrics.height() + 3.0;
		const double padding = 10.0;
		const double width   = std::max( static_cast<double>( bold_metrics.horizontalAdvance( title ) ), name_width + 24.0 + value_width ) + padding * 2.0;
		const double height  = padding * 2.0 + line * ( figure_count + 1 ) + 4.0;
		// Beside the day, on whichever side has room, and within the chart.
		double left = x + 14.0;
		if ( left + width > this->width() - 2.0 )
		{
			left = x - 14.0 - width;
		}
		left = std::clamp( left, 2.0, std::max( 2.0, this->width() - width - 2.0 ) );
		const QRectF card( left, std::max( 2.0, ( this->height() - height ) / 2.0 ), width, height );

		painter.save();
		painter.setPen( QPen( palette().color( QPalette::Mid ), 1.0 ) );
		painter.setBrush( palette().color( QPalette::Base ) );
		painter.drawRoundedRect( card, 8.0, 8.0 );
		painter.setFont( bold );
		painter.setPen( palette().color( QPalette::Text ) );
		double top = card.top() + padding;
		painter.drawText( QRectF( card.left() + padding, top, card.width() - padding * 2.0, line ), Qt::AlignLeft | Qt::AlignVCenter, title );
		top += line + 4.0;
		for ( std::size_t i = 0; i < figure_count; ++i )
		{
			const auto   figure = static_cast<Figure>( i );
			const bool   chosen = figure == figure_;
			const QRectF row( card.left() + padding, top, card.width() - padding * 2.0, line );
			painter.setFont( chosen ? bold : font() );
			painter.setPen( chosen ? palette().color( QPalette::Highlight ).lighter( 130 ) : palette().color( QPalette::Text ) );
			painter.drawText( row, Qt::AlignLeft | Qt::AlignVCenter, figureName( figure ) );
			painter.drawText( row, Qt::AlignRight | Qt::AlignVCenter, QLocale().toString( static_cast<qlonglong>( period.values[i] ) ) );
			top += line;
		}
		painter.restore();
	}

	// --- BarList ---

	BarList::BarList( QWidget* parent ) :
		QWidget( parent )
	{
		setMouseTracking( true );
		setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
	}

	void BarList::setRows( std::vector<Row> rows, const QColor& colour, const QString& empty )
	{
		rows_   = std::move( rows );
		colour_ = colour;
		empty_  = empty;
		hover_  = -1;
		updateGeometry();
		setFixedHeight( sizeHint().height() );
		update();
	}

	int BarList::rowHeight() const
	{
		return QFontMetrics( font() ).height() + 10;
	}

	QSize BarList::sizeHint() const
	{
		return { 300, rowHeight() * std::max<int>( 1, static_cast<int>( rows_.size() ) ) };
	}

	int BarList::rowAt( int y ) const
	{
		const int row = y / rowHeight();
		return row >= 0 && row < static_cast<int>( rows_.size() ) ? row : -1;
	}

	void BarList::mouseMoveEvent( QMouseEvent* event )
	{
		const int at = rowAt( static_cast<int>( event->position().y() ) );
		if ( at != hover_ )
		{
			hover_ = at;
			update();
		}
		QWidget::mouseMoveEvent( event );
	}

	void BarList::leaveEvent( QEvent* event )
	{
		hover_ = -1;
		update();
		QWidget::leaveEvent( event );
	}

	bool BarList::event( QEvent* event )
	{
		if ( event->type() == QEvent::ToolTip )
		{
			const auto* help = dynamic_cast<QHelpEvent*>( event );
			const int   at   = help != nullptr ? rowAt( help->pos().y() ) : -1;
			if ( at < 0 )
			{
				QToolTip::hideText();
				event->ignore();
				return true;
			}
			std::int64_t all = 0;
			for ( const Row& row : rows_ )
			{
				all += row.count;
			}
			const Row& row = rows_[static_cast<std::size_t>( at )];
			QToolTip::showText( help->globalPos(), QStringLiteral( "%1: %2 (%3%)" ).arg( row.label, QLocale().toString( static_cast<qlonglong>( row.count ) ) ).arg( all > 0 ? ( static_cast<double>( row.count ) * 100.0 ) / static_cast<double>( all ) : 0.0, 0, 'f', 1 ), this );
			return true;
		}
		return QWidget::event( event );
	}

	void BarList::paintEvent( QPaintEvent* /*event*/ )
	{
		QPainter painter( this );
		painter.setRenderHint( QPainter::Antialiasing );
		const QColor text  = palette().color( QPalette::Text );
		const QColor muted = palette().color( QPalette::PlaceholderText );
		if ( rows_.empty() )
		{
			painter.setPen( muted );
			painter.drawText( rect().adjusted( 4, 0, 0, 0 ), Qt::AlignLeft | Qt::AlignVCenter, empty_ );
			return;
		}
		const QFontMetrics metrics( font() );
		std::int64_t       largest = 1;
		int                counted = 0;
		for ( const Row& row : rows_ )
		{
			largest = std::max( largest, row.count );
			counted = std::max( counted, metrics.horizontalAdvance( QLocale().toString( static_cast<qlonglong>( row.count ) ) ) );
		}
		const int    height = rowHeight();
		const double label  = std::min( width() * 0.42, 260.0 );
		const double track  = std::max( 20.0, width() - label - counted - 24.0 );
		for ( std::size_t i = 0; i < rows_.size(); ++i )
		{
			const Row&   row = rows_[i];
			const QRectF line( 0, static_cast<double>( height ) * static_cast<double>( i ), width(), height );
			if ( static_cast<int>( i ) == hover_ )
			{
				painter.fillRect( line, QColor( text.red(), text.green(), text.blue(), 18 ) );
			}
			painter.setPen( text );
			painter.drawText( QRectF( line.left() + 4.0, line.top(), label - 12.0, line.height() ), Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText( row.label, Qt::ElideRight, static_cast<int>( label ) - 12 ) );
			const QRectF bar( label, line.center().y() - 5.0, std::max( 3.0, track * static_cast<double>( row.count ) / static_cast<double>( largest ) ), 10.0 );
			painter.setPen( Qt::NoPen );
			painter.setBrush( QColor( colour_.red(), colour_.green(), colour_.blue(), 40 ) );
			painter.drawRoundedRect( QRectF( label, bar.top(), track, bar.height() ), 5.0, 5.0 );
			painter.setBrush( static_cast<int>( i ) == hover_ ? colour_.lighter( 120 ) : colour_ );
			painter.drawRoundedRect( bar, 5.0, 5.0 );
			painter.setPen( muted );
			painter.drawText( QRectF( label + track + 8.0, line.top(), counted + 8.0, line.height() ), Qt::AlignRight | Qt::AlignVCenter, QLocale().toString( static_cast<qlonglong>( row.count ) ) );
		}
	}

} // namespace lexiglance::gui
