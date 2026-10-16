//
// HttpStub — a loopback HTTP/1.1 server for the telemetry tests. Never
// a real endpoint: it listens on 127.0.0.1, port 0.
//
// Records every request (method, path, headers, body, arrival time)
// and answers with whatever `respond` picks; status -1 accepts the
// request and never answers (a hung server, for transfer timeouts and
// aborts). Keep-alive, so Qt's connection reuse is exercised too.
//
// It also checks every request's header NAMES against the exact set
// its sink may send (Transport.h): anything Qt or the client adds
// beyond that — a Cookie, an Origin, a locale-bearing header nobody
// set on purpose — lands in headerViolations(), which the tests check
// in cleanup(). No QtHttpServer: it is an add-on module that CI's Qt
// install doesn't include.
//

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>
#include <memory>

namespace gitbolt::test {

/// Every header an OpenPanel request may carry, lower-case.
inline const QSet<QByteArray> kOpenPanelHeaders = {
    "host", "content-type", "content-length", "user-agent", "accept-language",
    "accept-encoding", "connection", "openpanel-client-id", "openpanel-client-secret",
};

/// Every header a GlitchTip request may carry, lower-case.
inline const QSet<QByteArray> kGlitchTipHeaders = {
    "host", "content-type", "content-length", "user-agent", "accept-language",
    "accept-encoding", "connection", "x-sentry-auth",
};

struct StubRequest {
    QByteArray method;
    QByteArray path;
    QList<QPair<QByteArray, QByteArray>> headers;   // names lower-case, in arrival order
    QByteArray body;
    qint64 arrivedMs = 0;     // since the stub started
    qint64 answeredMs = -1;   // -1: never answered
    int connection = 0;

    QByteArray header(const QByteArray& name) const {
        for (const auto& [key, value] : headers) {
            if (key == name)
                return value;
        }
        return {};
    }
    QSet<QByteArray> headerNames() const {
        QSet<QByteArray> names;
        for (const auto& header : headers)
            names.insert(header.first);
        return names;
    }
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    /// Body and header values together, for leak scans.
    QByteArray everything() const {
        QByteArray all = method + ' ' + path + '\n';
        for (const auto& [key, value] : headers)
            all += key + ": " + value + '\n';
        return all + body;
    }
};

struct StubResponse {
    int status = 202;   // -1: never answer
    QList<QPair<QByteArray, QByteArray>> headers;
};

class HttpStub {
public:
    HttpStub() {
        clock_.start();
        server_ = std::make_unique<QTcpServer>();
        QObject::connect(server_.get(), &QTcpServer::newConnection, server_.get(), [this] {
            while (QTcpSocket* socket = server_->nextPendingConnection())
                adopt(socket);
        });
        listening_ = server_->listen(QHostAddress::LocalHost, 0);
        port_ = server_->serverPort();
    }

    ~HttpStub() {
        for (const QPointer<QTcpSocket>& socket : std::as_const(sockets_)) {
            if (socket) {
                QObject::disconnect(socket, nullptr, nullptr, nullptr);
                socket->abort();
                delete socket.data();
            }
        }
    }

    bool isListening() const { return listening_; }
    quint16 port() const { return port_; }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(port_); }

    /// Picks each request's answer; 202 by default.
    std::function<StubResponse(const StubRequest&)> respond = [](const StubRequest&) {
        return StubResponse{};
    };

    const QList<StubRequest>& requests() const { return requests_; }
    /// The clock requests' arrivedMs is read from.
    qint64 elapsedMs() const { return clock_.elapsed(); }
    int disconnects() const { return disconnects_; }
    QStringList headerViolations() const { return violations_; }

    /// Stops accepting connections; later connects are refused.
    void stopListening() { server_->close(); }

private:
    struct Connection {
        int id = 0;
        QByteArray buffer;
    };

    void adopt(QTcpSocket* socket) {
        const int id = ++connectionCount_;
        sockets_.append(socket);
        auto connection = std::make_shared<Connection>();
        connection->id = id;
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
        StubRequest request;
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
        request.body = connection.buffer.mid(end + 4, contentLength);
        connection.buffer.remove(0, total);
        request.arrivedMs = clock_.elapsed();
        request.connection = connection.id;
        checkHeaders(request);

        const StubResponse response = respond(request);
        if (response.status >= 0) {
            QByteArray reply = "HTTP/1.1 " + QByteArray::number(response.status) + " Stub\r\n"
                               + "Content-Length: 0\r\nConnection: keep-alive\r\n";
            for (const auto& [key, value] : response.headers)
                reply += key + ": " + value + "\r\n";
            socket->write(reply + "\r\n");
            socket->flush();
            request.answeredMs = clock_.elapsed();
        }
        requests_.append(request);
        return true;
    }

    void checkHeaders(const StubRequest& request) {
        static const QRegularExpression store(QStringLiteral("^/api/[0-9]+/store/$"));
        const QSet<QByteArray>* allowed = nullptr;
        if (request.path == "/track")
            allowed = &kOpenPanelHeaders;
        else if (store.match(QString::fromLatin1(request.path)).hasMatch())
            allowed = &kGlitchTipHeaders;
        if (!allowed) {
            violations_ << QStringLiteral("unexpected path %1").arg(QString::fromLatin1(request.path));
            return;
        }
        const QSet<QByteArray> names = request.headerNames();
        if (names == *allowed && names.size() == request.headers.size())
            return;
        QStringList got;
        for (const auto& header : request.headers)
            got << QString::fromLatin1(header.first);
        violations_ << QStringLiteral("%1 headers: %2")
                           .arg(QString::fromLatin1(request.path), got.join(QLatin1Char(',')));
    }

    std::unique_ptr<QTcpServer> server_;
    QList<QPointer<QTcpSocket>> sockets_;
    QElapsedTimer clock_;
    QList<StubRequest> requests_;
    QStringList violations_;
    bool listening_ = false;
    quint16 port_ = 0;
    int connectionCount_ = 0;
    int disconnects_ = 0;
};

} // namespace gitbolt::test
