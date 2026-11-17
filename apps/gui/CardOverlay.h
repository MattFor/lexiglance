#ifndef LEXIGLANCE_GUI_CARDOVERLAY_H
#define LEXIGLANCE_GUI_CARDOVERLAY_H

#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>

namespace lexiglance::gui
{

	// A rounded card over the whole window, which dims behind it: what the settings application shows in place of a
	// dialog. Escape or a click beside the card closes it, unless it is told to stay; Enter presses the button in focus,
	// or the card's primary one. The focus goes back where it was when the card closes.
	class CardOverlay : public QWidget
	{
	public:
		// `width`: the card's at most; it narrows with the window.
		CardOverlay( int width, QWidget* parent );

		bool eventFilter( QObject* watched, QEvent* event ) override;

	protected:
		[[nodiscard]] QFrame* card() const
		{
			return card_;
		}

		// Covers the window and shows the card.
		void popUp();
		// Fits the card to what it holds, after that changed.
		void place();
		// The card's layout, with the margins and spacing every card has.
		QVBoxLayout* cardLayout();
		// A card's heading.
		[[nodiscard]] static QLabel* cardTitle( const QString& text = {} );

		void setDismissible( bool dismissible )
		{
			dismissible_ = dismissible;
		}

		void paintEvent( QPaintEvent* event ) override;
		void mousePressEvent( QMouseEvent* event ) override;
		void keyPressEvent( QKeyEvent* event ) override;
		void hideEvent( QHideEvent* event ) override;

	private:
		QFrame*           card_;
		int               width_;
		bool              dismissible_ = true;
		QPointer<QWidget> focus_before_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_CARDOVERLAY_H
