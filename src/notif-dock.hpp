#pragma once

#include <QWidget>
#include <obs.hpp>

#include <utility>
#include <vector>

class BibleServer;
class BibleWindow;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDialog;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QTimer;

class NotifDock : public QWidget {
	Q_OBJECT

public:
	explicit NotifDock(QWidget *parent = nullptr);

public slots:
	// Lo usa el atajo de teclado: muestra la frase elegida o la oculta si ya se ve.
	void toggleSelected();
	// Abre la ventana de la Biblia (botón del dock, menú Herramientas y atajo de teclado).
	void openBible();

private:
	template<class F> void push(F fn);

	OBSSource current();
	void refresh();
	void loadFromConfig();
	void callProc(const char *name);
	void setColorButton(QPushButton *b, uint32_t abgr);
	QSlider *addSlider(QFormLayout *form, const QString &label, int min, int max, const QString &suffix,
			   QLabel **valueLabel);

	// Lista de frases (1 a 9)
	QString phrase(int n);
	void updateSummary();
	void openListDialog();
	void showSelected();

	// Casilla 0: la Biblia (no tiene botón en el dock)
	void ensureBible();
	void setBibleActive(bool on);
	void showRemoteQr();

	std::vector<std::pair<QString, OBSWeakSource>> items_;
	bool loading_ = false;

	QComboBox *target_ = nullptr;
	QWidget *quick_ = nullptr;
	QWidget *panel_ = nullptr;
	QDialog *settingsWin_ = nullptr;

	QLabel *summary_ = nullptr;
	QPushButton *numBtns_[9] = {};
	QButtonGroup *group_ = nullptr;
	BibleWindow *bible_ = nullptr;
	BibleServer *server_ = nullptr;
	bool bibleActive_ = false;
	QString bibleText_;
	QString bibleLabel_;
	int selected_ = 1;

	QComboBox *direction_ = nullptr;
QDoubleSpinBox *animInSec_ = nullptr;
QDoubleSpinBox *animOutSec_ = nullptr;
QCheckBox *linkAnim_ = nullptr;
QDoubleSpinBox *holdSec_ = nullptr;
	QRadioButton *modeTime_ = nullptr;
	QRadioButton *modeAction_ = nullptr;
	QSlider *blur_ = nullptr;
	QComboBox *font_ = nullptr;
	QSpinBox *fontSize_ = nullptr;
	QCheckBox *bold_ = nullptr;
	QPushButton *bgBtn_ = nullptr;
	QPushButton *fgBtn_ = nullptr;
	QCheckBox *shadow_ = nullptr;
	QSlider *shAngle_ = nullptr;
	QSlider *shLen_ = nullptr;
	QSlider *shBlur_ = nullptr;
	QSlider *shAmount_ = nullptr;

	uint32_t bgColor_ = 0xFFFF4010;
	uint32_t fgColor_ = 0xFFFFFFFF;
	QTimer *timer_ = nullptr;
};
