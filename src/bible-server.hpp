#pragma once

#include <QObject>
#include <QString>

#include <functional>

class BibleWindow;
class QTcpServer;
class QTcpSocket;

// Servidor HTTP mínimo para controlar la Biblia desde el celular (mismo estilo que el del plugin
// de letras). Corre en el hilo principal con eventos de Qt: no bloquea OBS.
class BibleServer : public QObject {
public:
	explicit BibleServer(QObject *parent = nullptr);

	// Devuelve la ventana de la Biblia ya cargada (la crea si todavía no existe).
	void setProvider(std::function<BibleWindow *()> fn) { provider_ = std::move(fn); }

	bool startServer(quint16 port = 8081);
	void stopServer();
	bool isListening() const;
	quint16 port() const { return port_; }
	QString errorString() const;
	QString getLocalIP() const;

private:
	void onNewConnection();
	void handleSocket(QTcpSocket *socket);
	void handleRequest(QTcpSocket *socket, const QByteArray &head);
	void sendResponse(QTcpSocket *socket, int code, const QByteArray &reason, const QByteArray &contentType,
			  const QByteArray &body);

	QTcpServer *server_ = nullptr;
	quint16 port_ = 8081;
	std::function<BibleWindow *()> provider_;
};
