#include "bible-data.hpp"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

const char *const kBookNames[66] = {
	"Génesis", "Éxodo", "Levítico", "Números", "Deuteronomio", "Josué", "Jueces", "Rut", "1 Samuel",
	"2 Samuel", "1 Reyes", "2 Reyes", "1 Crónicas", "2 Crónicas", "Esdras", "Nehemías", "Ester", "Job",
	"Salmos", "Proverbios", "Eclesiastés", "Cantares", "Isaías", "Jeremías", "Lamentaciones", "Ezequiel",
	"Daniel", "Oseas", "Joel", "Amós", "Abdías", "Jonás", "Miqueas", "Nahúm", "Habacuc", "Sofonías", "Hageo",
	"Zacarías", "Malaquías", "Mateo", "Marcos", "Lucas", "Juan", "Hechos", "Romanos", "1 Corintios",
	"2 Corintios", "Gálatas", "Efesios", "Filipenses", "Colosenses", "1 Tesalonicenses", "2 Tesalonicenses",
	"1 Timoteo", "2 Timoteo", "Tito", "Filemón", "Hebreos", "Santiago", "1 Pedro", "2 Pedro", "1 Juan",
	"2 Juan", "3 Juan", "Judas", "Apocalipsis"};

// Minúsculas y sin tildes, para comparar nombres de libros.
QString norm(const QString &s)
{
	const QString d = s.normalized(QString::NormalizationForm_D);
	QString out;
	out.reserve(d.size());
	for (const QChar c : d) {
		if (c.category() == QChar::Mark_NonSpacing)
			continue;
		out += c;
	}
	return out.toLower().simplified();
}

// Nombre normalizado -> número de libro (incluye algunos nombres alternativos).
const std::map<QString, int> &name_map()
{
	static std::map<QString, int> m;
	if (m.empty()) {
		for (int i = 0; i < 66; i++)
			m[norm(QString::fromUtf8(kBookNames[i]))] = i + 1;
		m[QStringLiteral("salmo")] = 19;
		m[QStringLiteral("cantar de los cantares")] = 22;
		m[QStringLiteral("cantar")] = 22;
		m[QStringLiteral("hechos de los apostoles")] = 44;
		m[QStringLiteral("revelacion")] = 66;
	}
	return m;
}

int canonical_number(const QString &name)
{
	const auto &m = name_map();
	auto it = m.find(norm(name));
	return it == m.end() ? 0 : it->second;
}

std::shared_ptr<BibleVersion> load_xml(const QString &path, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error)
			*error = QStringLiteral("No se pudo abrir el archivo");
		return nullptr;
	}

	auto ver = std::make_shared<BibleVersion>();
	QXmlStreamReader xr(&f);

	BibleBook *book = nullptr;
	int chap = 0;
	int count = 0;

	while (!xr.atEnd()) {
		xr.readNext();
		if (!xr.isStartElement())
			continue;

		const QStringView tag = xr.name();
		if (tag.compare(u"BIBLEBOOK", Qt::CaseInsensitive) == 0) {
			int num = xr.attributes().value(QStringLiteral("bnumber")).toString().toInt();
			const QString name = xr.attributes().value(QStringLiteral("bname")).toString().trimmed();
			if (num <= 0)
				num = canonical_number(name);
			if (num <= 0)
				num = int(ver->books.size()) + 1;
			book = &ver->books[num];
			book->number = num;
			book->name = name.isEmpty() ? bible_book_name(num) : name;
			chap = 0;
		} else if (tag.compare(u"CHAPTER", Qt::CaseInsensitive) == 0) {
			chap = xr.attributes().value(QStringLiteral("cnumber")).toString().toInt();
		} else if (tag.compare(u"VERS", Qt::CaseInsensitive) == 0) {
			const int vn = xr.attributes().value(QStringLiteral("vnumber")).toString().toInt();
			const QString text = bible_clean(xr.readElementText(QXmlStreamReader::IncludeChildElements));
			if (book && chap > 0 && vn > 0) {
				book->chapters[chap].push_back({vn, text});
				count++;
			}
		}
	}

	if (xr.hasError() && count == 0) {
		if (error)
			*error = xr.errorString();
		return nullptr;
	}
	if (count == 0) {
		if (error)
			*error = QStringLiteral("No se encontraron versículos (formato XMLBIBLE esperado)");
		return nullptr;
	}
	return ver;
}

std::shared_ptr<BibleVersion> load_txt(const QString &path, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		if (error)
			*error = QStringLiteral("No se pudo abrir el archivo");
		return nullptr;
	}
	QTextStream in(&f);
	in.setEncoding(QStringConverter::Utf8);

	static const QRegularExpression re(QStringLiteral("^\\s*(.+?)\\s+(\\d+)\\s*:\\s*(\\d+)\\s+(.*\\S)\\s*$"));

	struct Line {
		QString book;
		int chap, verse;
		QString text;
	};
	std::vector<Line> lines;
	std::vector<QString> order; // libros en orden de aparición
	bool allKnown = true;

	while (!in.atEnd()) {
		const QString raw = in.readLine();
		if (raw.trimmed().isEmpty() || raw.trimmed().startsWith(QLatin1Char('#')))
			continue;
		const QRegularExpressionMatch m = re.match(raw);
		if (!m.hasMatch())
			continue;
		Line l;
		l.book = m.captured(1).trimmed();
		l.chap = m.captured(2).toInt();
		l.verse = m.captured(3).toInt();
		l.text = bible_clean(m.captured(4));
		if (l.chap <= 0 || l.verse <= 0)
			continue;
		if (std::find(order.begin(), order.end(), l.book) == order.end()) {
			order.push_back(l.book);
			if (canonical_number(l.book) == 0)
				allKnown = false;
		}
		lines.push_back(std::move(l));
	}

	if (lines.empty()) {
		if (error)
			*error = QStringLiteral("No se encontraron versículos (formato esperado: Libro 1:1 texto)");
		return nullptr;
	}

	// Si algún nombre no se reconoce, los libros se numeran por orden de aparición.
	auto numberOf = [&](const QString &name) {
		if (allKnown)
			return canonical_number(name);
		return int(std::find(order.begin(), order.end(), name) - order.begin()) + 1;
	};

	auto ver = std::make_shared<BibleVersion>();
	for (const Line &l : lines) {
		const int num = numberOf(l.book);
		BibleBook &b = ver->books[num];
		b.number = num;
		if (b.name.isEmpty())
			b.name = l.book;
		b.chapters[l.chap].push_back({l.verse, l.text});
	}
	return ver;
}

} // namespace

QString bible_book_name(int number)
{
	if (number >= 1 && number <= 66)
		return QString::fromUtf8(kBookNames[number - 1]);
	return QStringLiteral("Libro %1").arg(number);
}

QString bible_clean(QString text)
{
	static const QRegularExpression note(QStringLiteral("\\[\\d+\\]"));
	text.remove(QLatin1Char('*'));
	text.remove(note);
	return text.simplified();
}

std::shared_ptr<BibleVersion> bible_load(const QString &path, QString *error)
{
	std::shared_ptr<BibleVersion> ver;
	const QString ext = QFileInfo(path).suffix().toLower();
	if (ext == QLatin1String("xml"))
		ver = load_xml(path, error);
	else if (ext == QLatin1String("txt"))
		ver = load_txt(path, error);
	else {
		if (error)
			*error = QStringLiteral("Formato no admitido (usá .xml o .txt)");
		return nullptr;
	}

	if (!ver)
		return nullptr;

	// Los versículos de cada capítulo quedan en orden.
	for (auto &b : ver->books)
		for (auto &c : b.second.chapters)
			std::sort(c.second.begin(), c.second.end(),
				  [](const BibleVerse &a, const BibleVerse &b2) { return a.n < b2.n; });

	ver->file = QFileInfo(path).fileName();
	ver->label = bible_label_for(ver->file);
	return ver;
}

QString bible_label_for(const QString &fileName)
{
	const QString base = QFileInfo(fileName).completeBaseName();
	const QString l = norm(base);

	if (l.contains(QStringLiteral("reina")) && l.contains(QStringLiteral("valera"))) {
		static const QRegularExpression year(QStringLiteral("(\\d{4})"));
		const QRegularExpressionMatch m = year.match(base);
		return QStringLiteral("RV") + (m.hasMatch() ? m.captured(1) : QString());
	}
	if (l.contains(QStringLiteral("viviente")))
		return QStringLiteral("NTV");
	if (l.contains(QStringLiteral("internacional")))
		return QStringLiteral("NVI");

	QString s = base;
	if (s.startsWith(QStringLiteral("es_"), Qt::CaseInsensitive))
		s = s.mid(3).toUpper();
	s.replace(QLatin1Char('_'), QLatin1Char(' '));
	return s;
}

int bible_default_rank(const QString &label)
{
	if (label.startsWith(QStringLiteral("RV")))
		return 0;
	if (label == QStringLiteral("NTV"))
		return 1;
	if (label == QStringLiteral("NVI"))
		return 2;
	return 3;
}

const QString *bible_verse_text(const BibleVersion &v, int book, int chapter, int verse)
{
	auto b = v.books.find(book);
	if (b == v.books.end())
		return nullptr;
	auto c = b->second.chapters.find(chapter);
	if (c == b->second.chapters.end())
		return nullptr;
	const auto &vec = c->second;
	if (verse >= 1 && size_t(verse) <= vec.size() && vec[size_t(verse - 1)].n == verse)
		return &vec[size_t(verse - 1)].text;
	for (const BibleVerse &bv : vec)
		if (bv.n == verse)
			return &bv.text;
	return nullptr;
}
