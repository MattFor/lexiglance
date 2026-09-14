#ifndef LEXIGLANCE_GUI_COMMON_H
#define LEXIGLANCE_GUI_COMMON_H

#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::gui
{

	class DaemonClient;
	class Settings;

	[[nodiscard]] inline QString qs( std::string_view text )
	{
		return QString::fromUtf8( text.data(), static_cast<qsizetype>( text.size() ) );
	}

	[[nodiscard]] inline std::string ss( const QString& text )
	{
		return text.toStdString();
	}

	[[nodiscard]] QString formatBytes( std::uint64_t bytes );

	[[nodiscard]] QString chordText( const std::vector<std::string>& keys );

	// "JMdict [2026-09-12]" -> "JMdict".
	[[nodiscard]] QString shortTitle( const QString& title );

	// lexiglanced next to this executable, in the build tree, or on PATH.
	[[nodiscard]] QString daemonExecutable();

	// Asks before something that cannot be undone; true when `action` (the button's text) was chosen.
	[[nodiscard]] bool confirm( QWidget* parent, const QString& title, const QString& text, const QString& action );

	struct Context
	{
		DaemonClient* client   = nullptr;
		Settings*     settings = nullptr;
		// Brings a page to the front by name ("dictionaries", "scanning", ...).
		std::function<void( const QString& )> show_page;
		// Shows a note at the right of the status bar (the dictionaries' summary).
		std::function<void( const QString& )> show_summary;
	};

	class Page : public QWidget
	{
	public:
		explicit Page( Context context, QWidget* parent = nullptr ) :
			QWidget( parent ),
			context_( std::move( context ) )
		{
		}

		// The configuration was (re)loaded from the daemon.
		virtual void refresh() {}

		// The page became visible.
		virtual void activated() {}

	protected:
		[[nodiscard]] DaemonClient& client() const
		{
			return *context_.client;
		}

		[[nodiscard]] Settings& settings() const
		{
			return *context_.settings;
		}

		void showPage( const QString& name ) const
		{
			if ( context_.show_page )
			{
				context_.show_page( name );
			}
		}

		void showSummary( const QString& text ) const
		{
			if ( context_.show_summary )
			{
				context_.show_summary( text );
			}
		}

	private:
		Context context_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_COMMON_H
