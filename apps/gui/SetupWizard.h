#ifndef LEXIGLANCE_GUI_SETUPWIZARD_H
#define LEXIGLANCE_GUI_SETUPWIZARD_H

#include "Common.h"
#include "Downloader.h"

#include <lexiglance/language/Language.h>

#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lexiglance::gui
{

	// First-run (and post-update) setup: welcome, languages, then dictionaries and OCR downloaded without further clicks
	// (the keys explained meanwhile), and a sentence to try the popup on. For the first five seconds Escape and clicks
	// beside the card do nothing, so the welcome is not dismissed by accident. First-run setup cannot be skipped at all.
	// Dev builds can open it again from Help; that path reinstalls dictionaries and OCR.
	class SetupWizard final : public QWidget
	{
	public:
		SetupWizard( Context context, std::function<void()> finished, QWidget* parent );

		// Covers the parent. `lock_seconds`: how long Continue is held back.
		// `reinstall`: download dictionaries and OCR again even when already present (Help simulate).
		// `required`: first-run — no Skip, Escape, or click-outside dismiss until Get started.
		void open( int lock_seconds = 5, bool reinstall = false, bool required = false );

		bool eventFilter( QObject* watched, QEvent* event ) override;

	protected:
		void paintEvent( QPaintEvent* event ) override;
		void mousePressEvent( QMouseEvent* event ) override;
		void keyPressEvent( QKeyEvent* event ) override;

	private:
		enum class Page : std::uint8_t
		{
			Welcome = 0,
			Languages,
			Install,
			Done
		};

		struct InstallBatch;

		void                                             place();
		void                                             showPage( Page page );
		void                                             applyLanguages();
		void                                             startInstall();
		void                                             beginBatch( const std::shared_ptr<InstallBatch>& batch );
		void                                             pumpImports( const std::shared_ptr<InstallBatch>& batch );
		void                                             onDictDownloadDone( const std::shared_ptr<InstallBatch>& batch, const QString& name, const QString& target, const QString& error );
		void                                             onOcrDownloadDone( const std::shared_ptr<InstallBatch>& batch, const QString& error );
		void                                             refreshProgress( const std::shared_ptr<InstallBatch>& batch );
		void                                             tryFinishBatch( const std::shared_ptr<InstallBatch>& batch );
		void                                             finishInstall( const QString& error );
		void                                             complete();
		void                                             resetProgress();
		void                                             setOverall( int value, int maximum, const QString& text );
		[[nodiscard]] bool                               locked() const;
		[[nodiscard]] bool                               dismissible() const;
		[[nodiscard]] std::vector<const lang::Language*> selectedLanguages() const;
		// The translation option's label: how much it adds for the languages chosen.
		void updateTranslationBox();
		// The keys as they are set, for the tips while downloading and the sentence to try.
		void updateKeys();
		// The sentence to try the popup on, in the first language chosen, and the box as tall as it.
		void updatePractice();
		void fitPractice();

		Context                  context_;
		std::function<void()>    finished_;
		Downloader*              downloader_;
		QFrame*                  card_;
		QStackedWidget*          pages_;
		QLabel*                  welcome_title_;
		QLabel*                  lock_hint_;
		QPushButton*             continue_;
		QPushButton*             skip_;
		QLabel*                  install_status_;
		QLabel*                  overall_label_;
		QProgressBar*            overall_bar_;
		QLabel*                  dict_label_;
		QProgressBar*            dict_bar_;
		QLabel*                  ocr_label_;
		QProgressBar*            ocr_bar_;
		QCheckBox*               translation_box_;
		QLabel*                  translation_note_;
		QLabel*                  tips_;
		QLabel*                  done_blurb_;
		QTextBrowser*            practice_;
		QLabel*                  practice_status_;
		QPushButton*             done_;
		std::vector<QCheckBox*>  language_boxes_;
		std::vector<std::string> language_codes_;
		qint64                   unlock_at_ms_ = 0;
		bool                     installing_   = false;
		bool                     reinstall_    = false;
		bool                     required_     = false;
		// A word was found on the sentence to try.
		bool    tried_ = false;
		QString redist_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_SETUPWIZARD_H
