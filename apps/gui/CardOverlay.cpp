#include "CardOverlay.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace lexiglance::gui
{

	CardOverlay::CardOverlay( int width, QWidget* parent ) :
		QWidget( parent ),
		card_( new QFrame( this ) ),
		width_( width )
	{
		hide();
		setFocusPolicy( Qt::StrongFocus );
		card_->setObjectName( QStringLiteral( "helpCard" ) );
		parent->installEventFilter( this );
	}

	void CardOverlay::popUp()
	{
		place();
		show();
		raise();
	}

	void CardOverlay::place()
	{
		setGeometry( parentWidget()->rect() );
		card_->ensurePolished();
		const int width  = std::min( width_, this->width() - 48 );
		const int wanted = card_->heightForWidth( width );
		const int height = std::min( wanted > 0 ? wanted : card_->sizeHint().height(), this->height() - 48 );
		card_->setGeometry( ( this->width() - width ) / 2, ( this->height() - height ) / 2, width, height );
	}

	bool CardOverlay::eventFilter( QObject* watched, QEvent* event )
	{
		if ( watched == parentWidget() && event->type() == QEvent::Resize && isVisible() )
		{
			place();
		}
		return QWidget::eventFilter( watched, event );
	}

	void CardOverlay::paintEvent( QPaintEvent* /*event*/ )
	{
		QPainter painter( this );
		painter.fillRect( rect(), QColor( 0, 0, 0, 110 ) );
	}

	void CardOverlay::mousePressEvent( QMouseEvent* event )
	{
		// Clicks on the card come here too, as it takes none itself.
		if ( dismissible_ && !card_->geometry().contains( event->position().toPoint() ) )
		{
			hide();
		}
	}

	void CardOverlay::keyPressEvent( QKeyEvent* event )
	{
		if ( event->key() == Qt::Key_Escape )
		{
			if ( dismissible_ )
			{
				hide();
			}
			return;
		}
		QWidget::keyPressEvent( event );
	}

} // namespace lexiglance::gui
