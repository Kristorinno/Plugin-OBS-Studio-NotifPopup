#include "bible-window.hpp"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QInputDialog>
#include <QSaveFile>
#include <QPainter>
#include <QSet>
#include <QStyledItemDelegate>
#include <QShortcut>
#include <QSplitter>
#include <QStringList>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <functional>
#include <iterator>
#include <tuple>

namespace {

constexpr int kMaxSel = 3;       // máximo de versículos proyectados a la vez
constexpr int kMaxHistory = 30;  // registros del historial
constexpr int kTopFrequent = 10; // versículos en "Más frecuentes"

QString biblias_dir()
{
	char *p = obs_module_config_path("biblias");
	const QString s = QString::fromUtf8(p ? p : "");
	bfree(p);
	if (!s.isEmpty())
		os_mkdirs(s.toUtf8().constData());
	return s;
}

QString store_path()
{
	char *p = obs_module_config_path("bible.json");
	const QString s = QString::fromUtf8(p ? p : "");
	bfree(p);
	return s;
}

// [1,2,3] -> "1-3"; [1,3,5] -> "1, 3, 5"
QString compress(const std::vector<int> &v)
{
	QStringList parts;
	size_t i = 0;
	while (i < v.size()) {
		size_t j = i;
		while (j + 1 < v.size() && v[j + 1] == v[j] + 1)
			j++;
		if (j > i)
			parts << QStringLiteral("%1-%2").arg(v[i]).arg(v[j]);
		else
			parts << QString::number(v[i]);
		i = j + 1;
	}
	return parts.join(QStringLiteral(", "));
}

QListWidget *make_grid(QWidget *parent)
{
	auto *l = new QListWidget(parent);
	l->setViewMode(QListView::ListMode);
	l->setFlow(QListView::LeftToRight);
	l->setWrapping(true);
	l->setResizeMode(QListView::Adjust);
	l->setMovement(QListView::Static);
	l->setGridSize(QSize(52, 40));
	l->setUniformItemSizes(true);
	l->setSelectionMode(QAbstractItemView::SingleSelection);
	l->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	l->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	l->setStyleSheet(QStringLiteral("QListWidget { background: #212121; border: none; }"
					"QListWidget::item { background: #3d3d3d; color: #e6e6e6;"
					" border: 1px solid #212121; }"
					"QListWidget::item:hover { background: #4a4a4a; }"
					"QListWidget::item:selected { background: #b9c3cb; color: #101010; }"));
	QFont f = l->font();
	if (f.pointSizeF() > 0)
		f.setPointSizeF(f.pointSizeF() + 2);
	l->setFont(f);
	return l;
}

// Rol donde se marca si un versículo está al aire (proyectándose).
constexpr int kProjectingRole = Qt::UserRole + 1;

// Marca o desmarca un versículo como proyectándose. El color lo dibuja el delegate.
void apply_verse_style(QListWidgetItem *it, bool isProjecting)
{
	it->setData(kProjectingRole, isProjecting);
}

// Dibuja el fondo a mano: morado si se está proyectando, azul si está seleccionado.
// Así el stylesheet no pisa los colores.
class VerseDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QStyleOptionViewItem o(option);
		const bool proj = index.data(kProjectingRole).toBool();
		const bool sel = (o.state & QStyle::State_Selected) != 0;
		if (proj || sel) {
			p->fillRect(o.rect, proj ? QColor(138, 43, 226) : QColor(31, 111, 192));
			o.palette.setColor(QPalette::Text, Qt::white);
		} else {
			o.palette.setColor(QPalette::Text, QColor(238, 238, 238));
		}
		o.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_MouseOver);
		QStyledItemDelegate::paint(p, o, index);
	}
};

// Cambia el tamaño de fuente de una lista. Lo pone también en su stylesheet,
// porque un stylesheet activo puede ignorar el setFont.
void set_list_font(QListWidget *l, int pt)
{
	QVariant base = l->property("baseStyle");
	if (!base.isValid()) {
		base = l->styleSheet();
		l->setProperty("baseStyle", base);
	}
	QFont f = l->font();
	f.setPointSize(pt);
	l->setFont(f);
	l->setStyleSheet(base.toString() + QStringLiteral("QListWidget { font-size: %1pt; }").arg(pt));
	l->viewport()->update();
}

} // namespace

// Lista de versículos de la primera columna. Permite elegir hasta 3 con Ctrl, y que el doble
// clic proyecte la selección múltiple sin deshacerla.
class BibleVerseList : public QListWidget {
public:
	explicit BibleVerseList(QWidget *parent = nullptr) : QListWidget(parent) {}

	std::function<void()> onProject;

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		prev_.clear();
		const QList<QListWidgetItem *> sel = selectedItems();
		for (QListWidgetItem *it : sel)
			prev_.push_back(row(it));
		QListWidget::mousePressEvent(e);
	}

	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		QListWidgetItem *hit = itemAt(e->position().toPoint());
		if (!hit)
			return;
		const bool ctrl = (e->modifiers() & Qt::ControlModifier) != 0;
		if (!ctrl && prev_.size() > 1 && std::find(prev_.begin(), prev_.end(), row(hit)) != prev_.end()) {
			// El primer clic del doble clic dejó un solo versículo: se vuelve a la selección anterior.
			clearSelection();
			for (int r : prev_)
				if (QListWidgetItem *it = item(r))
					it->setSelected(true);
		}
		if (onProject)
			onProject();
		skipRelease_ = true;
		e->accept();
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (skipRelease_) {
			skipRelease_ = false;
			e->accept();
			return;
		}
		QListWidget::mouseReleaseEvent(e);
	}

private:
	std::vector<int> prev_;
	bool skipRelease_ = false;
};

// ---------------------------------------------------------------------------

BibleWindow::BibleWindow(QWidget *parent) : QWidget(nullptr, Qt::Window)
{
	setWindowTitle(QStringLiteral("Biblia"));
	resize(1120, 720);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(4);

	// --- Barra superior: Favoritos / Historial / Más frecuentes ---
	barTabs_ = new QTabBar;
	barTabs_->addTab(QStringLiteral("Favoritos"));
	barTabs_->addTab(QStringLiteral("Historial"));
	barTabs_->addTab(QStringLiteral("Más frecuentes"));
	barTabs_->setExpanding(false);
	root->addWidget(barTabs_);

	barList_ = new QListWidget;
	barList_->setViewMode(QListView::ListMode);
	barList_->setFlow(QListView::LeftToRight);
	barList_->setWrapping(false);
	barList_->setMovement(QListView::Static);
	barList_->setSpacing(2);
	barList_->setSelectionMode(QAbstractItemView::SingleSelection);
	barList_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	barList_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	barList_->setFixedHeight(80);
	barList_->setContextMenuPolicy(Qt::CustomContextMenu);
	barList_->setStyleSheet(QStringLiteral("QListWidget { background: #262626; border: none; }"
					       "QListWidget::item { background: #343434; color: #dddddd;"
					       " border: 1px solid #1a1a1a; border-radius: 6px; }"
					       "QListWidget::item:hover { background: #444444; }"
					       "QListWidget::item:selected { background: #4a6fa5; color: #ffffff; }"));
	root->addWidget(barList_);

	// --- Tres/Cuatro columnas ---
	split_ = new QSplitter(Qt::Horizontal);

	// Columna 1: versiones + versículos
	auto *col1 = new QWidget;
	auto *col1Lay = new QVBoxLayout(col1);
	col1Lay->setContentsMargins(0, 0, 0, 0);
	col1Lay->setSpacing(4);

	auto *head = new QHBoxLayout;
	head->setContentsMargins(0, 0, 0, 0);
	versionHost_ = new QWidget;
	versionLay_ = new QHBoxLayout(versionHost_);
	versionLay_->setContentsMargins(0, 0, 0, 0);
	versionLay_->setSpacing(4);
	addBtn_ = new QToolButton;
	addBtn_->setText(QStringLiteral("+"));
	addBtn_->setToolTip(QStringLiteral("Agregar biblia (.xml de HolyRics, o .txt con líneas 'Libro 1:1 texto')"));
	remoteBtn_ = new QToolButton;
	remoteBtn_->setText(QStringLiteral("Control remoto"));
	remoteBtn_->setToolTip(QStringLiteral("Muestra el código QR para controlar la Biblia desde el celular"));
	head->addWidget(versionHost_, 1);
	head->addWidget(remoteBtn_);
	head->addWidget(addBtn_);
	col1Lay->addLayout(head);

	verses_ = new BibleVerseList;
	verses_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	verses_->setWordWrap(true);
	verses_->setTextElideMode(Qt::ElideNone);
	verses_->setResizeMode(QListView::Adjust);
	verses_->setUniformItemSizes(false);
	verses_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	verses_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	verses_->setContextMenuPolicy(Qt::CustomContextMenu);
	verses_->setStyleSheet(QStringLiteral("QListWidget { background: #1f1f1f; border: none; }"
					      "QListWidget::item { padding: 8px 10px;"
					      " border-bottom: 1px solid #3a3a3a; }"));
	verses_->setItemDelegate(new VerseDelegate(verses_));
	{
		QFont f = verses_->font();
		if (f.pointSizeF() > 0)
			f.setPointSizeF(f.pointSizeF() + 3);
		verses_->setFont(f);
	}
	col1Lay->addWidget(verses_, 1);
	split_->addWidget(col1);

	// --- Contenedor de Libros (con buscador) ---
	booksHost_ = new QWidget;
	auto *booksHostLay = new QVBoxLayout(booksHost_);
	booksHostLay->setContentsMargins(0, 0, 0, 0);
	booksHostLay->setSpacing(4);

	searchBooks_ = new QLineEdit;
	searchBooks_->setPlaceholderText("Buscar libro...");
	searchBooks_->setClearButtonEnabled(true);
	searchBooks_->setStyleSheet("QLineEdit { background: #333; color: #fff; padding: 6px; border-radius: 4px; }");
	booksHostLay->addWidget(searchBooks_);

	booksSplit_ = new QSplitter(Qt::Horizontal);
	
	booksOld_ = new QListWidget;
	booksOld_->setSelectionMode(QAbstractItemView::SingleSelection);
	booksOld_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	booksOld_->setStyleSheet(QStringLiteral("QListWidget { background: #222222; border: none; }"
					     "QListWidget::item { color: #eeeeee; padding: 8px 12px;"
					     " border-bottom: 1px solid #333333; }"
					     "QListWidget::item:hover { background: #333333; }"
					     "QListWidget::item:selected { background: #5a6a74; color: #ffffff; }"));
	
	booksNew_ = new QListWidget;
	booksNew_->setSelectionMode(QAbstractItemView::SingleSelection);
	booksNew_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	booksNew_->setStyleSheet(booksOld_->styleSheet());

	booksSplit_->addWidget(booksOld_);
	booksSplit_->addWidget(booksNew_);
	booksHostLay->addWidget(booksSplit_, 1);

	// Lo guardamos temporalmente para el col3
	books_ = booksOld_; 

	split_->addWidget(booksHost_);

	// Columna 3: capítulos (mitad de arriba) y versículos (mitad de abajo)
	auto *col3 = new QSplitter(Qt::Vertical);
	auto *chapBox = new QWidget;
	auto *chapLay = new QVBoxLayout(chapBox);
	chapLay->setContentsMargins(0, 0, 0, 0);
	chapLay->setSpacing(2);
	chapLay->addWidget(new QLabel(QStringLiteral("Capítulos")));
	chapters_ = make_grid(chapBox);
	chapLay->addWidget(chapters_, 1);
	col3->addWidget(chapBox);

	auto *verseBox = new QWidget;
	auto *verseLay = new QVBoxLayout(verseBox);
	verseLay->setContentsMargins(0, 0, 0, 0);
	verseLay->setSpacing(2);
	verseLay->addWidget(new QLabel(QStringLiteral("Versículos")));
	verseGrid_ = make_grid(verseBox);
	verseLay->addWidget(verseGrid_, 1);
	col3->addWidget(verseBox);
	col3->setSizes({300, 300});
	split_->addWidget(col3);

	split_->setStretchFactor(0, 4);
	split_->setStretchFactor(1, 3);
	split_->setStretchFactor(2, 4);
	split_->setSizes({400, 300, 400});
	root->addWidget(split_, 1);

	status_ = new QLabel;
	status_->setMinimumHeight(20);
	root->addWidget(status_);

	statusTimer_ = new QTimer(this);
	statusTimer_->setSingleShot(true);
	connect(statusTimer_, &QTimer::timeout, this, [this] { status_->clear(); });

	// --- Conexiones ---
	connect(addBtn_, &QToolButton::clicked, this, [this] { addBibles(); });
	connect(remoteBtn_, &QToolButton::clicked, this, [this] { emit remoteRequested(); });
	connect(barTabs_, &QTabBar::currentChanged, this, [this](int) { refreshBar(); });
	connect(barList_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
		const QVariant d = it->data(Qt::UserRole);
		if (!d.isValid())
			return;
		const int idx = d.toInt();
		if (idx < 0 || size_t(idx) >= barRefs_.size())
			return;
		const Ref r = barRefs_[size_t(idx)];
		navigateTo(r.book, r.chap, r.verses); // se carga en la primera columna, sin proyectar
	});
	connect(barList_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { onBarContextMenu(p); });
	connect(verses_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { onVerseContextMenu(p); });
	
	// Menús para cambiar el tamaño de la fuente
	booksOld_->setContextMenuPolicy(Qt::CustomContextMenu);
	booksNew_->setContextMenuPolicy(Qt::CustomContextMenu);
	chapters_->setContextMenuPolicy(Qt::CustomContextMenu);
	verseGrid_->setContextMenuPolicy(Qt::CustomContextMenu);
	
	connect(booksOld_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { onBooksContextMenu(p, booksOld_); });
	connect(booksNew_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { onBooksContextMenu(p, booksNew_); });
	connect(chapters_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { onGridContextMenu(p, chapters_); });
	connect(verseGrid_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { onGridContextMenu(p, verseGrid_); });
	connect(verses_, &QListWidget::itemSelectionChanged, this, [this] { onVerseSelectionChanged(); });
	verses_->onProject = [this] { project(); };

	connect(chapters_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
		navigateTo(curBook_, it->data(Qt::UserRole).toInt(), {1});
	});
	connect(verseGrid_, &QListWidget::itemClicked, this,
		[this](QListWidgetItem *it) { selectOnly(it->data(Qt::UserRole).toInt()); });

	// --- Lógica de Pantalla Completa y Buscador ---
	// Detecta el cambio de tamaño de la ventana (maximizado o no)
	installEventFilter(this);
	
	// Filtro del buscador
	connect(searchBooks_, &QLineEdit::textChanged, this, [this](const QString &text) {
		const QString query = text.trimmed().toLower();
		
		// Llenamos las listas según el filtro
		buildBooksFiltered(query);
	});
	
	// Cuando se cambia de libro en cualquiera de las dos listas, sincronizamos la otra
	connect(booksOld_, &QListWidget::currentRowChanged, this, [this](int row) {
		if (updating_ || row < 0) return;
		booksNew_->clearSelection();
		QListWidgetItem *it = booksOld_->item(row);
		if (it) navigateTo(it->data(Qt::UserRole).toInt(), 1, {1});
	});
	connect(booksNew_, &QListWidget::currentRowChanged, this, [this](int row) {
		if (updating_ || row < 0) return;
		booksOld_->clearSelection();
		QListWidgetItem *it = booksNew_->item(row);
		if (it) navigateTo(it->data(Qt::UserRole).toInt(), 1, {1});
	});

	// --- Teclas (solo valen con esta ventana en primer plano) ---
	auto addKey = [this](const QKeySequence &key, QWidget *w, Qt::ShortcutContext ctx, std::function<void()> fn) {
		auto *sc = new QShortcut(key, w);
		sc->setContext(ctx);
		connect(sc, &QShortcut::activated, this, std::move(fn));
	};
	addKey(QKeySequence(Qt::Key_Return), this, Qt::WindowShortcut, [this] { project(); });
	addKey(QKeySequence(Qt::Key_Enter), this, Qt::WindowShortcut, [this] { project(); });
	addKey(QKeySequence(Qt::Key_Escape), this, Qt::WindowShortcut, [this] { 
		projectingVerses_.clear();
		for (int i = 0; i < verses_->count(); i++) {
			apply_verse_style(verses_->item(i), false);
		}
		verses_->viewport()->update(); // Forzamos el repintado del delegate
		emit hideRequested(); 
	});
	addKey(QKeySequence(Qt::Key_F6), this, Qt::WindowShortcut, [this] { addFavorite(); });
	addKey(QKeySequence(Qt::Key_Delete), barList_, Qt::WidgetShortcut, [this] {
		if (barTabs_->currentIndex() != 0)
			return;
		QListWidgetItem *it = barList_->currentItem();
		if (!it || !it->data(Qt::UserRole).isValid())
			return;
		removeFavorite(it->data(Qt::UserRole).toInt());
	});
}

BibleWindow::~BibleWindow()
{
	saveStore();
}

void BibleWindow::closeEvent(QCloseEvent *e)
{
	saveStore();
	QWidget::closeEvent(e);
}

void BibleWindow::showAndFocus()
{
	ensureLoaded();

	if (!geometryRestored_) {
		geometryRestored_ = true;
		if (!storedGeometry_.isEmpty())
			restoreGeometry(storedGeometry_);
		else
			setWindowState(Qt::WindowMaximized); // primera vez: pantalla completa
	}

	// Restauramos el estado (maximizado o normal) desde el archivo de configuración
	if (isMaximized()) {
		showMaximized();
	} else {
		showNormal();
	}
	raise();
	activateWindow();
	verses_->setFocus();
}

void BibleWindow::showStatus(const QString &msg)
{
	status_->setText(msg);
	statusTimer_->start(4000);
}

// ---------------------------------------------------------------------------
// Carga y guardado
// ---------------------------------------------------------------------------

void BibleWindow::ensureLoaded()
{
	if (loaded_)
		return;
	loaded_ = true;

	loadStore();
	
	// Aplicamos los tamaños de fuente guardados
	{
		set_list_font(verses_, fontVerses_);
		set_list_font(booksOld_, fontBooks_);
		set_list_font(booksNew_, fontBooks_);
		set_list_font(chapters_, fontGrid_);
		set_list_font(verseGrid_, fontGrid_);
	}
	
	scanVersions();

	if (!storedSplitter_.isEmpty())
		split_->restoreState(storedSplitter_);

	rebuildVersionBar();
	buildBooks();
	navigateTo(1, 1, {1});
	refreshBar();

	if (versions_.empty())
		showStatus(QStringLiteral("No hay biblias cargadas: tocá + y elegí los archivos .xml o .txt"));
}

void BibleWindow::loadStore()
{
	QFile f(store_path());
	if (!f.open(QIODevice::ReadOnly))
		return;
	const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();

	order_.clear();
	const QJsonArray ord = o.value(QStringLiteral("order")).toArray();
	for (const QJsonValue &v : ord)
		order_.push_back(v.toString());

	auto readRef = [](const QJsonObject &r) {
		Ref x{r.value(QStringLiteral("b")).toInt(), r.value(QStringLiteral("c")).toInt(), {}};
		const QJsonArray vs = r.value(QStringLiteral("v")).toArray();
		for (const QJsonValue &v : vs)
			x.verses.push_back(v.toInt());
		return x;
	};

	favorites_.clear();
	const QJsonArray fav = o.value(QStringLiteral("favorites")).toArray();
	for (const QJsonValue &v : fav) {
		const Ref r = readRef(v.toObject());
		if (r.book > 0 && r.chap > 0 && !r.verses.empty())
			favorites_.push_back(r);
	}

	history_.clear();
	const QJsonArray hist = o.value(QStringLiteral("history")).toArray();
	for (const QJsonValue &v : hist) {
		const Ref r = readRef(v.toObject());
		if (r.book > 0 && r.chap > 0 && !r.verses.empty())
			history_.push_back(r);
	}

	counts_.clear();
	const QJsonObject cnt = o.value(QStringLiteral("counts")).toObject();
	for (auto it = cnt.begin(); it != cnt.end(); ++it)
		counts_[it.key()] = it.value().toInt();

	storedGeometry_ = QByteArray::fromBase64(o.value(QStringLiteral("geometry")).toString().toLatin1());
	storedSplitter_ = QByteArray::fromBase64(o.value(QStringLiteral("splitter")).toString().toLatin1());
	int storedState = o.value(QStringLiteral("window_state")).toInt(Qt::WindowNoState);
	if (storedState & Qt::WindowMaximized) setWindowState(Qt::WindowMaximized);
	
	// Cargamos los tamaños de fuente
	fontVerses_ = o.value(QStringLiteral("font_verses")).toInt(13);
	fontBooks_ = o.value(QStringLiteral("font_books")).toInt(11);
	fontGrid_ = o.value(QStringLiteral("font_grid")).toInt(11);
}

void BibleWindow::saveStore()
{
	if (!loaded_)
		return;
	biblias_dir(); // se asegura de que exista la carpeta de ajustes

	if (!versions_.empty()) {
		order_.clear();
		for (const auto &v : versions_)
			order_.push_back(v->file);
	}

	auto writeRef = [](const Ref &r) {
		QJsonObject o;
		o.insert(QStringLiteral("b"), r.book);
		o.insert(QStringLiteral("c"), r.chap);
		QJsonArray vs;
		for (int v : r.verses)
			vs.append(v);
		o.insert(QStringLiteral("v"), vs);
		return o;
	};

	QJsonObject o;
	QJsonArray ord;
	for (const QString &s : order_)
		ord.append(s);
	o.insert(QStringLiteral("order"), ord);

	QJsonArray fav;
	for (const Ref &r : favorites_)
		fav.append(writeRef(r));
	o.insert(QStringLiteral("favorites"), fav);

	QJsonArray hist;
	for (const Ref &r : history_)
		hist.append(writeRef(r));
	o.insert(QStringLiteral("history"), hist);

	QJsonObject cnt;
	for (const auto &kv : counts_)
		cnt.insert(kv.first, kv.second);
	o.insert(QStringLiteral("counts"), cnt);

	o.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
	o.insert(QStringLiteral("splitter"), QString::fromLatin1(split_->saveState().toBase64()));
	o.insert(QStringLiteral("window_state"), int(windowState()));
	
	// Guardamos los tamaños de fuente
	o.insert(QStringLiteral("font_verses"), fontVerses_);
	o.insert(QStringLiteral("font_books"), fontBooks_);
	o.insert(QStringLiteral("font_grid"), fontGrid_);

	QSaveFile f(store_path());
	if (!f.open(QIODevice::WriteOnly))
		return;
	f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
	f.commit();
}

// Lee las biblias de la carpeta del plugin (las que ya están cargadas no se vuelven a leer).
void BibleWindow::scanVersions()
{
	const QDir dir(biblias_dir());
	const QStringList files = dir.entryList({QStringLiteral("*.xml"), QStringLiteral("*.txt")}, QDir::Files, QDir::Name);

	QString firstError;
	QApplication::setOverrideCursor(Qt::WaitCursor);
	for (const QString &name : files) {
		bool already = false;
		for (const auto &v : versions_)
			if (v->file == name)
				already = true;
		if (already)
			continue;

		QString err;
		std::shared_ptr<BibleVersion> v = bible_load(dir.filePath(name), &err);
		if (v)
			versions_.push_back(v);
		else if (firstError.isEmpty())
			firstError = name + QStringLiteral(": ") + err;
	}
	QApplication::restoreOverrideCursor();

	auto idxOf = [this](const std::shared_ptr<BibleVersion> &v) {
		auto it = std::find(order_.begin(), order_.end(), v->file);
		return it == order_.end() ? INT_MAX : int(it - order_.begin());
	};
	std::stable_sort(versions_.begin(), versions_.end(),
			 [&](const std::shared_ptr<BibleVersion> &a, const std::shared_ptr<BibleVersion> &b) {
				 return std::make_tuple(idxOf(a), bible_default_rank(a->label), a->label) <
					std::make_tuple(idxOf(b), bible_default_rank(b->label), b->label);
			 });

	if (!firstError.isEmpty())
		showStatus(QStringLiteral("No se pudo leer ") + firstError);
}

void BibleWindow::addBibles()
{
	const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Agregar biblia"), QString(),
								QStringLiteral("Biblias (*.xml *.txt)"));
	if (paths.isEmpty())
		return;

	const QString dir = biblias_dir();
	for (const QString &p : paths) {
		const QFileInfo src(p);
		const QString dest = dir + QLatin1Char('/') + src.fileName();
		if (src.absoluteFilePath() != QFileInfo(dest).absoluteFilePath()) {
			QFile::remove(dest);
			QFile::copy(p, dest);
		}
		// Si ya estaba cargada una con ese nombre, se descarta para volver a leerla.
		versions_.erase(std::remove_if(versions_.begin(), versions_.end(),
					       [&](const std::shared_ptr<BibleVersion> &v) { return v->file == src.fileName(); }),
				versions_.end());
	}

	scanVersions();
	rebuildVersionBar();
	saveStore();
	onLeftChanged();
}

// ---------------------------------------------------------------------------
// Versiones
// ---------------------------------------------------------------------------

const BibleVersion *BibleWindow::left() const
{
	return versions_.empty() ? nullptr : versions_.front().get();
}

QString BibleWindow::bookName(int number) const
{
	if (const BibleVersion *lv = left()) {
		auto b = lv->books.find(number);
		if (b != lv->books.end() && !b->second.name.isEmpty())
			return b->second.name;
	}
	return bible_book_name(number);
}

void BibleWindow::rebuildVersionBar()
{
	while (QLayoutItem *child = versionLay_->takeAt(0)) {
		if (QWidget *w = child->widget())
			w->deleteLater();
		delete child;
	}

	for (size_t i = 0; i < versions_.size(); i++) {
		const bool first = (i == 0);
		auto *box = new QFrame;
		box->setStyleSheet(first ? QStringLiteral("QFrame { background: #2f5d8f; border-radius: 6px; }")
					 : QStringLiteral("QFrame { background: #3a3f46; border-radius: 6px; }"));
		auto *lay = new QHBoxLayout(box);
		lay->setContentsMargins(2, 2, 2, 2);
		lay->setSpacing(2);

		auto *prev = new QToolButton;
		prev->setText(QStringLiteral("◀"));
		prev->setAutoRaise(true);
		prev->setEnabled(i > 0);
		prev->setToolTip(QStringLiteral("Mover a la izquierda"));

		auto *name = new QLabel((first ? QStringLiteral("✓ ") : QString()) + versions_[i]->label);
		name->setStyleSheet(QStringLiteral("QLabel { color: #ffffff; font-weight: bold; background: transparent; }"));
		name->setToolTip(first ? QStringLiteral("Esta versión es la que sale al aire")
				       : QStringLiteral("Para proyectarla, pasala a la izquierda"));

		auto *next = new QToolButton;
		next->setText(QStringLiteral("▶"));
		next->setAutoRaise(true);
		next->setEnabled(i + 1 < versions_.size());
		next->setToolTip(QStringLiteral("Mover a la derecha"));

		const int idx = int(i);
		connect(prev, &QToolButton::clicked, this, [this, idx] { moveVersion(idx, -1); });
		connect(next, &QToolButton::clicked, this, [this, idx] { moveVersion(idx, +1); });

		lay->addWidget(prev);
		lay->addWidget(name, 1);
		lay->addWidget(next);
		versionLay_->addWidget(box);
	}
	versionLay_->addStretch(1);
}

void BibleWindow::moveVersion(int index, int delta)
{
	const int to = index + delta;
	if (index < 0 || to < 0 || size_t(index) >= versions_.size() || size_t(to) >= versions_.size())
		return;
	std::swap(versions_[size_t(index)], versions_[size_t(to)]);
	saveStore();
	rebuildVersionBar();
	if (index == 0 || to == 0)
		onLeftChanged(); // cambió la versión que sale al aire
}

// Cambió la versión de la izquierda: se rearma todo con el mismo libro, capítulo y versículos.
void BibleWindow::onLeftChanged()
{
	const std::vector<int> keep = lastValid_;
	buildBooks();
	navigateTo(curBook_, curChap_, keep.empty() ? std::vector<int>{1} : keep);
	refreshBar();
}

// ---------------------------------------------------------------------------
// Libros, capítulos y versículos
// ---------------------------------------------------------------------------

void BibleWindow::buildBooks()
{
	buildBooksFiltered(QString()); // Sin filtro, muestra todos
}

void BibleWindow::buildBooksFiltered(const QString &query)
{
	const bool was = updating_;
	updating_ = true;
	
	booksOld_->clear();
	booksNew_->clear();
	
	const bool isSearch = !query.isEmpty();
	
	for (int n = 1; n <= 66; n++) {
		QString name = bookName(n);
		
		if (isSearch) {
			QString normName = name.toLower();
			QString normQuery = query.toLower();
			
			// Si busca "1 corintios", ignorar el número y buscar solo "corintios"
			if (normQuery.startsWith("1 ") || normQuery.startsWith("2 ") || normQuery.startsWith("3 ")) {
				normQuery = normQuery.mid(2);
			}
			
			// Si el nombre del libro empieza con número, le quitamos el número para comparar
			if (normName.startsWith("1 ") || normName.startsWith("2 ") || normName.startsWith("3 ")) {
				normName = normName.mid(2);
			}
			
			if (!normName.contains(normQuery)) {
				continue;
			}
		}
		
		QListWidgetItem *it;
		if (n <= 39) {
			it = new QListWidgetItem(name, booksOld_);
		} else {
			it = new QListWidgetItem(name, booksNew_);
		}
		it->setData(Qt::UserRole, n);
	}
	
	updating_ = was;
}

void BibleWindow::navigateTo(int book, int chap, const std::vector<int> &verses)
{
	updating_ = true;
	curBook_ = std::clamp(book, 1, 66);
	curChap_ = chap;

	auto updateListSelection = [this](QListWidget *list) {
		for (int r = 0; r < list->count(); r++) {
			if (list->item(r)->data(Qt::UserRole).toInt() == curBook_) {
				list->setCurrentRow(r);
				list->scrollToItem(list->item(r), QAbstractItemView::PositionAtCenter);
				return true;
			}
		}
		return false;
	};

	if (!updateListSelection(booksOld_)) {
		updateListSelection(booksNew_);
	}

	fillChapters();
	fillVerseGrid();
	fillVerseList();

	for (int v : verses)
		selectVerseItem(v, true);
	lastValid_ = selectedVerses();
	if (!lastValid_.empty()) {
		for (int i = 0; i < verses_->count(); i++) {
			if (verses_->item(i)->data(Qt::UserRole).toInt() == lastValid_.front()) {
				verses_->scrollToItem(verses_->item(i), QAbstractItemView::PositionAtTop);
				break;
			}
		}
	}
	syncGrid();
	updating_ = false;
	updateTitle();
}

void BibleWindow::fillChapters()
{
	chapters_->clear();
	const BibleVersion *lv = left();
	if (!lv)
		return;
	auto b = lv->books.find(curBook_);
	if (b == lv->books.end() || b->second.chapters.empty())
		return;
	if (b->second.chapters.find(curChap_) == b->second.chapters.end())
		curChap_ = b->second.chapters.begin()->first;

	for (const auto &c : b->second.chapters) {
		auto *it = new QListWidgetItem(QString::number(c.first), chapters_);
		it->setTextAlignment(Qt::AlignCenter);
		it->setData(Qt::UserRole, c.first);
		if (c.first == curChap_) {
			it->setSelected(true);
			chapters_->scrollToItem(it);
		}
	}
}

void BibleWindow::fillVerseGrid()
{
	verseGrid_->clear();
	const BibleVersion *lv = left();
	if (!lv)
		return;
	auto b = lv->books.find(curBook_);
	if (b == lv->books.end())
		return;
	auto c = b->second.chapters.find(curChap_);
	if (c == b->second.chapters.end())
		return;
	for (const BibleVerse &v : c->second) {
		auto *it = new QListWidgetItem(QString::number(v.n), verseGrid_);
		it->setTextAlignment(Qt::AlignCenter);
		it->setData(Qt::UserRole, v.n);
	}
}

void BibleWindow::fillVerseList()
{
	verses_->clear();
	const BibleVersion *lv = left();
	if (!lv) {
		auto *h = new QListWidgetItem(QStringLiteral("No hay biblias cargadas. Tocá + (arriba a la derecha) y elegí "
							     "los archivos .xml o .txt."),
					      verses_);
		h->setFlags(Qt::NoItemFlags);
		return;
	}
	auto b = lv->books.find(curBook_);
	const std::vector<BibleVerse> *vec = nullptr;
	if (b != lv->books.end()) {
		auto c = b->second.chapters.find(curChap_);
		if (c != b->second.chapters.end())
			vec = &c->second;
	}
	if (!vec) {
		auto *h = new QListWidgetItem(QStringLiteral("Este capítulo no está en esta versión."), verses_);
		h->setFlags(Qt::NoItemFlags);
		return;
	}
	for (const BibleVerse &v : *vec) {
		auto *it = new QListWidgetItem(QStringLiteral("%1  %2").arg(v.n).arg(v.text), verses_);
		it->setData(Qt::UserRole, v.n);
	}
}

void BibleWindow::syncGrid()
{
	verseGrid_->clearSelection();
	if (lastValid_.empty())
		return;
	for (int i = 0; i < verseGrid_->count(); i++) {
		QListWidgetItem *it = verseGrid_->item(i);
		if (it->data(Qt::UserRole).toInt() == lastValid_.front()) {
			it->setSelected(true);
			verseGrid_->scrollToItem(it);
			break;
		}
	}
}

void BibleWindow::selectVerseItem(int verse, bool on)
{
	for (int i = 0; i < verses_->count(); i++) {
		QListWidgetItem *it = verses_->item(i);
		if ((it->flags() & Qt::ItemIsSelectable) && it->data(Qt::UserRole).toInt() == verse) {
			it->setSelected(on);
			return;
		}
	}
}

// Un solo versículo (clic en la cuadrícula de la tercera columna): se marca en la primera columna.
void BibleWindow::selectOnly(int verse)
{
	updating_ = true;
	verses_->clearSelection();
	selectVerseItem(verse, true);
	lastValid_ = selectedVerses();
	for (int i = 0; i < verses_->count(); i++) {
		if (verses_->item(i)->data(Qt::UserRole).toInt() == verse) {
			verses_->scrollToItem(verses_->item(i), QAbstractItemView::PositionAtTop);
			break;
		}
	}
	syncGrid();
	updating_ = false;
}

std::vector<int> BibleWindow::selectedVerses() const
{
	std::vector<int> out;
	const QList<QListWidgetItem *> sel = verses_->selectedItems();
	for (QListWidgetItem *it : sel) {
		const int v = it->data(Qt::UserRole).toInt();
		if (v > 0)
			out.push_back(v);
	}
	std::sort(out.begin(), out.end());
	return out;
}

void BibleWindow::onVerseSelectionChanged()
{
	if (updating_)
		return;

	const std::vector<int> cur = selectedVerses();
	if (cur.size() > size_t(kMaxSel)) {
		// Máximo 3 versículos: se vuelve a la selección anterior.
		updating_ = true;
		verses_->clearSelection();
		for (int v : lastValid_)
			selectVerseItem(v, true);
		updating_ = false;
		showStatus(QStringLiteral("Máximo %1 versículos a la vez").arg(kMaxSel));
		return;
	}
	lastValid_ = cur;
	syncGrid();
	verses_->viewport()->update(); // el delegate pinta azul lo seleccionado
}

void BibleWindow::updateTitle()
{
	setWindowTitle(QStringLiteral("Biblia — %1 %2").arg(bookName(curBook_)).arg(curChap_));
}

// ---------------------------------------------------------------------------
// Proyección, favoritos, historial y frecuentes
// ---------------------------------------------------------------------------

void BibleWindow::project()
{
	const BibleVersion *lv = left();
	if (!lv) {
		showStatus(QStringLiteral("No hay biblias cargadas"));
		return;
	}
	const std::vector<int> vs = selectedVerses();
	if (vs.empty()) {
		showStatus(QStringLiteral("Elegí un versículo"));
		return;
	}

	QStringList lines;
	std::vector<int> used;
	for (int v : vs) {
		const QString *t = bible_verse_text(*lv, curBook_, curChap_, v);
		if (!t)
			continue;
		lines << QStringLiteral("%1  %2").arg(v).arg(*t);
		used.push_back(v);
	}
	if (used.empty())
		return;

	// "Libro  capítulo", un renglón en blanco, y una línea por versículo ("número  versículo").
	const QString name = bookName(curBook_);
	const QString text = name + QStringLiteral("  ") + QString::number(curChap_) + QStringLiteral("\n\n") +
			     lines.join(QLatin1Char('\n'));
	const QString label = QStringLiteral("%1 %2:%3").arg(name).arg(curChap_).arg(compress(used));

	record(curBook_, curChap_, used);
	
	// Guardamos los versículos que se están proyectando y actualizamos el color
	projectingVerses_ = used;
	for (int i = 0; i < verses_->count(); i++) {
		QListWidgetItem *it = verses_->item(i);
		int v = it->data(Qt::UserRole).toInt();
		bool isProj = std::find(projectingVerses_.begin(), projectingVerses_.end(), v) != projectingVerses_.end();
		apply_verse_style(it, isProj);
	}
	
	emit projectRequested(text, label);
}

// Historial (un registro por versículo) y conteo de frecuentes.
void BibleWindow::record(int book, int chap, const std::vector<int> &verses)
{
	std::vector<Ref> nh;
	for (int v : verses)
		nh.push_back(Ref{book, chap, {v}});
	for (const Ref &old : history_) {
		bool dup = false;
		for (const Ref &n : nh)
			if (n.book == old.book && n.chap == old.chap && n.verses == old.verses)
				dup = true;
		if (!dup)
			nh.push_back(old);
	}
	if (nh.size() > size_t(kMaxHistory))
		nh.resize(size_t(kMaxHistory));
	history_ = std::move(nh);

	for (int v : verses)
		counts_[QStringLiteral("%1:%2:%3").arg(book).arg(chap).arg(v)]++;

	saveStore();
	refreshBar();
}

void BibleWindow::addFavorite()
{
	const std::vector<int> vs = selectedVerses();
	if (vs.empty()) {
		showStatus(QStringLiteral("Elegí un versículo para agregarlo a favoritos"));
		return;
	}
	for (const Ref &f : favorites_) {
		if (f.book == curBook_ && f.chap == curChap_ && f.verses == vs) {
			showStatus(QStringLiteral("Ya está en favoritos"));
			return;
		}
	}
	favorites_.push_back(Ref{curBook_, curChap_, vs});
	saveStore();
	refreshBar();
	showStatus(QStringLiteral("Agregado a favoritos: %1 %2:%3").arg(bookName(curBook_)).arg(curChap_).arg(compress(vs)));
}

void BibleWindow::removeFavorite(int index)
{
	if (index < 0 || size_t(index) >= favorites_.size())
		return;
	favorites_.erase(favorites_.begin() + index);
	saveStore();
	refreshBar();
}

// Contenido de cada pestaña de la barra: favoritos, historial o los 10 más proyectados.
auto BibleWindow::barItems(int tab) const -> std::vector<std::pair<Ref, QString>>
{
	std::vector<std::pair<Ref, QString>> out;
	if (tab == 0) {
		for (const Ref &r : favorites_)
			out.emplace_back(r, QString());
	} else if (tab == 1) {
		for (const Ref &r : history_)
			out.emplace_back(r, QString());
	} else {
		std::vector<std::pair<QString, int>> all(counts_.begin(), counts_.end());
		std::stable_sort(all.begin(), all.end(),
				 [](const std::pair<QString, int> &a, const std::pair<QString, int> &b) { return a.second > b.second; });
		for (size_t i = 0; i < all.size() && i < size_t(kTopFrequent); i++) {
			const QStringList p = all[i].first.split(QLatin1Char(':'));
			if (p.size() != 3)
				continue;
			out.emplace_back(Ref{p[0].toInt(), p[1].toInt(), {p[2].toInt()}},
					 QStringLiteral("\u00D7%1").arg(all[i].second));
		}
	}
	return out;
}

void BibleWindow::refreshBar()
{
	barList_->clear();
	barRefs_.clear();

	const int tab = barTabs_->currentIndex();
	for (const auto &entry : barItems(tab)) {
		const Ref &r = entry.first;
		QString txt = bookName(r.book) + QLatin1Char('\n') + QStringLiteral("%1:%2").arg(r.chap).arg(compress(r.verses));
		if (!entry.second.isEmpty())
			txt += QStringLiteral("  ") + entry.second;
		auto *it = new QListWidgetItem(txt, barList_);
		it->setTextAlignment(Qt::AlignCenter);
		it->setSizeHint(QSize(132, 48));
		it->setData(Qt::UserRole, int(barRefs_.size()));
		barRefs_.push_back(r);
	}

	if (barList_->count() == 0) {
		const QString hint = tab == 0 ? QStringLiteral("(vacío: F6 agrega los versículos elegidos)")
					      : tab == 1 ? QStringLiteral("(vacío: acá quedan los versículos proyectados)")
							 : QStringLiteral("(vacío: acá quedan los 10 más proyectados)");
		auto *h = new QListWidgetItem(hint, barList_);
		h->setFlags(Qt::NoItemFlags);
	}
}

// Clic derecho sobre la barra: solo en Favoritos, con las opciones para eliminar.
void BibleWindow::onBarContextMenu(const QPoint &pos)
{
	if (barTabs_->currentIndex() != 0)
		return;

	QListWidgetItem *it = barList_->itemAt(pos);
	const bool onChip = it && it->data(Qt::UserRole).isValid();

	QMenu menu(this);
	QAction *delOne = onChip ? menu.addAction(QStringLiteral("Eliminar favorito")) : nullptr;
	QAction *delAll = menu.addAction(QStringLiteral("Eliminar todos los favoritos"));

	QAction *a = menu.exec(barList_->viewport()->mapToGlobal(pos));
	if (!a)
		return;
	if (a == delOne) {
		removeFavorite(it->data(Qt::UserRole).toInt());
	} else if (a == delAll) {
		favorites_.clear();
		saveStore();
		refreshBar();
	}
}

// Clic derecho sobre los versículos de la primera columna.
void BibleWindow::onVerseContextMenu(const QPoint &pos)
{
	QMenu menu(this);
	QAction *proj = menu.addAction(QStringLiteral("Proyectar (Enter)"));
	QAction *fav = menu.addAction(QStringLiteral("Agregar a favoritos (F6)"));
	QAction *changeFont = menu.addAction("Cambiar tamaño de fuente...");
	
	QAction *a = menu.exec(verses_->viewport()->mapToGlobal(pos));
	if (a == proj)
		project();
	else if (a == fav)
		addFavorite();
	else if (a == changeFont) {
		bool ok;
		int newSize = QInputDialog::getInt(this, "Tamaño de fuente (Versículos)", 
			"Tamaño (puntos):", fontVerses_, 6, 30, 1, &ok);
		if (ok) {
			fontVerses_ = newSize;
			set_list_font(verses_, newSize);
			saveStore();
		}
	}
}

void BibleWindow::onBooksContextMenu(const QPoint &pos, QListWidget *list)
{
	QMenu menu(this);
	QAction *changeFont = menu.addAction("Cambiar tamaño de fuente...");
	QAction *a = menu.exec(list->viewport()->mapToGlobal(pos));
	if (a == changeFont) {
		bool ok;
		int newSize = QInputDialog::getInt(this, "Tamaño de fuente (Libros)", 
			"Tamaño (puntos):", fontBooks_, 6, 30, 1, &ok);
		if (ok) {
			fontBooks_ = newSize;
			set_list_font(booksOld_, newSize);
			set_list_font(booksNew_, newSize); // Ambas listas comparten el mismo tamaño
			saveStore(); // Guardamos el cambio
		}
	}
}

void BibleWindow::onGridContextMenu(const QPoint &pos, QListWidget *list)
{
	QMenu menu(this);
	QAction *changeFont = menu.addAction("Cambiar tamaño de fuente...");
	QAction *a = menu.exec(list->viewport()->mapToGlobal(pos));
	if (a == changeFont) {
		bool ok;
		int newSize = QInputDialog::getInt(this, "Tamaño de fuente (Capítulos/Versículos)", 
			"Tamaño (puntos):", fontGrid_, 6, 30, 1, &ok);
		if (ok) {
			fontGrid_ = newSize;
			set_list_font(chapters_, newSize);
			set_list_font(verseGrid_, newSize); // Comparten el mismo tamaño
			saveStore();
		}
	}
}

bool BibleWindow::eventFilter(QObject *obj, QEvent *ev)
{
	if (ev->type() == QEvent::WindowStateChange) {
		bool nowFullscreen = isMaximized() || isFullScreen();
		if (nowFullscreen != isFullscreen_) {
			isFullscreen_ = nowFullscreen;
			updateLayoutForMode(isFullscreen_);
		}
	}
	return QWidget::eventFilter(obj, ev);
}

void BibleWindow::updateLayoutForMode(bool fullscreen)
{
	if (fullscreen) {
		// Modo 4 columnas: Antiguo y Nuevo Testamento lado a lado (horizontal)
		booksSplit_->setOrientation(Qt::Horizontal);
		booksOld_->setVisible(true);
		booksNew_->setVisible(true);
		split_->setSizes({300, 400, 300}); // Ajustamos el ancho de la columna central
	} else {
		// Modo 3 columnas: Antiguo y Nuevo Testamento apilados (vertical)
		booksSplit_->setOrientation(Qt::Vertical);
		booksOld_->setVisible(true);
		booksNew_->setVisible(true); // Lo dejamos visible pero apilado
		booksSplit_->setSizes({300, 300}); // Mitad y mitad
		split_->setSizes({520, 210, 400});
	}
}

// ---------------------------------------------------------------------------
// Control remoto (celular)
// ---------------------------------------------------------------------------

void BibleWindow::prepare()
{
	ensureLoaded();
}

QJsonObject BibleWindow::remoteState() const
{
	const std::vector<int> sel = selectedVerses();
	QJsonArray arr;
	for (int v : sel)
		arr.append(v);

	QJsonObject o;
	o.insert(QStringLiteral("version"), left() ? left()->label : QString());
	o.insert(QStringLiteral("book"), curBook_);
	o.insert(QStringLiteral("bookName"), bookName(curBook_));
	o.insert(QStringLiteral("chap"), curChap_);
	o.insert(QStringLiteral("sel"), arr);
	o.insert(QStringLiteral("cite"), sel.empty() ? QString()
						     : QStringLiteral("%1 %2:%3").arg(bookName(curBook_)).arg(curChap_).arg(compress(sel)));
	return o;
}

QJsonObject BibleWindow::remoteBooks() const
{
	QJsonArray arr;
	for (int n = 1; n <= 66; n++) {
		QJsonObject b;
		b.insert(QStringLiteral("n"), n);
		b.insert(QStringLiteral("name"), bookName(n));
		arr.append(b);
	}
	QJsonObject o;
	o.insert(QStringLiteral("books"), arr);
	return o;
}

QJsonObject BibleWindow::remoteChapters(int book) const
{
	QJsonArray arr;
	if (const BibleVersion *lv = left()) {
		auto b = lv->books.find(book);
		if (b != lv->books.end())
			for (const auto &c : b->second.chapters)
				arr.append(c.first);
	}
	QJsonObject o;
	o.insert(QStringLiteral("chapters"), arr);
	return o;
}

QJsonObject BibleWindow::remoteVerses(int book, int chap) const
{
	QJsonArray arr;
	if (const BibleVersion *lv = left()) {
		auto b = lv->books.find(book);
		if (b != lv->books.end()) {
			auto c = b->second.chapters.find(chap);
			if (c != b->second.chapters.end()) {
				for (const BibleVerse &v : c->second) {
					QJsonObject vo;
					vo.insert(QStringLiteral("n"), v.n);
					vo.insert(QStringLiteral("t"), v.text);
					arr.append(vo);
				}
			}
		}
	}
	QJsonObject o;
	o.insert(QStringLiteral("verses"), arr);
	return o;
}

QJsonObject BibleWindow::remoteBar(int tab) const
{
	QJsonArray arr;
	for (const auto &entry : barItems(std::clamp(tab, 0, 2))) {
		const Ref &r = entry.first;
		QJsonArray vs;
		for (int v : r.verses)
			vs.append(v);
		QJsonObject it;
		it.insert(QStringLiteral("b"), r.book);
		it.insert(QStringLiteral("c"), r.chap);
		it.insert(QStringLiteral("v"), vs);
		it.insert(QStringLiteral("book"), bookName(r.book));
		it.insert(QStringLiteral("ref"), QStringLiteral("%1:%2").arg(r.chap).arg(compress(r.verses)));
		it.insert(QStringLiteral("extra"), entry.second);
		arr.append(it);
	}
	QJsonObject o;
	o.insert(QStringLiteral("items"), arr);
	return o;
}

void BibleWindow::remoteLoad(int book, int chap, const std::vector<int> &verses)
{
	if (book < 1 || book > 66 || chap < 1)
		return;
	std::vector<int> v = verses;
	std::sort(v.begin(), v.end());
	v.erase(std::unique(v.begin(), v.end()), v.end());
	if (v.size() > size_t(kMaxSel))
		v.resize(size_t(kMaxSel));
	navigateTo(book, chap, v);
}

void BibleWindow::remoteProject()
{
	project();
}

void BibleWindow::remoteHide()
{
	emit hideRequested();
}

void BibleWindow::remoteAddFavorite()
{
	addFavorite();
}

// Mueve la selección al versículo siguiente o anterior (pasando de capítulo o de libro si hace falta) y lo proyecta.
void BibleWindow::remoteStep(int delta)
{
	const BibleVersion *lv = left();
	if (!lv || delta == 0)
		return;

	auto bit = lv->books.find(curBook_);
	if (bit == lv->books.end())
		return;
	auto cit = bit->second.chapters.find(curChap_);
	if (cit == bit->second.chapters.end() || cit->second.empty())
		return;

	int book = curBook_, chap = curChap_, verse = 0;
	const std::vector<int> sel = selectedVerses();
	const std::vector<BibleVerse> &vec = cit->second;

	if (sel.empty()) {
		verse = vec.front().n; // sin selección: se parte del primer versículo del capítulo
	} else {
		const int from = delta > 0 ? sel.back() : sel.front();
		size_t idx = 0;
		for (size_t i = 0; i < vec.size(); i++)
			if (vec[i].n == from)
				idx = i;

		if (delta > 0) {
			if (idx + 1 < vec.size()) {
				verse = vec[idx + 1].n;
			} else if (std::next(cit) != bit->second.chapters.end()) {
				++cit;
				chap = cit->first;
				verse = cit->second.front().n;
			} else {
				++bit;
				while (bit != lv->books.end() && bit->second.chapters.empty())
					++bit;
				if (bit == lv->books.end()) {
					showStatus(QStringLiteral("Es el último versículo de la Biblia"));
					return;
				}
				book = bit->first;
				chap = bit->second.chapters.begin()->first;
				verse = bit->second.chapters.begin()->second.front().n;
			}
		} else {
			if (idx > 0) {
				verse = vec[idx - 1].n;
			} else if (cit != bit->second.chapters.begin()) {
				--cit;
				chap = cit->first;
				verse = cit->second.back().n;
			} else {
				if (bit == lv->books.begin()) {
					showStatus(QStringLiteral("Es el primer versículo de la Biblia"));
					return;
				}
				--bit;
				auto last = std::prev(bit->second.chapters.end());
				book = bit->first;
				chap = last->first;
				verse = last->second.back().n;
			}
		}
	}

	navigateTo(book, chap, {verse});
	project();
}
