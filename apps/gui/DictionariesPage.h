#ifndef LEXIGLANCE_GUI_DICTIONARIESPAGE_H
#define LEXIGLANCE_GUI_DICTIONARIESPAGE_H

#include "Common.h"
#include "Downloader.h"

#include <lexiglance/core/Json.h>

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QTreeWidget>

#include <deque>
#include <optional>

namespace lexiglance::gui
{

	struct InstalledDictionary
	{
		QString title;
		QString revision;
		QString description;
		QString attribution;
		QString url;
		QString index_url;
		QString download_url;
		// What it is for ("Words", "Names", "Kanji", ...), as the daemon tells from its contents.
		QString       kind;
		bool          enabled   = true;
		bool          updatable = false;
		std::int64_t  terms     = 0;
		std::int64_t  meta      = 0;
		std::int64_t  kanji     = 0;
		std::uint64_t size      = 0;
	};

	class DictionariesPage : public Page
	{
	public:
		explicit DictionariesPage( Context context, QWidget* parent = nullptr );

		void refresh() override;
		void activated() override;

	protected:
		void dragEnterEvent( QDragEnterEvent* event ) override;
		void dropEvent( QDropEvent* event ) override;

	private:
		struct Download
		{
			QString name;
			QUrl    url;
			QString replaces;
		};

		void reload();
		void rebuildTable();
		void updateSummary();
		void importFiles( const QStringList& files );
		void showRecommended();
		void checkUpdates();
		void enqueueDownload( Download download );
		void startNextDownload();
		void move( int delta );
		// The rows' order after a drag becomes the priority order.
		void reorder();
		void sortAutomatically();
		void removeSelected();
		void showDetails();
		void setBusy( const QString& text, int value, int maximum );

		Downloader*                      downloader_;
		QTreeWidget*                     table_;
		QTextBrowser*                    details_;
		QProgressBar*                    progress_;
		QLabel*                          activity_;
		QPushButton*                     remove_;
		QPushButton*                     up_;
		QPushButton*                     down_;
		QPushButton*                     sort_;
		std::vector<InstalledDictionary> installed_;
		std::deque<Download>             downloads_;
		bool                             downloading_ = false;
		bool                             rebuilding_  = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_DICTIONARIESPAGE_H
