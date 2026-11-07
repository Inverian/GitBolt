//
// HttpServer — a loopback HTTP/1.1 server for the update tests, modelled
// on tests/telemetry/HttpStub.h but serving bodies. Never a real
// endpoint: it listens on 127.0.0.1, port 0, so every test stays off
// GitHub. No QtHttpServer: CI's Qt install doesn't include it.
//
// Any path: answers come from `routes` (exact path, query included),
// or from `respond` when a test sets it; anything else is a 404. An
// answer can be
//   - a body with any status and headers (Content-Length is always the
//     whole body's);
//   - a redirect to another path or another server's port
//     (Served::redirect);
//   - slow: the body in chunks, `chunkDelayMs` apart, after an initial
//     `delayMs`;
//   - stalled: headers and the first `stallAfter` bytes, then silence
//     with the connection held open (a transfer that stops midway);
//   - never answered: status -1 (a server that accepts and hangs).
// Every request is recorded (method, path, headers, arrival), so a test
// can prove a URL was never asked for. Keep-alive, as Qt reuses
// connections.
//

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <memory>

namespace gitbolt::test {

struct ServedRequest {
    QByteArray method;
    QByteArray path;
    QList<QPair<QByteArray, QByteArray>> headers;   // names lower-case, in arrival order
    qint64 arrivedMs = 0;                           // since the server started
    int connection = 0;

    QByteArray header(const QByteArray& name) const {
        for (const auto& [key, value] : headers) {
            if (key == name)
                return value;
        }
        return {};
    }
    bool hasHeader(const QByteArray& name) const {
        return std::any_of(headers.cbegin(), headers.cend(), [&](const auto& h) { return h.first == name; });
    }
};

struct Served {
    int status = 200;   // -1: never answer
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    int delayMs = 0;          // before anything is sent
    int chunkBytes = 0;       // > 0: the body in chunks of this size...
    int chunkDelayMs = 0;     // ...this far apart
    qint64 stallAfter = -1;   // >= 0: send this much of the body, then nothing

    static Served ok(QByteArray body) {
        Served served;
        served.body = std::move(body);
        served.headers = {{"Content-Type", "application/octet-stream"}};
        return served;
    }
    static Served json(QByteArray body) {
        Served served;
        served.body = std::move(body);
        served.headers = {{"Content-Type", "application/json; charset=utf-8"}};
        return served;
    }
    static Served withStatus(int status, QByteArray body = {}) {
        Served served;
        served.status = status;
        served.body = std::move(body);
        return served;
    }
    static Served redirect(const QUrl& to, int status = 302) {
        Served served;
        served.status = status;
        served.headers = {{"Location", to.toEncoded()}};
        served.body = "<html><body>You are being redirected.</body></html>";
        return served;
    }
    static Served never() { return withStatus(-1); }
};

class HttpServer {
public:
    HttpServer() {
        clock_.start();
        server_ = std::make_unique<QTcpServer>();
        QObject::connect(server_.get(), &QTcpServer::newConnection, server_.get(), [this] {
            while (QTcpSocket* socket = server_->nextPendingConnection())
                adopt(socket);
        });
        listening_ = server_->listen(QHostAddress::LocalHost, 0);
        port_ = server_->serverPort();
    }

    ~HttpServer() {
        for (const QPointer<QTcpSocket>& socket : std::as_const(sockets_)) {
            if (socket) {
                QObject::disconnect(socket, nullptr, nullptr, nullptr);
                socket->abort();
                delete socket.data();
            }
        }
    }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool isListening() const { return listening_; }
    quint16 port() const { return port_; }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(port_); }
    QUrl url(const QString& path) const { return QUrl(origin() + path); }

    /// Exact path (query included) → answer.
    QHash<QByteArray, Served> routes;
    /// When set, answers every request instead of `routes`.
    std::function<Served(const ServedRequest&)> respond;

    const QList<ServedRequest>& requests() const { return requests_; }
    int hits(const QByteArray& path) const {
        return int(std::count_if(requests_.cbegin(), requests_.cend(),
                                 [&](const ServedRequest& request) { return request.path == path; }));
    }
    int disconnects() const { return disconnects_; }
    qint64 elapsedMs() const { return clock_.elapsed(); }

private:
    struct Connection {
        int id = 0;
        QByteArray buffer;
    };

    static QByteArray reason(int status) {
        switch (status) {
        case 200: return "OK";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 307: return "Temporary Redirect";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        default: return "Stub";
        }
    }

    void adopt(QTcpSocket* socket) {
        sockets_.append(socket);
        auto connection = std::make_shared<Connection>();
        connection->id = ++connectionCount_;
        QObject::connect(socket, &QTcpSocket::readyRead, server_.get(), [this, socket, connection] {
            connection->buffer += socket->readAll();
            while (parseOne(socket, *connection)) {
            }
        });
        QObject::connect(socket, &QTcpSocket::disconnected, server_.get(), [this, socket] {
            ++disconnects_;
            socket->deleteLater();
        });
    }

    // Parses one complete request off the front of the buffer and
    // answers it; false when the buffer holds no complete request.
    bool parseOne(QTcpSocket* socket, Connection& connection) {
        const qsizetype end = connection.buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return false;
        const QList<QByteArray> lines = connection.buffer.left(end).split('\n');
        ServedRequest request;
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() >= 2) {
            request.method = requestLine.at(0);
            request.path = requestLine.at(1);
        }
        qsizetype contentLength = 0;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            request.headers.append({name, value});
            if (name == "content-length")
                contentLength = value.toLongLong();
        }
        const qsizetype total = end + 4 + contentLength;
        if (connection.buffer.size() < total)
            return false;
        connection.buffer.remove(0, total);
        request.arrivedMs = clock_.elapsed();
        request.connection = connection.id;
        requests_.append(request);

        Served served;
        if (respond)
            served = respond(request);
        else if (const auto it = routes.constFind(request.path); it != routes.constEnd())
            served = *it;
        else
            served = Served::withStatus(404, "not found");
        if (served.status >= 0)
            answer(socket, served);
        return true;
    }

    void answer(QTcpSocket* socket, const Served& served) {
        QByteArray head = "HTTP/1.1 " + QByteArray::number(served.status) + ' ' + reason(served.status) + "\r\n"
                          + "Content-Length: " + QByteArray::number(served.body.size()) + "\r\n"
                          + "Connection: keep-alive\r\n";
        for (const auto& [key, value] : served.headers)
            head += key + ": " + value + "\r\n";
        head += "\r\n";

        const qint64 sendable = served.stallAfter >= 0 ? std::min<qint64>(served.stallAfter, served.body.size())
                                                       : served.body.size();
        const QByteArray body = served.body.left(sendable);
        if (served.delayMs <= 0 && served.chunkBytes <= 0) {
            socket->write(head + body);
            socket->flush();
            return;
        }
        // Slow: a timer owned by the socket, so it dies with the
        // connection when the client gives up.
        QPointer<QTcpSocket> guard(socket);
        auto remaining = std::make_shared<QByteArray>(body);
        const int chunk = served.chunkBytes > 0 ? served.chunkBytes : int(body.size());
        const int gap = served.chunkDelayMs;
        auto* timer = new QTimer(socket);
        timer->setSingleShot(true);
        auto headSent = std::make_shared<bool>(false);
        QObject::connect(timer, &QTimer::timeout, socket, [guard, timer, remaining, chunk, gap, head, headSent] {
            if (!guard)
                return;
            if (!*headSent) {
                guard->write(head);
                *headSent = true;
            }
            const QByteArray next = remaining->left(chunk);
            remaining->remove(0, next.size());
            guard->write(next);
            guard->flush();
            if (!remaining->isEmpty())
                timer->start(gap);
        });
        timer->start(served.delayMs);
    }

    std::unique_ptr<QTcpServer> server_;
    QList<QPointer<QTcpSocket>> sockets_;
    QElapsedTimer clock_;
    QList<ServedRequest> requests_;
    bool listening_ = false;
    quint16 port_ = 0;
    int connectionCount_ = 0;
    int disconnects_ = 0;
};

} // namespace gitbolt::test
