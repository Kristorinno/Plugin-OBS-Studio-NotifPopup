#include "bible-server.hpp"
#include "bible-window.hpp"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

// Página del celular. Los acentos van como entidades HTML o \u en el JavaScript,
// para no depender de /utf-8 en el compilador. Está partida en dos para no pasar el límite de MSVC.
const char *kPageHead = R"HTML(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Biblia OBS</title>
<style>
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; overflow: hidden; }
body {
  background: #1e1e1e; color: #fff;
  font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
  display: flex; flex-direction: column; height: 100%;
}
button {
  background: #333; color: #fff; border: 0; border-radius: 12px; font-size: 1rem;
  touch-action: manipulation; -webkit-tap-highlight-color: transparent;
}
button:active { background: #2a4365; }
#top { display: flex; align-items: stretch; background: #111; border-bottom: 1px solid #333; }
#top button {
  flex: 1; padding: 14px 2px; border-radius: 0; background: transparent;
  color: #888; font-weight: bold; font-size: .85rem;
}
#top button.on { color: #fff; border-bottom: 3px solid #2a4365; }
#songs { padding: 14px 10px; color: #9db7e0; text-decoration: none; font-weight: bold; font-size: .85rem; }
#crumb { display: flex; align-items: center; gap: 10px; padding: 0 12px; }
#crumb button { padding: 8px 16px; margin: 8px 0; }
#crumb span { font-weight: bold; font-size: 1.1rem; }
#main { flex: 1; overflow-y: auto; padding: 8px 12px; }
.grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; }
.grid.books { grid-template-columns: repeat(2, 1fr); }
.grid button { padding: 16px 4px; font-size: 1.1rem; }
.sep { height: 14px; grid-column: 1 / -1; }
.v {
  display: block; width: 100%; text-align: left; background: #222; border-radius: 0;
  padding: 12px 14px; border-bottom: 1px solid #333; line-height: 1.35; white-space: pre-wrap;
}
.v.sel { background: #1f6fc0; }
.chip { display: block; width: 100%; text-align: left; margin-bottom: 8px; padding: 14px; }
.empty { text-align: center; color: #888; padding: 24px; }
#cite { text-align: center; color: #9db7e0; font-size: .9rem; padding: 4px; min-height: 24px; }
#st { text-align: center; font-size: .85rem; color: #ff6b6b; min-height: 18px; }
.ctl { display: flex; gap: 8px; padding: 6px 12px; background: #111; }
.ctl button { flex: 1; padding: 16px 4px; font-weight: bold; }
#pj { flex: 2; background: #1f6fc0; font-size: 1.2rem; }
#hd { background: #7a2f2f; }
</style>
</head>
<body>
<div id="top">
  <button id="t_b" class="on" onclick="tab('b')">Libros</button>
  <button id="t_0" onclick="tab(0)">Favoritos</button>
  <button id="t_1" onclick="tab(1)">Historial</button>
  <button id="t_2" onclick="tab(2)">Frecuentes</button>
  <a id="songs" href="#">Canciones</a>
</div>
<div id="crumb"></div>
<div id="main"></div>
<div id="cite"></div>
<div id="st"></div>
<div class="ctl">
  <button onclick="step('prev')">&#9664; Anterior</button>
  <button id="pj" onclick="act('/project','')">Proyectar</button>
  <button onclick="step('next')">Siguiente &#9654;</button>
</div>
<div class="ctl">
  <button id="hd" onclick="act('/hide','')">Ocultar</button>
  <button onclick="act('/fav/add','Agregado a favoritos')">&#9733; Favorito</button>
</div>
)HTML";

const char *kPageBody = R"HTML(<script>
var nav = { book: 1, chap: 1, sel: [] };
var bookName = '';
var mode = 'b';
var view = 'books';
var st = document.getElementById('st');

function $(i) { return document.getElementById(i); }
function api(u) {
  return fetch(u, { cache: 'no-store' }).then(function (r) {
    if (!r.ok) throw new Error();
    return r.json();
  });
}
function fail() { st.textContent = 'Sin conexi\u00f3n con OBS'; }
function ok() { st.textContent = ''; }
function el(tag, cls, txt) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (txt !== undefined) e.textContent = txt;
  return e;
}
function crumb(back, label) {
  var c = $('crumb');
  c.innerHTML = '';
  if (!back) return;
  var b = el('button', '', '\u25C0');
  b.onclick = back;
  c.appendChild(b);
  c.appendChild(el('span', '', label));
}
function setTabs() {
  ['b', 0, 1, 2].forEach(function (k) { $('t_' + k).className = (k === mode) ? 'on' : ''; });
}
function tab(t) {
  mode = t;
  setTabs();
  if (t === 'b') {
    if (view === 'verses') loadVerses();
    else if (view === 'chaps') loadChaps();
    else loadBooks();
  } else {
    loadBar(t);
  }
}

function loadBooks() {
  view = 'books';
  crumb(null);
  api('/api/books').then(function (d) {
    var m = $('main');
    m.innerHTML = '';
    var g = el('div', 'grid books');
    d.books.forEach(function (b) {
      if (b.n === 40) g.appendChild(el('div', 'sep'));
      var x = el('button', '', b.name);
      x.onclick = function () { nav.book = b.n; bookName = b.name; loadChaps(); };
      g.appendChild(x);
    });
    m.appendChild(g);
    ok();
  }).catch(fail);
}

function loadChaps() {
  view = 'chaps';
  crumb(loadBooks, bookName);
  api('/api/chapters/' + nav.book).then(function (d) {
    var m = $('main');
    m.innerHTML = '';
    var g = el('div', 'grid');
    d.chapters.forEach(function (c) {
      var x = el('button', '', String(c));
      x.onclick = function () { nav.chap = c; nav.sel = []; loadVerses(); };
      g.appendChild(x);
    });
    m.appendChild(g);
    ok();
  }).catch(fail);
}

function loadVerses() {
  view = 'verses';
  crumb(loadChaps, bookName + ' ' + nav.chap);
  api('/api/verses/' + nav.book + '/' + nav.chap).then(function (d) {
    var m = $('main');
    m.innerHTML = '';
    d.verses.forEach(function (v) {
      var x = el('button', 'v' + (nav.sel.indexOf(v.n) >= 0 ? ' sel' : ''), v.n + '  ' + v.t);
      x.onclick = function () { toggle(v.n, x); };
      m.appendChild(x);
    });
    m.scrollTop = 0;
    ok();
  }).catch(fail);
}

// Tocar un versi\u00f3culo solo lo elige (hasta 3) y lo carga en la PC; se proyecta con "Proyectar".
function toggle(n, x) {
  var i = nav.sel.indexOf(n);
  if (i >= 0) {
    nav.sel.splice(i, 1);
  } else {
    if (nav.sel.length >= 3) { st.textContent = 'M\u00e1ximo 3 vers\u00edculos'; return; }
    nav.sel.push(n);
    nav.sel.sort(function (a, b) { return a - b; });
  }
  x.className = 'v' + (i >= 0 ? '' : ' sel');
  api('/load/' + nav.book + '/' + nav.chap + '/' + nav.sel.join(',')).then(function (s) {
    $('cite').textContent = s.cite || '';
    ok();
  }).catch(fail);
}

function loadBar(t) {
  crumb(null);
  api('/api/bar/' + t).then(function (d) {
    var m = $('main');
    m.innerHTML = '';
    if (!d.items.length) { m.appendChild(el('div', 'empty', '(vac\u00edo)')); ok(); return; }
    d.items.forEach(function (it) {
      var x = el('button', 'chip', it.book + '  ' + it.ref + (it.extra ? '   ' + it.extra : ''));
      x.onclick = function () {
        api('/load/' + it.b + '/' + it.c + '/' + it.v.join(',')).then(function (s) {
          nav.book = it.b; nav.chap = it.c; nav.sel = it.v.slice(); bookName = it.book;
          $('cite').textContent = s.cite || '';
          mode = 'b'; setTabs(); loadVerses();
        }).catch(fail);
      };
      m.appendChild(x);
    });
    ok();
  }).catch(fail);
}

function act(url, msg) {
  api(url).then(function (s) {
    $('cite').textContent = msg || s.cite || '';
    ok();
  }).catch(fail);
}

// Anterior / Siguiente: la PC mueve la selecci\u00f3n al vers\u00edculo vecino y lo proyecta.
function step(dir) {
  api('/' + dir).then(function (s) {
    nav.book = s.book; nav.chap = s.chap; nav.sel = s.sel; bookName = s.bookName;
    $('cite').textContent = s.cite || '';
    mode = 'b'; setTabs(); loadVerses();
  }).catch(fail);
}

// Si cambian cosas desde la PC, la lista de vers\u00edculos se actualiza sola.
setInterval(function () {
  if (document.hidden) return;
  api('/api/state').then(function (s) {
    if (mode === 'b' && view === 'verses' &&
        (s.book !== nav.book || s.chap !== nav.chap || String(s.sel) !== String(nav.sel))) {
      nav.book = s.book; nav.chap = s.chap; nav.sel = s.sel; bookName = s.bookName;
      loadVerses();
    }
    ok();
  }).catch(function () {});
}, 3000);

$('songs').href = location.protocol + '//' + location.hostname + ':8080/';
loadBooks();
</script>
</body>
</html>
)HTML";

std::vector<int> parse_verses(const QByteArray &s)
{
	std::vector<int> out;
	const QList<QByteArray> parts = s.split(',');
	for (const QByteArray &p : parts) {
		bool ok = false;
		const int v = p.toInt(&ok);
		if (ok && v > 0)
			out.push_back(v);
	}
	return out;
}

} // namespace

BibleServer::BibleServer(QObject *parent) : QObject(parent), server_(new QTcpServer(this))
{
	connect(server_, &QTcpServer::newConnection, this, [this] { onNewConnection(); });
}

bool BibleServer::startServer(quint16 port)
{
	if (server_->isListening())
		return true;
	port_ = port;
	// Any: acepta conexiones desde otros dispositivos de la red (el celular).
	return server_->listen(QHostAddress::Any, port_);
}

void BibleServer::stopServer()
{
	server_->close();
}

bool BibleServer::isListening() const
{
	return server_->isListening();
}

QString BibleServer::errorString() const
{
	return server_->errorString();
}

QString BibleServer::getLocalIP() const
{
	QString fallback;
	const auto ifaces = QNetworkInterface::allInterfaces();
	for (const QNetworkInterface &ni : ifaces) {
		const auto f = ni.flags();
		if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) ||
		    (f & QNetworkInterface::IsLoopBack))
			continue;

		// Adaptadores virtuales (VMware, VirtualBox, Hyper-V, WSL): solo como último recurso.
		const QString name = ni.humanReadableName();
		const bool virt = name.contains(QStringLiteral("vEthernet"), Qt::CaseInsensitive) ||
				  name.contains(QStringLiteral("VMware"), Qt::CaseInsensitive) ||
				  name.contains(QStringLiteral("VirtualBox"), Qt::CaseInsensitive) ||
				  name.contains(QStringLiteral("Hyper-V"), Qt::CaseInsensitive) ||
				  name.contains(QStringLiteral("WSL"), Qt::CaseInsensitive);

		for (const QNetworkAddressEntry &e : ni.addressEntries()) {
			const QHostAddress a = e.ip();
			if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback())
				continue;
			if (a.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16))
				continue;
			if (!virt)
				return a.toString();
			if (fallback.isEmpty())
				fallback = a.toString();
		}
	}
	return fallback.isEmpty() ? QStringLiteral("127.0.0.1") : fallback;
}

void BibleServer::onNewConnection()
{
	while (QTcpSocket *socket = server_->nextPendingConnection())
		handleSocket(socket);
}

void BibleServer::handleSocket(QTcpSocket *socket)
{
	auto buf = std::make_shared<QByteArray>();

	connect(socket, &QTcpSocket::readyRead, this, [this, socket, buf]() {
		buf->append(socket->readAll());

		if (buf->size() > 8192) {
			QObject::disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
			sendResponse(socket, 413, "Payload Too Large", "text/plain", "too large");
			return;
		}

		const int end = buf->indexOf("\r\n\r\n");
		if (end < 0)
			return; // todavía no llegó el encabezado completo

		QObject::disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
		handleRequest(socket, buf->left(end));
	});

	connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);

	// Evita conexiones colgadas.
	QTimer::singleShot(5000, socket, [socket]() {
		if (socket->state() != QAbstractSocket::UnconnectedState)
			socket->abort();
	});
}

void BibleServer::handleRequest(QTcpSocket *socket, const QByteArray &head)
{
	const QByteArray firstLine = head.left(head.indexOf("\r\n"));
	const QList<QByteArray> parts = firstLine.split(' ');
	if (parts.size() < 2) {
		sendResponse(socket, 400, "Bad Request", "text/plain", "bad request");
		return;
	}

	const QByteArray method = parts.at(0);
	QByteArray path = parts.at(1);
	const int q = path.indexOf('?');
	if (q >= 0)
		path = path.left(q);

	if (method != "GET") {
		sendResponse(socket, 405, "Method Not Allowed", "text/plain", "method not allowed");
		return;
	}

	if (path == "/") {
		sendResponse(socket, 200, "OK", "text/html; charset=utf-8", QByteArray(kPageHead) + QByteArray(kPageBody));
		return;
	}

	auto sendJson = [&](const QJsonObject &o) {
		sendResponse(socket, 200, "OK", "application/json; charset=utf-8",
			     QJsonDocument(o).toJson(QJsonDocument::Compact));
	};

	// Todo lo demás necesita la ventana de la Biblia (se carga la primera vez que se pide).
	BibleWindow *w = provider_ ? provider_() : nullptr;
	if (!w) {
		sendResponse(socket, 503, "Service Unavailable", "text/plain", "bible not available");
		return;
	}

	const QList<QByteArray> s = path.split('/'); // s[0] está vacío
	const QByteArray a = s.value(1);

	if (a == "api") {
		const QByteArray b = s.value(2);
		if (b == "state") {
			sendJson(w->remoteState());
		} else if (b == "books") {
			sendJson(w->remoteBooks());
		} else if (b == "chapters" && s.size() >= 4) {
			sendJson(w->remoteChapters(s.value(3).toInt()));
		} else if (b == "verses" && s.size() >= 5) {
			sendJson(w->remoteVerses(s.value(3).toInt(), s.value(4).toInt()));
		} else if (b == "bar" && s.size() >= 4) {
			sendJson(w->remoteBar(s.value(3).toInt()));
		} else {
			sendResponse(socket, 404, "Not Found", "text/plain", "not found");
		}
	} else if (a == "load" && s.size() >= 4) {
		// /load/libro/capítulo/v1,v2,v3: se carga en la primera columna, sin proyectar.
		w->remoteLoad(s.value(2).toInt(), s.value(3).toInt(), parse_verses(s.value(4)));
		sendJson(w->remoteState());
	} else if (a == "project") {
		w->remoteProject();
		sendJson(w->remoteState());
	} else if (a == "hide") {
		w->remoteHide();
		sendJson(w->remoteState());
	} else if (a == "prev" || a == "next") {
		w->remoteStep(a == "next" ? +1 : -1);
		sendJson(w->remoteState());
	} else if (a == "fav" && s.value(2) == "add") {
		w->remoteAddFavorite();
		sendJson(w->remoteState());
	} else {
		// Incluye /favicon.ico, que el navegador del celular pide solo.
		sendResponse(socket, 404, "Not Found", "text/plain", "not found");
	}
}

void BibleServer::sendResponse(QTcpSocket *socket, int code, const QByteArray &reason, const QByteArray &contentType,
			       const QByteArray &body)
{
	QByteArray out;
	out.append("HTTP/1.1 ").append(QByteArray::number(code)).append(' ').append(reason).append("\r\n");
	out.append("Content-Type: ").append(contentType).append("\r\n");
	out.append("Content-Length: ").append(QByteArray::number(body.size())).append("\r\n");
	out.append("Cache-Control: no-store\r\n");
	out.append("Connection: close\r\n\r\n");
	out.append(body);

	socket->write(out);
	socket->disconnectFromHost(); // cierra después de enviar todo lo pendiente
}
