#include "notif-dock.hpp"
#include "notif-filter.hpp"
#include "bible-server.hpp"
#include "bible-window.hpp"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QUrl>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QEvent>
#include <QObject>
#include <functional>

#include <algorithm>
#include <cstring>

namespace {
class ResizeWatcher : public QObject {
public:
	ResizeWatcher(QObject *parent, std::function<void()> fn)
		: QObject(parent), fn_(std::move(fn)) {}
protected:
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (ev->type() == QEvent::Resize) fn_();
		return QObject::eventFilter(obj, ev);
	}
private:
	std::function<void()> fn_;
};
constexpr int kMaxChars = 5000;
constexpr int kSlots = 9;
constexpr int kMaxVisibleLines = 12; // alto máximo de cada casillero de frase, en renglones

// Estilo de los botones numerados: oscuros, número blanco; el elegido se ve azulado.
// Si el casillero está vacío, el número se ve atenuado.
QString num_style(bool filled)
{
	return QStringLiteral("QPushButton { background: #1e1e1e; color: %1; border: none; border-radius: 6px;"
			      " font-weight: bold; font-size: 16px; padding: 0px; }"
			      "QPushButton:hover { background: #2c2c2c; }"
			      "QPushButton:checked { background: #4a4f7a; color: #d6d9ff; border: 1px solid #6b719f; }")
		.arg(filled ? QStringLiteral("#ffffff") : QStringLiteral("#7d7d7d"));
}

QColor to_qcolor(uint32_t c)
{
	return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF), int((c >> 24) & 0xFF));
}

uint32_t to_abgr(const QColor &q)
{
	return (uint32_t(q.alpha()) << 24) | (uint32_t(q.blue()) << 16) | (uint32_t(q.green()) << 8) |
	       uint32_t(q.red());
}

QByteArray phrase_key(int n)
{
	return QStringLiteral("phrase_%1").arg(n).toUtf8();
}

struct Found {
	std::vector<std::pair<QString, OBSWeakSource>> items;
};

void enum_filter_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	if (std::strcmp(obs_source_get_unversioned_id(child), NOTIF_FILTER_ID) != 0)
		return;
	auto *found = static_cast<Found *>(param);
	const QString label = QString::fromUtf8(obs_source_get_name(parent)) + QStringLiteral("  ›  ") +
			      QString::fromUtf8(obs_source_get_name(child));
	found->items.emplace_back(label, OBSGetWeakRef(child));
}

bool enum_source_cb(void *param, obs_source_t *src)
{
	obs_source_enum_filters(src, enum_filter_cb, param);
	return true;
}

} // namespace

// Los ajustes son generales: se guardan una sola vez y valen para todas las fuentes.
template<class F> void NotifDock::push(F fn)
{
	if (loading_)
		return;
	OBSDataAutoRelease d = obs_data_create();
	fn(d.Get());
	notif_config_update(d);
}

QSlider *NotifDock::addSlider(QFormLayout *form, const QString &label, int min, int max, const QString &suffix,
			      QLabel **valueLabel)
{
	auto *row = new QWidget;
	auto *lay = new QHBoxLayout(row);
	lay->setContentsMargins(0, 0, 0, 0);
	auto *slider = new QSlider(Qt::Horizontal);
	slider->setRange(min, max);
	auto *val = new QLabel;
	val->setMinimumWidth(48);
	lay->addWidget(slider, 1);
	lay->addWidget(val);
	connect(slider, &QSlider::valueChanged, this,
		[val, suffix](int v) { val->setText(QString::number(v) + suffix); });
	val->setText(QString::number(slider->value()) + suffix);
	form->addRow(label, row);
	if (valueLabel)
		*valueLabel = val;
	return slider;
}

void NotifDock::setColorButton(QPushButton *b, uint32_t abgr)
{
	const QColor c = to_qcolor(abgr);
	const QString txt = c.lightness() > 128 ? "#000" : "#fff";
	b->setStyleSheet(QString("background-color: rgba(%1,%2,%3,%4); color: %5;")
				 .arg(c.red())
				 .arg(c.green())
				 .arg(c.blue())
				 .arg(c.alpha())
				 .arg(txt));
	b->setText(c.name(QColor::HexArgb));
}

NotifDock::NotifDock(QWidget *parent) : QWidget(parent)
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(0, 0, 0, 0);

	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // Sin scroll horizontal
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);    // Scroll vertical solo si hace falta
	scroll->setStyleSheet("QScrollArea { border: none; }");

	auto *inner = new QWidget;
	auto *outer = new QVBoxLayout(inner);
	outer->setContentsMargins(6, 6, 16, 6); // Margen derecho extra para que la barra no pise los botones
scroll->setWidget(inner);
mainLayout->addWidget(scroll);

// Selector de filtro (uno por cada fuente donde lo hayas agregado)
	
	target_ = new QComboBox;
	target_->setPlaceholderText("Agregá el filtro a una fuente");
	target_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	target_->setMinimumContentsLength(12);
	auto *refreshBtn = new QPushButton("Actualizar");
	auto *topRow = new QHBoxLayout;
	topRow->addWidget(target_, 1);
	topRow->addWidget(refreshBtn);
	outer->addWidget(new QLabel("Fuente con el filtro:"));
	outer->addLayout(topRow);

	// --- Zona rápida: resumen, 9 botones, mostrar / ocultar, editar lista ---
	quick_ = new QWidget;
	auto *quickLay = new QVBoxLayout(quick_);
	quickLay->setContentsMargins(0, 0, 0, 0);

	summary_ = new QLabel("Sin filtro disponible");
	summary_->setWordWrap(true);
	summary_->setStyleSheet("background: #3a3f5c; color: #e6e8f5; border-radius: 10px;"
				" padding: 6px 12px; font-size: 14px;");
	summary_->setMinimumHeight(44);
	summary_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	quickLay->addWidget(summary_);

	// Una sola fila: [X] [1] [2] ... [9]. La X oculta la notificación.
	auto *numRow = new QHBoxLayout;
	numRow->setSpacing(4);

	auto *xBtn = new QPushButton("X");
	xBtn->setMinimumHeight(36);
	xBtn->setMinimumWidth(30);
	xBtn->setToolTip("Ocultar la notificación ahora");
	xBtn->setStyleSheet("QPushButton { background: #7a2f2f; color: #ffffff; border: none; border-radius: 6px;"
			    " font-size: 16px; font-weight: bold; padding: 0px; }"
			    "QPushButton:hover { background: #a03a3a; }");
	numRow->addWidget(xBtn, 2);

	group_ = new QButtonGroup(this);
	QButtonGroup *group = group_;
	group->setExclusive(true);
	for (int i = 0; i < kSlots; i++) {
		auto *b = new QPushButton(QString::number(i + 1));
		b->setCheckable(true);
		b->setMinimumHeight(36);
		b->setStyleSheet(num_style(false));
		group->addButton(b, i + 1);
		numRow->addWidget(b, 2);
		numBtns_[i] = b;
	}
	numBtns_[0]->setChecked(true);
	quickLay->addLayout(numRow);

	auto *showBtn = new QPushButton("Mostrar notificación");
	auto *hideBtn = new QPushButton("Ocultar");
	auto *showRow = new QHBoxLayout;
	showRow->addWidget(showBtn, 3);
	showRow->addWidget(hideBtn, 1);
	quickLay->addLayout(showRow);

	auto *listBtn = new QPushButton("Editar lista de frases…");
	auto *gearBtn = new QPushButton(QStringLiteral("\u2699"));
gearBtn->setToolTip("Ajustes de la notificación");
gearBtn->setFixedSize(36, 36); // Le damos un tamaño cuadrado perfecto
// Agregamos padding: 0 y centramos el texto para que no se corte
gearBtn->setStyleSheet("font-size: 20px; padding: 0px; text-align: center;"); 
	auto *listRow = new QHBoxLayout;
	listRow->addWidget(listBtn, 1);
	listRow->addWidget(gearBtn);
	quickLay->addLayout(listRow);

	auto *bibleBtn = new QPushButton("Abrir Biblia");
	bibleBtn->setToolTip("Abre la ventana de la Biblia (F5 por defecto)");
	quickLay->addWidget(bibleBtn);

	outer->addWidget(quick_);

	// --- Panel de ajustes con scroll ---
	panel_ = new QWidget;
	auto *form = new QFormLayout(panel_);
	form->setRowWrapPolicy(QFormLayout::WrapAllRows); // etiqueta arriba, control abajo: más angosto

	direction_ = new QComboBox;
	direction_->addItems({"Desde arriba", "Desde la izquierda", "Desde la derecha"});
	form->addRow("Dirección:", direction_);

	animInSec_ = new QDoubleSpinBox;
animInSec_->setRange(0.01, 5.0);     // Permite bajar hasta 0.01s
animInSec_->setSingleStep(0.01);     // Salta de centésima en centésima
animInSec_->setDecimals(2);          // Muestra 0.08, 0.10, etc.
animInSec_->setSuffix(" s");
form->addRow("Tiempo de aparición:", animInSec_);

	linkAnim_ = new QCheckBox("Usar el mismo tiempo para salir");
	linkAnim_->setChecked(true);
	form->addRow(linkAnim_);

	animOutSec_ = new QDoubleSpinBox;
	animOutSec_->setRange(0.1, 5.0);
	animOutSec_->setSingleStep(0.1);
	animOutSec_->setSuffix(" s");
	form->addRow("Tiempo de desaparición:", animOutSec_);

	modeTime_ = new QRadioButton("Función por tiempo");
	modeAction_ = new QRadioButton("Función por acción");
	modeTime_->setChecked(true);
	form->addRow("Modo:", modeTime_);
	form->addRow(modeAction_);

	holdSec_ = new QDoubleSpinBox;
	holdSec_->setRange(0.5, 120.0);
	holdSec_->setSingleStep(0.5);
	holdSec_->setSuffix(" s");
	form->addRow("Tiempo visible:", holdSec_);

	blur_ = addSlider(form, "Desenfoque final:", 0, 100, "", nullptr);

	font_ = new QComboBox;
	font_->setMaxVisibleItems(15);
	font_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	font_->setMinimumContentsLength(12);
	font_->addItems(QFontDatabase::families());
	auto *addFontBtn = new QPushButton("Agregar fuente…");
	form->addRow("Fuente de letra:", font_);
	form->addRow(addFontBtn);

	fontSize_ = new QSpinBox;
	fontSize_->setRange(8, 300);
	fontSize_->setSuffix(" px");
	form->addRow("Tamaño de letra:", fontSize_);

	bold_ = new QCheckBox("Negrita");
	form->addRow(bold_);

	bgBtn_ = new QPushButton;
	fgBtn_ = new QPushButton;
	form->addRow("Color de fondo:", bgBtn_);
	form->addRow("Color de letra:", fgBtn_);

	shadow_ = new QCheckBox("Activar sombra");
	form->addRow(shadow_);
	shAmount_ = addSlider(form, "Cantidad de sombra:", 0, 100, " %", nullptr);
	shAngle_ = addSlider(form, "Ángulo de la sombra:", 0, 360, "°", nullptr);
	shAngle_->setToolTip("0 = derecha, 90 = abajo");
	shLen_ = addSlider(form, "Largo de la sombra:", 0, 100, " px", nullptr);
	shBlur_ = addSlider(form, "Difuminado de la sombra:", 0, 100, " px", nullptr);

	// Ventana de ajustes: no bloquea OBS y se puede dejar abierta mientras trabajás.
	settingsWin_ = new QDialog(this);
settingsWin_->setWindowTitle("Ajustes de la notificación");
settingsWin_->resize(340, 640);
settingsWin_->setMinimumWidth(300);
auto *winLay = new QVBoxLayout(settingsWin_);

auto *settingsScroll = new QScrollArea; 
settingsScroll->setWidgetResizable(true);
settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
settingsScroll->setWidget(panel_);
winLay->addWidget(settingsScroll);

	// --- Conexiones -------------------------------------------------------
	connect(refreshBtn, &QPushButton::clicked, this, [this] { refresh(); });
	connect(showBtn, &QPushButton::clicked, this, [this] { showSelected(); });
	connect(xBtn, &QPushButton::clicked, this, [this] { callProc("hide"); });
	connect(hideBtn, &QPushButton::clicked, this, [this] { callProc("hide"); });
	connect(listBtn, &QPushButton::clicked, this, [this] { openListDialog(); });
	connect(bibleBtn, &QPushButton::clicked, this, [this] { openBible(); });

// --- Menú contextual para el texto del resumen (clic derecho) ---
summary_->setContextMenuPolicy(Qt::CustomContextMenu);
connect(summary_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
	QMenu menu(this);
	QAction *actText = menu.addAction("Editar texto");
	QAction *actBg = menu.addAction("Editar color de fondo");
	QAction *actFg = menu.addAction("Editar color de letra"); // <--- Nueva opción

	QAction *selectedAct = menu.exec(summary_->mapToGlobal(pos));
	if (selectedAct == actText) {
		openListDialog();      // Abre la ventana con las 9 frases
	} else if (selectedAct == actBg) {
		bgBtn_->click();       // Abre la paleta de colores de fondo
	} else if (selectedAct == actFg) {
		fgBtn_->click();       // Abre la paleta de colores de letra
	}
});

connect(gearBtn, &QPushButton::clicked, this, [this] {
		settingsWin_->show();
		settingsWin_->raise();
		settingsWin_->activateWindow();
	});

	connect(group, &QButtonGroup::idClicked, this, [this](int id) {
		selected_ = id;
		bibleActive_ = false; // al elegir un botón del 1 al 9 vuelve a mandar la frase de la lista
		push([&](obs_data_t *d) { obs_data_set_int(d, "phrase_sel", id); });
		updateSummary();
	});

	connect(direction_, &QComboBox::currentIndexChanged, this,
		[this](int i) { push([&](obs_data_t *d) { obs_data_set_int(d, "direction", i); }); });

	// Aparición: si "link_anim" está tildado, también actualiza la salida.
	connect(animInSec_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		{
			QSignalBlocker b(animOutSec_);
			if (linkAnim_->isChecked())
				animOutSec_->setValue(v);
		}
		push([&](obs_data_t *d) {
			obs_data_set_double(d, "anim_in_sec", v);
			if (linkAnim_->isChecked())
				obs_data_set_double(d, "anim_out_sec", v);
		});
	});
	connect(animOutSec_, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { push([&](obs_data_t *d) { obs_data_set_double(d, "anim_out_sec", v); }); });
	connect(linkAnim_, &QCheckBox::toggled, this, [this](bool on) {
		animOutSec_->setEnabled(!on);
		if (on) {
			QSignalBlocker b(animOutSec_);
			animOutSec_->setValue(animInSec_->value());
			push([&](obs_data_t *d) {
				obs_data_set_bool(d, "link_anim", true);
				obs_data_set_double(d, "anim_out_sec", animInSec_->value());
			});
		} else {
			push([&](obs_data_t *d) { obs_data_set_bool(d, "link_anim", false); });
		}
	});

	connect(holdSec_, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { push([&](obs_data_t *d) { obs_data_set_double(d, "hold_sec", v); }); });
	connect(modeTime_, &QRadioButton::toggled, this, [this](bool on) {
		holdSec_->setEnabled(on);
		if (on)
			push([&](obs_data_t *d) { obs_data_set_int(d, "mode", 0); });
	});
	connect(modeAction_, &QRadioButton::toggled, this, [this](bool on) {
		if (on)
			push([&](obs_data_t *d) { obs_data_set_int(d, "mode", 1); });
	});
	connect(blur_, &QSlider::valueChanged, this,
		[this](int v) { push([&](obs_data_t *d) { obs_data_set_double(d, "blur_max", v); }); });

	connect(font_, &QComboBox::currentTextChanged, this, [this](const QString &fam) {
		push([&](obs_data_t *d) {
			obs_data_set_string(d, "font_family", fam.toUtf8().constData());
			obs_data_set_string(d, "font_file", ""); // fuente del sistema
		});
	});
	connect(addFontBtn, &QPushButton::clicked, this, [this] {
		const QString path = QFileDialog::getOpenFileName(this, "Agregar fuente", QString(),
								  "Fuentes (*.ttf *.otf)");
		if (path.isEmpty())
			return;
		const int id = QFontDatabase::addApplicationFont(path);
		const QStringList fams = QFontDatabase::applicationFontFamilies(id);
		if (fams.isEmpty())
			return;
		{
			QSignalBlocker b(font_);
			if (font_->findText(fams.first()) < 0)
				font_->addItem(fams.first());
			font_->setCurrentText(fams.first());
		}
		push([&](obs_data_t *d) {
			obs_data_set_string(d, "font_family", fams.first().toUtf8().constData());
			obs_data_set_string(d, "font_file", path.toUtf8().constData());
		});
	});

	connect(fontSize_, &QSpinBox::valueChanged, this,
		[this](int v) { push([&](obs_data_t *d) { obs_data_set_int(d, "font_size", v); }); });
	connect(bold_, &QCheckBox::toggled, this,
		[this](bool v) { push([&](obs_data_t *d) { obs_data_set_bool(d, "bold", v); }); });

	connect(bgBtn_, &QPushButton::clicked, this, [this] {
		const QColor c = QColorDialog::getColor(to_qcolor(bgColor_), this, "Color de fondo",
							QColorDialog::ShowAlphaChannel);
		if (!c.isValid())
			return;
		bgColor_ = to_abgr(c);
		setColorButton(bgBtn_, bgColor_);
		push([&](obs_data_t *d) { obs_data_set_int(d, "bg_color", bgColor_); });
	});
	connect(fgBtn_, &QPushButton::clicked, this, [this] {
		const QColor c = QColorDialog::getColor(to_qcolor(fgColor_), this, "Color de letra",
							QColorDialog::ShowAlphaChannel);
		if (!c.isValid())
			return;
		fgColor_ = to_abgr(c);
		setColorButton(fgBtn_, fgColor_);
		push([&](obs_data_t *d) { obs_data_set_int(d, "text_color", fgColor_); });
	});

	connect(shadow_, &QCheckBox::toggled, this, [this](bool v) {
		shAmount_->setEnabled(v);
		shAngle_->setEnabled(v);
		shLen_->setEnabled(v);
		shBlur_->setEnabled(v);
		push([&](obs_data_t *d) { obs_data_set_bool(d, "shadow", v); });
	});
	connect(shAmount_, &QSlider::valueChanged, this,
		[this](int v) { push([&](obs_data_t *d) { obs_data_set_int(d, "shadow_amount", v); }); });
	connect(shAngle_, &QSlider::valueChanged, this,
		[this](int v) { push([&](obs_data_t *d) { obs_data_set_int(d, "shadow_angle", v); }); });
	connect(shLen_, &QSlider::valueChanged, this,
		[this](int v) { push([&](obs_data_t *d) { obs_data_set_int(d, "shadow_len", v); }); });
	connect(shBlur_, &QSlider::valueChanged, this,
		[this](int v) { push([&](obs_data_t *d) { obs_data_set_int(d, "shadow_blur", v); }); });

	// Busca filtros nuevos cada 2 segundos (solo reconstruye la lista si cambió).
	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
	timer_->start(2000);

	// Control remoto de la Biblia desde el celular (puerto 8081; el de letras usa el 8080).
	server_ = new BibleServer(this);
	server_->setProvider([this]() -> BibleWindow * {
		ensureBible();
		bible_->prepare();
		return bible_;
	});
	server_->startServer(8081);

	loadFromConfig();
	refresh();
}

OBSSource NotifDock::current()
{
	const int i = target_->currentIndex();
	if (i < 0 || i >= int(items_.size()))
		return OBSSource();
	return OBSGetStrongRef(items_[size_t(i)].second);
}

// Texto guardado en el casillero n (1 a 9). Es general: no depende de la fuente elegida.
QString NotifDock::phrase(int n)
{
	QString out;
	notif_config_read([&](obs_data_t *d) { out = QString::fromUtf8(obs_data_get_string(d, phrase_key(n).constData())); });
	return out;
}

// Actualiza el resumen de arriba y el aspecto de los 9 botones.
void NotifDock::updateSummary()
{
	QString texts[kSlots];
	notif_config_read([&](obs_data_t *d) {
		for (int i = 0; i < kSlots; i++)
			texts[i] = QString::fromUtf8(obs_data_get_string(d, phrase_key(i + 1).constData()));
	});

	for (int i = 1; i <= kSlots; i++) {
		const QString &t = texts[i - 1];
		const bool filled = !t.trimmed().isEmpty();
		numBtns_[i - 1]->setStyleSheet(num_style(filled));
		numBtns_[i - 1]->setToolTip(filled ? t.left(300) : QStringLiteral("(vacía)"));
	}

	if (bibleActive_) {
		QString cite = bibleLabel_.isEmpty() ? QString("Biblia") : bibleLabel_;
		summary_->setText(QString("0 — %1").arg(cite));
		return;
	}

	QString t = texts[selected_ - 1].simplified();
	if (t.isEmpty()) {
		summary_->setText(QString("%1 — (vacía)").arg(selected_));
	} else {
		if (t.size() > 70)
			t = t.left(70) + QStringLiteral("…");
		summary_->setText(QString("%1 — %2").arg(selected_).arg(t));
	}
}

// Ventana con los 9 casilleros para escribir las frases.
void NotifDock::openListDialog()
{
	QDialog dlg(this);
	dlg.setWindowTitle("Lista de frases");
	dlg.resize(520, 620);
	auto *lay = new QVBoxLayout(&dlg);

	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	auto *inner = new QWidget;
	auto *form = new QFormLayout(inner);

	QPlainTextEdit *edits[kSlots] = {};
for (int i = 0; i < kSlots; i++) {
	auto *e = new QPlainTextEdit;
	e->setPlainText(phrase(i + 1));
	e->setPlaceholderText(QString("Frase %1 (máximo %2 caracteres)").arg(i + 1).arg(kMaxChars));
	e->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); // aparece solo si el texto pasa los 12 renglones
	e->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	e->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	// Ajusta la altura de la casilla al alto del texto, con un máximo de 12 renglones visibles.
	auto fitHeight = [e] {
		e->document()->setTextWidth(e->viewport()->width());
		// En QPlainTextEdit, size().height() son líneas visuales (ya con el ajuste de renglón), no píxeles.
		const double lines = e->document()->size().height();
		const double shown = std::min(lines, double(kMaxVisibleLines));
		const int h = int(shown * e->fontMetrics().lineSpacing() + 2 * e->document()->documentMargin()) +
			      2 * e->frameWidth() + 4;
		e->setFixedHeight(std::max(72, h));
	};

	connect(e, &QPlainTextEdit::textChanged, e, [e, fitHeight] {
		QString t = e->toPlainText();
		if (t.size() > kMaxChars) {
			QSignalBlocker b(e);
			t.truncate(kMaxChars);
			e->setPlainText(t);
			e->moveCursor(QTextCursor::End);
		}
		fitHeight();
	});

	// Al cambiar el ancho de la ventana, se recalcula el alto (por el word wrap).
	e->installEventFilter(new ResizeWatcher(e, fitHeight));

	auto *num = new QLabel(QString("<b>%1</b>").arg(i + 1));
	num->setAlignment(Qt::AlignTop | Qt::AlignRight);
	form->addRow(num, e);
	edits[i] = e;

	// Primera pasada: cuando ya tiene ancho asignado.
	QTimer::singleShot(0, e, fitHeight);
}
	scroll->setWidget(inner);
	lay->addWidget(scroll, 1);

	auto *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	lay->addWidget(box);

	if (dlg.exec() != QDialog::Accepted)
		return;

	QStringList texts;
	for (int i = 0; i < kSlots; i++)
		texts << edits[i]->toPlainText().left(kMaxChars);

	push([&](obs_data_t *d) {
		for (int i = 0; i < kSlots; i++)
			obs_data_set_string(d, phrase_key(i + 1).constData(), texts[i].toUtf8().constData());
	});
	updateSummary();
}

// Guarda la frase elegida como texto actual y la muestra en la fuente elegida.
void NotifDock::showSelected()
{
	if (!current())
		return;

	// Casilla 0 (Biblia): se vuelve a mostrar el último versículo proyectado, sin cambiarlo.
	if (bibleActive_) {
		if (bibleText_.trimmed().isEmpty())
			return;
		push([&](obs_data_t *d) { obs_data_set_string(d, "text", bibleText_.toUtf8().constData()); });
		callProc("show");
		return;
	}

	const QString t = phrase(selected_);
	if (t.trimmed().isEmpty())
		return;
	push([&](obs_data_t *d) { obs_data_set_string(d, "text", t.toUtf8().constData()); });
	callProc("show");
}

void NotifDock::refresh()
{
	Found found;
	obs_enum_sources(enum_source_cb, &found);
	obs_enum_scenes(enum_source_cb, &found);

	bool same = found.items.size() == items_.size();
	for (size_t i = 0; same && i < items_.size(); i++) {
		OBSSource a = OBSGetStrongRef(found.items[i].second);
		OBSSource b = OBSGetStrongRef(items_[i].second);
		obs_source_t *pa = a;
		obs_source_t *pb = b;
		same = found.items[i].first == items_[i].first && pa == pb;
	}
	if (same)
		return;

	OBSSource prevRef = current();
	obs_source_t *prev = prevRef;

	items_ = std::move(found.items);
	{
		QSignalBlocker b(target_);
		target_->clear();
		int sel = 0;
		for (size_t i = 0; i < items_.size(); i++) {
			target_->addItem(items_[i].first);
			OBSSource s = OBSGetStrongRef(items_[i].second);
			obs_source_t *ps = s;
			if (prev && ps == prev)
				sel = int(i);
		}
		if (!items_.empty())
			target_->setCurrentIndex(sel);
	}

	// Si todavía no existe el archivo de ajustes generales, se toman los de la primera fuente
	// que ya tenía el filtro con ajustes propios (versiones anteriores).
	if (notif_config_is_fresh() && !items_.empty()) {
		OBSSource first = OBSGetStrongRef(items_[0].second);
		if (first) {
			OBSDataAutoRelease old = obs_source_get_settings(first);
			notif_config_import_from(old);
			loadFromConfig();
		}
	}
}

// Carga en los controles los ajustes generales guardados.
void NotifDock::loadFromConfig()
{
	struct Values {
		int direction = 0, mode = 0, fontSize = 42, shAmount = 50, shAngle = 90, shLen = 16, shBlur = 24,
		    sel = 1;
		double animInSec = 1.0, animOutSec = 1.0, holdSec = 5.0, blurMax = 30.0;
		bool bold = true, shadow = false, linkAnim = true;
		QString fam, ffile;
		uint32_t bg = 0xFFFF4010, fg = 0xFFFFFFFF;
	} v;

	notif_config_read([&](obs_data_t *d) {
		v.direction = int(obs_data_get_int(d, "direction"));
		v.animInSec = obs_data_get_double(d, "anim_in_sec");
		v.animOutSec = obs_data_get_double(d, "anim_out_sec");
		v.linkAnim = obs_data_get_bool(d, "link_anim");
		v.holdSec = obs_data_get_double(d, "hold_sec");
		v.mode = int(obs_data_get_int(d, "mode"));
		v.blurMax = obs_data_get_double(d, "blur_max");
		v.fam = QString::fromUtf8(obs_data_get_string(d, "font_family"));
		v.ffile = QString::fromUtf8(obs_data_get_string(d, "font_file"));
		v.fontSize = int(obs_data_get_int(d, "font_size"));
		v.bold = obs_data_get_bool(d, "bold");
		v.shadow = obs_data_get_bool(d, "shadow");
		v.shAmount = int(obs_data_get_int(d, "shadow_amount"));
		v.shAngle = int(obs_data_get_int(d, "shadow_angle"));
		v.shLen = int(obs_data_get_int(d, "shadow_len"));
		v.shBlur = int(obs_data_get_int(d, "shadow_blur"));
		v.bg = uint32_t(obs_data_get_int(d, "bg_color"));
		v.fg = uint32_t(obs_data_get_int(d, "text_color"));
		v.sel = int(obs_data_get_int(d, "phrase_sel"));
	});

	// Mientras se cargan los valores, los controles no guardan nada (push no hace nada con loading_).
	loading_ = true;

	direction_->setCurrentIndex(v.direction);
	animInSec_->setValue(v.animInSec);
	animOutSec_->setValue(v.animOutSec);
	linkAnim_->setChecked(v.linkAnim);
	animOutSec_->setEnabled(!v.linkAnim);
	holdSec_->setValue(v.holdSec);
	if (v.mode == 1)
		modeAction_->setChecked(true);
	else
		modeTime_->setChecked(true);
	holdSec_->setEnabled(v.mode != 1);
	blur_->setValue(int(v.blurMax));

	if (!v.ffile.isEmpty())
		QFontDatabase::addApplicationFont(v.ffile);
	if (font_->findText(v.fam) < 0)
		font_->addItem(v.fam);
	font_->setCurrentText(v.fam);

	fontSize_->setValue(v.fontSize);
	bold_->setChecked(v.bold);
	shadow_->setChecked(v.shadow);
	shAmount_->setValue(v.shAmount);
	shAngle_->setValue(v.shAngle);
	shLen_->setValue(v.shLen);
	shBlur_->setValue(v.shBlur);

	bgColor_ = v.bg;
	fgColor_ = v.fg;
	setColorButton(bgBtn_, bgColor_);
	setColorButton(fgBtn_, fgColor_);

	shAmount_->setEnabled(v.shadow);
	shAngle_->setEnabled(v.shadow);
	shLen_->setEnabled(v.shadow);
	shBlur_->setEnabled(v.shadow);

	// Casillero elegido (por defecto el 1)
	selected_ = std::clamp(v.sel, 1, kSlots);
	numBtns_[selected_ - 1]->setChecked(true);

	loading_ = false;
	updateSummary();
}

void NotifDock::callProc(const char *name)
{
	OBSSource s = current();
	if (!s)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(s);
	if (!ph)
		return;
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(ph, name, &cd);
	calldata_free(&cd);
}

// Atajo de teclado: si la notificación se ve, la oculta; si no, muestra la frase elegida.
void NotifDock::toggleSelected()
{
	// Con la ventana de la Biblia en primer plano, F3 no hace nada.
	if (bible_ && bible_->isActiveWindow())
		return;

	OBSSource s = current();
	if (!s)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(s);
	if (!ph)
		return;

	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(ph, "is_visible", &cd);
	bool visible = false;
	calldata_get_bool(&cd, "visible", &visible);
	calldata_free(&cd);

	if (visible)
		callProc("hide");
	else
		showSelected();
}

// ---------------------------------------------------------------------------
// Biblia (casilla 0)
// ---------------------------------------------------------------------------

void NotifDock::ensureBible()
{
	if (bible_)
		return;

bible_ = new BibleWindow(nullptr);

connect(bible_, &BibleWindow::projectRequested, this, [this](const QString &text, const QString &label) {
	bibleText_ = text;
	bibleLabel_ = label;
	setBibleActive(true);

	if (!current()) {
		bible_->showStatus(QStringLiteral("No hay ninguna fuente con el filtro de notificación elegida en el dock"));
		return;
	}

	push([&](obs_data_t *d) { 
		obs_data_set_string(d, "text", text.toUtf8().constData()); 
	});
	callProc("show");
});
connect(bible_, &BibleWindow::hideRequested, this, [this] { callProc("hide"); });
connect(bible_, &BibleWindow::remoteRequested, this, [this] { showRemoteQr(); });
}

void NotifDock::openBible()
{
	ensureBible();
	setBibleActive(true);
	
	// Si la ventana ya está visible y activa, F5 la minimiza.
	if (bible_->isVisible() && bible_->isActiveWindow()) {
		bible_->showMinimized();
	} else if (bible_->isMinimized()) {
		// Si estaba minimizada, la restauramos SIN perder si estaba maximizada
		bible_->setWindowState(bible_->windowState() & ~Qt::WindowMinimized);
		bible_->raise();
		bible_->activateWindow();
	} else {
		bible_->showAndFocus(); // Si estaba cerrada, la abre
	}
}

// Activa o desactiva la casilla 0. Activa, ningún botón del 1 al 9 queda marcado.
void NotifDock::setBibleActive(bool on)
{
	bibleActive_ = on;
	if (on && group_) {
		group_->setExclusive(false);
		for (int i = 0; i < kSlots; i++)
			numBtns_[i]->setChecked(false);
		group_->setExclusive(true);
	} else if (!on) {
		numBtns_[selected_ - 1]->setChecked(true);
	}
	updateSummary();
}

// Diálogo "Conectar Celular": código QR (lo genera api.qrserver.com, igual que en el plugin de letras).
void NotifDock::showRemoteQr()
{
	QWidget *owner = bible_ ? static_cast<QWidget *>(bible_) : static_cast<QWidget *>(this);
	QDialog dlg(owner);
	dlg.setWindowTitle(QStringLiteral("Conectar Celular"));
	dlg.setFixedSize(400, 480);
	auto *layout = new QVBoxLayout(&dlg);

	if (!server_ || !server_->isListening()) {
		const QString why = server_ ? server_->errorString() : QString();
		auto *msg = new QLabel(QStringLiteral("No se pudo iniciar el control remoto en el puerto 8081.\n%1\n\n"
						      "Puede que otro programa esté usando ese puerto.")
					       .arg(why),
				       &dlg);
		msg->setAlignment(Qt::AlignCenter);
		msg->setWordWrap(true);
		layout->addWidget(msg, 1);
		auto *close = new QPushButton(QStringLiteral("Cerrar"), &dlg);
		connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
		layout->addWidget(close);
		dlg.exec();
		return;
	}

	const QString url = QStringLiteral("http://%1:%2").arg(server_->getLocalIP()).arg(server_->port());

	auto *label = new QLabel(QStringLiteral("Escaneá este código QR con tu celular:\n(Asegurate de estar en el mismo Wi-Fi)"), &dlg);
	label->setAlignment(Qt::AlignCenter);
	layout->addWidget(label);

	auto *qrLabel = new QLabel(QStringLiteral("Generando código QR..."), &dlg);
	qrLabel->setAlignment(Qt::AlignCenter);
	qrLabel->setMinimumSize(300, 300);
	layout->addWidget(qrLabel, 1, Qt::AlignCenter);

	auto *urlLabel = new QLabel(url, &dlg);
	urlLabel->setAlignment(Qt::AlignCenter);
	urlLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 14px;"));
	layout->addWidget(urlLabel);

	auto *buttons = new QHBoxLayout();
	auto *copyBtn = new QPushButton(QStringLiteral("Copiar Enlace"), &dlg);
	auto *closeBtn = new QPushButton(QStringLiteral("Cerrar"), &dlg);
	buttons->addWidget(copyBtn);
	buttons->addWidget(closeBtn);
	layout->addLayout(buttons);

	connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
	connect(copyBtn, &QPushButton::clicked, this, [url]() { QApplication::clipboard()->setText(url); });

	// Generar el QR usando una API gratuita (necesita internet en la PC en este momento).
	auto *manager = new QNetworkAccessManager(&dlg);
	const QString encodedUrl = QString::fromLatin1(QUrl::toPercentEncoding(url));
	const QString apiUrl =
		QStringLiteral("http://api.qrserver.com/v1/create-qr-code/?size=300x300&data=%1").arg(encodedUrl);
	QNetworkReply *reply = manager->get(QNetworkRequest(QUrl(apiUrl)));
	connect(reply, &QNetworkReply::finished, &dlg, [reply, qrLabel]() {
		if (reply->error() == QNetworkReply::NoError) {
			QPixmap pixmap;
			pixmap.loadFromData(reply->readAll());
			qrLabel->setPixmap(pixmap);
		} else {
			qrLabel->setText(QStringLiteral("No se pudo cargar el QR.\n%1\n\nUsá el enlace de abajo.")
						 .arg(reply->errorString()));
		}
		reply->deleteLater();
	});

	dlg.exec();
}
