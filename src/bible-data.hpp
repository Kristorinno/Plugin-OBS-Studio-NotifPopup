#pragma once

#include <QString>

#include <map>
#include <memory>
#include <vector>

// Un versículo: número y texto ya limpio (sin asteriscos ni marcas [n] de notas).
struct BibleVerse {
	int n = 0;
	QString text;
};

struct BibleBook {
	int number = 0; // 1 a 66
	QString name;
	std::map<int, std::vector<BibleVerse>> chapters; // capítulo -> versículos
};

struct BibleVersion {
	QString file;  // nombre del archivo (sin carpeta)
	QString label; // nombre corto que se ve en la pestaña, por ejemplo RV1960
	std::map<int, BibleBook> books;
};

// Carga un archivo .xml (formato XMLBIBLE de HolyRics) o .txt ("Libro cap:vers texto").
// Si falla devuelve nullptr y deja el motivo en *error.
std::shared_ptr<BibleVersion> bible_load(const QString &path, QString *error);

// Nombre corto para la pestaña a partir del nombre de archivo.
QString bible_label_for(const QString &fileName);

// Orden por defecto: RV, NTV, NVI y después las demás.
int bible_default_rank(const QString &label);

// Nombre estándar del libro (1 a 66).
QString bible_book_name(int number);

// Texto de un versículo, o nullptr si no existe.
const QString *bible_verse_text(const BibleVersion &v, int book, int chapter, int verse);

// Quita asteriscos y marcas de nota [número], y junta los espacios.
QString bible_clean(QString text);
