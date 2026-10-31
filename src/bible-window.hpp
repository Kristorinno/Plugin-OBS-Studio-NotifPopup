#pragma once

#include <QByteArray>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QSplitter>
#include <QString>
#include <QWidget>

#include <map>
#include <utility>
#include <memory>
#include <vector>

#include "bible-data.hpp"

class BibleVerseList;
class QHBoxLayout;
class QLabel;
class QListWidgetItem;
class QPoint;
class QTabBar;
class QTimer;
class QToolButton;

// Ventana "Biblia": 3/4 columnas (versículos, libros, capítulos y versículos) y una barra
// de atajos (favoritos, historial y más frecuentes). Proyecta mediante la señal projectRequested.
class BibleWindow : public QWidget {
	Q_OBJECT

public:
	explicit BibleWindow(QWidget *parent = nullptr);
	~BibleWindow() override;

	// Abre la ventana (la primera vez carga las biblias y los datos guardados).
	void showAndFocus();
	// Mensaje corto abajo de la ventana.
	void showStatus(const QString &msg);

	// --- Control remoto (celular) ---
	// Carga biblias y datos guardados sin mostrar la ventana.
	void prepare();
	QJsonObject remoteState() const;
	QJsonObject remoteBooks() const;
	QJsonObject remoteChapters(int book) const;
	QJsonObject remoteVerses(int book, int chap) const;
	QJsonObject remoteBar(int tab) const; // 0 favoritos, 1 historial, 2 más frecuentes
	void remoteLoad(int book, int chap, const std::vector<int> &verses); // carga sin proyectar
	void remoteProject();
	void remoteHide();
	void remoteStep(int delta); // versículo siguiente (+1) o anterior (-1), y lo proyecta
	void remoteAddFavorite();

signals:
	// Texto ya armado para la notificación y la cita corta (por ejemplo "Génesis 1:1-3").
	void projectRequested(const QString &text, const QString &label);
	void hideRequested();
	// Botón "Control remoto": el dock muestra el código QR.
	void remoteRequested();

protected:
	void closeEvent(QCloseEvent *e) override;
	bool eventFilter(QObject *obj, QEvent *ev) override;

private:
	struct Ref {
		int book;
		int chap;
		std::vector<int> verses;
	};

	// carga y guardado
	void ensureLoaded();
	void loadStore();
	void saveStore();
	void scanVersions();
	void addBibles();

	// versiones
	const BibleVersion *left() const;
	QString bookName(int number) const;
	void rebuildVersionBar();
	void moveVersion(int index, int delta);
	void onLeftChanged();

	// navegación
	void buildBooks();
	void buildBooksFiltered(const QString &query);
	void updateLayoutForMode(bool fullscreen);
	void navigateTo(int book, int chap, const std::vector<int> &verses);
	void fillChapters();
	void fillVerseGrid();
	void fillVerseList();
	void syncGrid();
	void selectOnly(int verse);
	void selectVerseItem(int verse, bool on);
	std::vector<int> selectedVerses() const;
	void onVerseSelectionChanged();

	// proyección y atajos
	void project();
	void record(int book, int chap, const std::vector<int> &verses);
	void addFavorite();
	void refreshBar();
	void onBarContextMenu(const QPoint &pos);
	void onVerseContextMenu(const QPoint &pos);
	void onBooksContextMenu(const QPoint &pos, QListWidget *list);
	void onGridContextMenu(const QPoint &pos, QListWidget *list);
	void removeFavorite(int index);
	void updateTitle();
	auto barItems(int tab) const -> std::vector<std::pair<Ref, QString>>;

	std::vector<std::shared_ptr<BibleVersion>> versions_;
	std::vector<QString> order_; // orden guardado de las versiones (nombres de archivo)

	std::vector<Ref> favorites_;
	std::vector<Ref> history_;
	std::map<QString, int> counts_;
	std::vector<Ref> barRefs_;

	QByteArray storedGeometry_;
	QByteArray storedSplitter_;

	int curBook_ = 1;
	int curChap_ = 1;
	std::vector<int> lastValid_;
	std::vector<int> projectingVerses_; // Versículos que están al aire
	bool updating_ = false;
	bool loaded_ = false;
	bool geometryRestored_ = false;

	QTabBar *barTabs_ = nullptr;
	QListWidget *barList_ = nullptr;
	QSplitter *split_ = nullptr;
	QWidget *versionHost_ = nullptr;
	QHBoxLayout *versionLay_ = nullptr;
	QToolButton *addBtn_ = nullptr;
	QToolButton *remoteBtn_ = nullptr;
	BibleVerseList *verses_ = nullptr;
	QListWidget *books_ = nullptr;
	QListWidget *chapters_ = nullptr;
	QListWidget *verseGrid_ = nullptr;
	QLabel *status_ = nullptr;
	QTimer *statusTimer_ = nullptr;

	QWidget *booksHost_ = nullptr;
	QSplitter *booksSplit_ = nullptr;
	QListWidget *booksOld_ = nullptr;
	QListWidget *booksNew_ = nullptr;
	QLineEdit *searchBooks_ = nullptr;
	bool isFullscreen_ = false;

	// Tamaños de fuente por columna (en puntos)
	int fontVerses_ = 13;
	int fontBooks_ = 11;
	int fontGrid_ = 11;
};
