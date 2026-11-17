#include "CardOverlay.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>

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
		if ( QWidget* focused = QApplication::focusWidget(); focused != nullptr && !isAncestorOf( focused ) )
		{
			focus_before_ = focused;
		}
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

	QVBoxLayout* CardOverlay::cardLayout()
	{
		auto* layout = new QVBoxLayout( card_ );
		layout->setContentsMargins( 28, 22, 28, 20 );
		layout->setSpacing( 12 );
		return layout;
	}

	QLabel* CardOverlay::cardTitle( const QString& text )
	{
		auto* title = new QLabel( text );
		QFont font  = title->font();
		font.setPointSizeF( font.pointSizeF() * 1.4 );
		font.setBold( true );
		title->setFont( font );
		title->setWordWrap( true );
		return title;
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
		// Buttons outside a dialog take only Space.
		if ( event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter )
		{
			auto* button = qobject_cast<QPushButton*>( focusWidget() );
			if ( button == nullptr || !isAncestorOf( button ) )
			{
				const auto buttons = card_->findChildren<QPushButton*>();
				const auto primary = std::ranges::find_if( buttons, [this]( QPushButton* candidate ) { return candidate->property( "primary" ).toBool() && candidate->isEnabled() && candidate->isVisibleTo( card_ ); } );
				button             = primary != buttons.end() ? *primary : nullptr;
			}
			if ( button != nullptr )
			{
				button->click();
			}
			return;
		}
		QWidget::keyPressEvent( event );
	}

	void CardOverlay::hideEvent( QHideEvent* event )
	{
		if ( focus_before_ != nullptr && !event->spontaneous() )
		{
			focus_before_->setFocus();
		}
		QWidget::hideEvent( event );
	}

} // namespace lexiglance::gui
