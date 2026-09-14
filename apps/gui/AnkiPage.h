#ifndef LEXIGLANCE_GUI_ANKIPAGE_H
#define LEXIGLANCE_GUI_ANKIPAGE_H

#include "Common.h"

#include <QCheckBox>
#include <QComboBox>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>

#include <functional>

namespace lexiglance::gui
{

	// Pronunciation audio and Anki export (AnkiConnect).
	class AnkiPage : public Page
	{
	public:
		explicit AnkiPage( Context context, QWidget* parent = nullptr );

		void refresh() override;
		void activated() override;

	private:
		using Reply = std::function<void( const QJsonValue& result, const QString& error )>;

		void storeAudio();
		void storeAnki();
		void connectAnki();
		void loadFields( const QString& model );
		void showFields( const QStringList& names );
		void request( const QString& action, const QJsonObject& params, Reply done );

		QNetworkAccessManager* network_;
		QCheckBox*             audio_enabled_;
		QCheckBox*             autoplay_;
		QPlainTextEdit*        sources_;
		QPushButton*           test_audio_;
		QLabel*                audio_status_;
		QCheckBox*             anki_enabled_;
		QLineEdit*             url_;
		QLineEdit*             key_;
		QPushButton*           connect_;
		QLabel*                anki_status_;
		QComboBox*             deck_;
		QComboBox*             model_;
		QTableWidget*          fields_;
		QLineEdit*             tags_;
		QCheckBox*             duplicates_;
		bool                   loading_ = false;
		// AnkiConnect answered the last time it was asked; a poll is on its way.
		bool anki_connected_ = false;
		bool anki_asking_    = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_ANKIPAGE_H
