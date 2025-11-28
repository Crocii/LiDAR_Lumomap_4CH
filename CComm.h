/*
 * Copyright (C) 2024
 *
 * This file is part of LumosLiDARViewer.
 *
 * LumosLiDARViewer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * LumosLiDARViewer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with LumosLiDARViewer.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef COMM_H
#define COMM_H

#ifdef ENABLE_RUNINTIME
#include <QtCore/QFuture>
#include <QFutureWatcher>
#endif

#include <QtCore/QTimer>
#include <QtCore/QObject>

#include <QtCore/QMutex>
#include <QtConcurrent>


#define THREAD_BEGIN    QtConcurrent::run([&]() {
#define THREAD_END      });

#ifndef _WINBASE_
#define IGNORE              0       // Ignore signal
#define INFINITE		0xFFFFFFFF  // Infinite timeout
#endif

#define comm_noErr (\
m_status < eStatus::onError)
#define comm_onErr (\
m_status >= eStatus::onError)
#define comm_home (\
m_status == eStatus::closed)
#define comm_idle (\
m_status == eStatus::ready)
#define comm_busy (\
m_status == eStatus::sending || \
m_status == eStatus::recving)
#define comm_doing (comm_busy || \
m_status == eStatus::connecting || \
m_status == eStatus::closing)
#define comm_done (\
m_status == eStatus::connected || \
m_status == eStatus::sent || \
m_status == eStatus::recved)


//*===============================================================*//
//*                        Astract Classe                         *//
//*===============================================================*//

class Comm : public QObject {
    Q_OBJECT

public:
    Comm(QObject *parent = nullptr, int commID = 0)
        : QObject(parent), connWatchdog(this), progTimeout(this)
    {
        m_commID = commID;
        m_status = eStatus::closed;
        //m_statErr = eStatus::noErrStat;

        QObject::connect(&connWatchdog, &QTimer::timeout, this, [&]() {
            if (m_holdWatchdog) {
                m_holdWatchdog = false;
                return;
            }

            if (!comm_idle)
                return;

            m_isChecking = true;
            checkConn();
            m_isChecking = false;
        });

        progTimeout.setSingleShot(true);
        QObject::connect(&progTimeout, &QTimer::timeout, this, &Comm::onProgTimeout);
    }
    virtual ~Comm() override {
        m_isClosed = true;
        m_status = eStatus::closed;
        connWatchdog.stop();
        progTimeout.stop();
    }

private slots:
    void onProgTimeout() {
        if (worker.isRunning()) {
            worker.cancel();
            if (m_status == eStatus::sending) {
                setStatus(eStatus::sendFailed);
            }
            else if(m_status == eStatus::recving) {
                setStatus(eStatus::recvFailed);
            }
            else if(m_status == eStatus::connecting) {
                setStatus(eStatus::connFailed);
            }
            else if (m_status == eStatus::inboxing) {
                emit onProgress(this, eProgress::inbox, m_bytesInbox);
                doEvents();
            }
        }
    }

public:
    // Events
    enum class eStatus : unsigned int
    {
        closed = 0,			//정상 종료 상태.
        connecting,			//연결 시도 이벤트 As Master, Listening As Device
        connected,			//연결 완료 이벤트, 상태 점검 후 자동으로 ready로 넘어감.
        ready,				//idle 상태, 이전 단계 정상 완료, 다음 단계 진행 할 수 있음 확인.
        inboxing,           //수신버퍼 확인.
        sending,			//Tx 시작/진행 이벤트.
        sent,				//Tx 완료 이벤트.
        recving,			//Rx 시작/진행 이벤트.
        recved,				//Rx 완료 이벤트.
        closing,
        onError = 1000,
        connLost,
        connFailed,
        disconnFailed,
        sendFailed,
        recvFailed,
    };

    enum class eProgress : unsigned int
    {
        sending = (unsigned int)eStatus::sending,
        inbox = (unsigned int)eStatus::ready,
        recving = (unsigned int)eStatus::recving,
    };

    bool setConnInfo(QString connString, int connNum = 0, void* connInfo = nullptr) {
        m_connInfo = connInfo;
        m_connString = connString;
        m_connNum = connNum;
        m_connAvailable = setConnInfoProc(m_connString, m_connNum, m_connInfo);
        return m_connAvailable;
    }

public:
    bool connect(quint32 timeout = INFINITE) {
        if (!m_connAvailable)
            return false;

        eStatus curStatus = m_status;

        if (curStatus != eStatus::closed)
            return false;

        m_isClosed = false;

        setStatus(eStatus::connecting);
        m_isConnected = connectProc(timeout);
        if (m_isConnected)
            setStatus(eStatus::connected);
        else
            setStatus(eStatus::connFailed);
        return m_isConnected;
    }

    bool close(quint32 timeout = INFINITE) {
        m_isClosed = true;

        //eStatus curStatus = m_status;

        if (!checkConnProc())
            return true;

        setStatus(eStatus::closing);
        m_isConnected = !closeProc(timeout);

        if (!m_isConnected)
            setStatus(eStatus::closed);
        else
            setStatus(eStatus::disconnFailed);
        return m_isConnected;
    }

private:
    Q_INVOKABLE bool doSendProc(QByteArray &data, quint32 timeout) {
        QMutexLocker locker(&commMtx);

        bool ret = false;
        m_bytesSent = 0;

        setStatus(eStatus::sending);
        if (m_enableSendTimeout && timeout && timeout < INFINITE) {
            progTimeout.start(timeout);
            ret = sendProc(data, timeout);
            progTimeout.stop();
        }
        else
            ret = sendProc(data, timeout);

        if (ret)
            setStatus(eStatus::sent);
        else
            setStatus(eStatus::sendFailed);

        return ret;
    }

public:
    bool send(QByteArray &data, quint32 timeout = INFINITE, bool async = false) {
        if (m_isClosed)
            return false;

        if (!comm_idle)
            return false;

        if (async) {
            worker = QtConcurrent::run([this, &data, timeout]() {
                QMetaObject::invokeMethod(this, "doSendProc",
                                          Qt::BlockingQueuedConnection,
                                          Q_ARG(QByteArray&, data),
                                          Q_ARG(quint32, timeout));
            });
            return true;
        }
        else {
            return doSendProc(data, timeout);
        }
    }

private:
    Q_INVOKABLE bool doInboxProc(quint32 timeout) {
        QMutexLocker locker(&commMtx);

        bool ret = false;

        setStatus(eStatus::inboxing);
        if (m_enableRecvTimeout && timeout && timeout < INFINITE) {
            progTimeout.start(timeout);
            ret = inboxProc(timeout);
            progTimeout.stop();
        }
        else
            ret = inboxProc(timeout);

        m_holdWatchdog = true;
        m_status = eStatus::ready;
        emit onStatus(this, eStatus::ready);

        if (ret) {
            emit onProgress(this, eProgress::inbox, m_bytesInbox);
        }

        QTimer::singleShot(0, this, [this]() {
            m_holdWatchdog = false;
        });
        return ret;
    }


public:
    bool inbox(quint32 timeout = IGNORE, bool async = false) {
        if (m_isClosed)
            return false;

        if (!comm_idle)
            return false;

        m_holdWatchdog = true;

        if (async) {
            worker = QtConcurrent::run([this, timeout]() {
                QMetaObject::invokeMethod(this, "doInboxProc",
                                          Qt::BlockingQueuedConnection,
                                          Q_ARG(quint32, timeout));
            });
            m_holdWatchdog = false;
            return true;
        }
        else {
            m_holdWatchdog = false;
            return doInboxProc(timeout);
        }
    }

private:
    Q_INVOKABLE bool doRecvProc(QByteArray &data, quint32 timeout,
                                quint32 expectedBytes) {
        QMutexLocker locker(&commMtx);

        bool ret = false;
        m_bytesRecv = 0;

        setStatus(eStatus::recving);
        if (m_enableRecvTimeout && timeout && timeout < INFINITE) {
            progTimeout.start(timeout);
            ret = recvProc(data, timeout, expectedBytes);
            progTimeout.stop();
        }
        else
            ret = recvProc(data, timeout, expectedBytes);

        if (ret)
            setStatus(eStatus::recved);
        else
            setStatus(eStatus::recvFailed);

        return ret;
    }

public:
    bool recv(QByteArray &buffer, quint32 timeout = INFINITE,
              quint32 expectedBytes = IGNORE, bool async = false) {
        if (m_isClosed)
            return false;

        if (!comm_idle)
            return false;

        if (async) {
            worker = QtConcurrent::run([this, &buffer, timeout, expectedBytes]() {
                QMetaObject::invokeMethod(this, "doRecvProc",
                                          Qt::BlockingQueuedConnection,
                                          Q_ARG(QByteArray&, buffer),
                                          Q_ARG(quint32, timeout) ,
                                          Q_ARG(quint32, expectedBytes));
            });
            return true;
        }
        else {
            return doRecvProc(buffer, timeout, expectedBytes);
        }
    }

    int bytesRecv() const {
        return m_bytesRecv;
    }

    int bytesSent() const {
        return m_bytesSent;
    }

    int bytesInbox() const {
        return m_bytesInbox;
    }

    eStatus getStatus() const {
        return m_status;
    }

    bool checkConn() {
        if (this->m_isClosed) {
            setStatus(eStatus::closed);
            m_isConnected = false;
            return false;
        }

        if (comm_busy)
            return true;

        if (this->checkConnProc()) {
            if (!m_isConnected) {
                m_isConnected = true;
                setStatus(eStatus::connected);
            }
            else {
                setStatus(eStatus::ready);
                doEvents();
            }
        }
        else {
            if (m_isConnected) {
                m_isConnected = false;
                setStatus(eStatus::connLost);
            }
            else
                setStatus(eStatus::closed);
        }
        return m_isConnected;
    }

    bool reconnect() {
        if (checkConnProc())
            closeProc();
        connectProc();
        //commMtx.unlock();
        return checkConn();
    }

    bool isConnected() const {
        return m_isConnected;
    }

    bool waitForReady() {
        if (m_status == eStatus::ready)
            return true;

        if (!comm_done)
            return false;

        while (!comm_idle) {
            doEvents();
        }
        return true;
    }

    bool isOnError() const {
        return comm_onErr;
    }

    bool isIdle() const {
        return comm_idle;
    }

    bool isAtHome() const {
        return comm_home;
    }

    bool isBusy() const {
        return comm_busy;
    }

    bool isDone() const {
        return comm_done;
    }

    void doEvents(int timeout = IGNORE) {
        stopwatch.start();
        do {
            QCoreApplication::processEvents();
        } while (stopwatch.elapsed() < timeout);
    }

public:
    // Connection monitoring
    void setWatchDog(bool checkConnAlive = false, int checkInterval = INFINITE) {
        if (checkConnAlive) {
            if (connWatchdog.isActive())
                connWatchdog.stop();
            connWatchdog.start(checkInterval);
        }
        else {
            connWatchdog.stop();
        }
    }

protected:
    //Must be implemented.
    virtual bool setConnInfoProc(QString connString, int connNum = 0,
                                 void* connInfo = nullptr) = 0;
    virtual bool connectProc(quint32 timeout = INFINITE) = 0;
    virtual bool closeProc(quint32 timeout = INFINITE) const = 0;
    virtual bool sendProc(QByteArray &data, quint32 timeout = INFINITE) = 0;
    virtual bool inboxProc(quint32 timeout = IGNORE) = 0;
    virtual bool recvProc(QByteArray &buffer, quint32 timeout = INFINITE,
                          quint32 expectedBytes = IGNORE) = 0;
    virtual bool checkConnProc() = 0;

private:
    void setStatus(eStatus status, bool async = false) {
        if (m_status == status) return;
        m_status = status;
        if (async || comm_onErr) {
            QTimer::singleShot(0, this, [this, status]() {
                emit onStatus(this, status);
                this->checkConn();
            });
        }
        else {
            switch (status) {
            case eStatus::connecting:
            case eStatus::sending:
            case eStatus::recving:
            case eStatus::closing:
            case eStatus::inboxing:
                emit onStatus(this, status);
                doEvents();
                break;
            case eStatus::sent:
                QTimer::singleShot(0, this, [this, status]() {
                    emit onStatus(this, status);
                });
                QTimer::singleShot(0, this, [this]() {
                    m_holdWatchdog = true;
                    m_status = eStatus::ready;
                    emit onStatus(this, eStatus::ready);
                    QTimer::singleShot(0, this, [this]() {
                        m_holdWatchdog = false;
                    });
                });
                break;
            case eStatus::connected:
            case eStatus::recved:
                QTimer::singleShot(0, this, [this, status]() {
                    emit onStatus(this, status);
                });
                QTimer::singleShot(0, this, [this]() {
                    m_status = eStatus::ready;
                    emit onStatus(this, eStatus::ready);
                });
                break;
            default:
                QTimer::singleShot(0, this, [this, status]() {
                    emit onStatus(this, status);
                });
                break;
            }
        }
    }

protected:
    void setAlert(Comm *sender, int alertCode, const QString msg) {
        emit onAlert(sender, alertCode, msg);
        doEvents();
    }

    void setProgress(eProgress progress, quint32 bytesTotal) {
        progTimeout.stop();
        emit onProgress(this, progress, bytesTotal);
        doEvents();
        progTimeout.start(m_timeout);
    }

    bool isClosed() const {
        return m_isClosed;
    }

signals:
    void onStatus(Comm *sender, eStatus status);
    void onProgress(Comm *sender, eProgress progress, quint32 bytes);
    void onAlert(Comm *sender, int alertCode, const QString msg);

protected:
    int m_commID = 0;
    void *m_connInfo = nullptr;
    QString m_connString;
    int m_connNum = 0;
    bool m_connAvailable = false;

    eStatus m_status = eStatus::closed;
    //eStatErr m_statErr = eStatus::noErrStat;
    bool m_isClosed = true;
    bool m_isConnected = false;
#ifdef ENABLE_RUNINTIME
    bool m_futureRunning = false;
    bool m_futureResult = false;
#endif
    int m_bytesSent = 0;
    int m_bytesRecv = 0;
    int m_bytesInbox = 0;

    QTimer connWatchdog;
    bool m_holdWatchdog = false;
    bool m_isChecking = false;
    QTimer progTimeout;
    QElapsedTimer stopwatch;
    QFuture<void> worker;
    QMutex commMtx;

protected:
    quint32 m_inbox = 0;
    quint32 m_timeout = INFINITE;
    bool m_enableConnTimeout = false, m_enableSendTimeout = false, m_enableRecvTimeout = false;
};

#include <QtCore/QByteArray>
#include <QtNetwork/QHostAddress>

//*===============================================================*//
//*                        Dirived Classes                        *//
//*===============================================================*//

// CommTCP class
#include <QtNetwork/QTcpSocket>
class CommTCP : public Comm {
    Q_OBJECT

public:
    CommTCP(QObject *parent = nullptr, int commID = 0)
        : Comm(parent, commID), socket(new QTcpSocket(this)) {
        QObject::connect(socket, &QTcpSocket::errorOccurred, this, &CommTCP::handleError);
        QObject::connect(socket, &QTcpSocket::disconnected, this, &CommTCP::handleLostConn);
    }
    ~CommTCP() {
        socket->close();
    }

protected:
    bool setConnInfoProc(QString connString, int connNum, void* connInfo) override {
        Q_UNUSED(connInfo);
        bool isOK = (connNum > 0 && connNum <= 65535);
        if (!isOK)
            return false;
        if (connString.count('.') != 3)
            hostAddr = QHostAddress::Any;
        else
            hostAddr.setAddress(connString);
        m_port = (quint16)connNum;
        return true;
    }

    bool connectProc(quint32 timeout = INFINITE) override {
        if (!m_connAvailable)
            return false;
        if (checkConnProc())
            return true;
        // eStatus curStatus = m_status;
        // if (curStatus != eStatus::connecting)
        //     return false;
        socket->connectToHost(hostAddr, m_port);
        bool isOK = checkConnProc();
        if (!isOK && timeout)
            isOK = socket->waitForConnected(timeout);
        return isOK;
    }

    bool closeProc(quint32 timeout = INFINITE) const override {
        if (socket->state() == QAbstractSocket::UnconnectedState)
            return true;
        socket->disconnectFromHost();
        if (timeout)
            socket->waitForDisconnected(timeout);
        return socket->state() == QAbstractSocket::UnconnectedState;
    }

    bool sendProc(QByteArray &data, quint32 timeout = INFINITE) override {
        m_bytesSent = socket->write(data);
        if (timeout)
            socket->waitForBytesWritten(timeout);
        return m_bytesSent == data.size();
    }

    bool inboxProc(quint32 timeout = IGNORE) override {
        if (timeout)
            socket->waitForReadyRead(timeout);
        m_bytesInbox = socket->bytesAvailable();
        return m_bytesInbox > 0;
    }

    bool recvProc(QByteArray &buffer, quint32 timeout = INFINITE,
                  quint32 expectedBytes = IGNORE) override {
        if (timeout)
            socket->waitForReadyRead(timeout);

        while (socket->bytesAvailable()) {
            buffer += socket->readAll();
            if (expectedBytes && buffer.size() >= (int)expectedBytes)
                break;

            if (timeout && timeout < INFINITE) {
                doEvents();
                socket->waitForReadyRead(timeout);
            }
        }
        m_bytesRecv = buffer.size();
        return (bool)m_bytesRecv;
    }

    bool checkConnProc() override {
        return socket->state() == QAbstractSocket::ConnectedState;
    }

private:
    QTcpSocket *socket;
    quint16 m_port;
    QHostAddress hostAddr;

    void handleError(QAbstractSocket::SocketError err) {
        if (m_status == eStatus::inboxing || m_status == eStatus::recving)
            return;
        Q_UNUSED(err);
        this->setAlert(this, (int)err, socket->errorString());
    }

    void handleLostConn() {
        if (!this->isClosed())
            checkConn();
    }
};

// CommUDP class
#include <QtNetwork/QUdpSocket>
class CommUDP : public Comm {
    Q_OBJECT

public:

    CommUDP(QObject *parent = nullptr, int commID = 0)
        : Comm(parent, commID), socket(new QUdpSocket(this)) {
        QObject::connect(socket, &QUdpSocket::errorOccurred, this, &CommUDP::handleError);
        QObject::connect(socket, &QUdpSocket::disconnected, this, &CommUDP::handleLostConn);
    }
    ~CommUDP() { socket->close(); }

protected:
    bool setConnInfoProc(QString connString, int connNum, void* connInfo) override {
        Q_UNUSED(connInfo);
        bool isOK = (connNum > 0 && connNum <= 65535);
        if (!isOK)
            return false;
        if (connString.count('.') != 3)
            hostAddr = QHostAddress::LocalHost;
        else
            hostAddr.setAddress(connString);
        m_port = (quint16)connNum;
        return true;
    }

    bool connectProc(quint32 timeout = INFINITE) override {
        if (!m_connAvailable)
            return false;
        if (checkConnProc())
            return true;

        bool isOK = false;
        isOK = socket->bind(0, QTcpSocket::ShareAddress);
        m_localPort = socket->localPort();

        // if (hostAddr != QHostAddress::LocalHost) {
        //     socket->connectToHost(hostAddr, m_port);
        //     if (timeout)
        //         isOK = socket->waitForConnected(timeout);
        // }
        // else {
        //     isOK = socket->bind(0, QTcpSocket::ShareAddress);
        //     m_localPort = socket->localPort();
        // }
        if (!isOK)
            return false;

        return this->checkConnProc();
    }

    bool closeProc(quint32 timeout = INFINITE) const override {
        if (socket->state() == QAbstractSocket::UnconnectedState)
            return true;
        socket->disconnectFromHost();
        if (timeout)
            socket->waitForDisconnected(timeout);
        return socket->state() == QAbstractSocket::UnconnectedState;
    }

    bool sendProc(QByteArray &data, quint32 timeout = INFINITE) override {
        m_bytesSent = socket->writeDatagram(data, hostAddr, m_port);
        if (timeout)
            socket->waitForBytesWritten(timeout);
        return m_bytesSent == data.size();
    }

    bool inboxProc(quint32 timeout = IGNORE) override {
        if (timeout)
            socket->waitForReadyRead(timeout);
        m_bytesInbox = socket->bytesAvailable();
        return m_bytesInbox > 0;
    }

    bool recvProc(QByteArray &buffer, quint32 timeout = INFINITE,
                  quint32 expectedBytes = IGNORE) override {
        if (timeout)
            socket->waitForReadyRead(timeout);

        quint32 bytesRead = socket->bytesAvailable(), curStart = 0, curRead = 0;

        while (bytesRead) {
            curStart = buffer.size();
            buffer.resize(buffer.size() + bytesRead);
            curRead = 0;
            while (curRead < bytesRead) {
                curRead += socket->readDatagram(buffer.data() + curStart + curRead, bytesRead - curRead, &m_sender, &m_port);
                if (curRead == 0) {
                    buffer.clear();
                    return false;
                }
            }

            if (expectedBytes && curStart + curRead >= expectedBytes)
                break;

            if (timeout && timeout < INFINITE) {
                doEvents();
                socket->waitForReadyRead(timeout);
            }
            bytesRead = socket->bytesAvailable();
        }
        m_bytesRecv = buffer.size();
        socket->close();
        bool isOK = socket->bind(QHostAddress::Any, m_localPort);
        return isOK && (bool)m_bytesRecv;
    }

    bool checkConnProc() override {
        QAbstractSocket::SocketState curStat = socket->state();
        return curStat == QAbstractSocket::ConnectedState || \
                curStat == QAbstractSocket::BoundState;

        if (hostAddr == QHostAddress::LocalHost)
            return curStat == QAbstractSocket::BoundState;
        else
            return curStat == QAbstractSocket::ConnectedState;
    }

private:
    QUdpSocket *socket;
    quint16 m_port, m_localPort;
    QHostAddress hostAddr, m_sender;

    void handleError(QAbstractSocket::SocketError err) {
        if (m_status == eStatus::inboxing || m_status == eStatus::recving)
            return;
        Q_UNUSED(err);
        setAlert(nullptr, (int)err, socket->errorString());
        qDebug() << err << ": " << socket->errorString();
    }

    void handleLostConn() {
        if (!this->isClosed())
            checkConn();
    }
};

#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>
#include <QList>
class CommSerial : public Comm {
    Q_OBJECT

public:
    CommSerial(QObject *parent, int commID = 0)
        : Comm(parent, commID), serial(new QSerialPort(this)) {
        QObject::connect(serial, &QSerialPort::errorOccurred, this, &CommSerial::handleError);
        //QObject::connect(serial, &QSerialPort::readyRead, this, &CommSerial::handleReadyRead);
    }
    ~CommSerial() { serial->close(); }

protected:
    bool setConnInfoProc(QString connString, int connNum, void* connInfo) override {
        Q_UNUSED(connNum)
        Q_UNUSED(connInfo)
        m_connAvailable = false;

        serial->setPortName(connString);
        serial->setBaudRate(connNum);

        m_connAvailable = true;
        return m_connAvailable;
    }

    bool connectProc(quint32 timeout = INFINITE) override {

        if (!m_connAvailable) return false;

        if (serial->isOpen())
            return true;
        return serial->open(QIODevice::ReadWrite);
    }

    bool closeProc(quint32 timeout = INFINITE) const override {
        Q_UNUSED(timeout)

        if (serial->isOpen()) {
            serial->close();
        }
        if (serial->isOpen() && timeout && timeout < INFINITE)
            _sleep(timeout);

        return !serial->isOpen();
    }

    bool sendProc(QByteArray &data, quint32 timeout = INFINITE) override {
        m_bytesSent = serial->write(data);
        if (timeout)
            serial->waitForBytesWritten(timeout);
        return m_bytesSent == data.size();
    }

    bool inboxProc(quint32 timeout = IGNORE) override {
        if (timeout)
            serial->waitForReadyRead(timeout);
        m_bytesInbox = serial->bytesAvailable();
        return m_bytesInbox > 0;
    }

    bool recvProc(QByteArray &buffer, quint32 timeout = INFINITE,
                  quint32 expectedBytes = IGNORE) override {
        if (timeout && timeout < INFINITE) {
            doEvents();
            serial->waitForReadyRead(timeout);
        }
        int bytesRx = serial->bytesAvailable();

        while (bytesRx) {
            buffer += serial->readAll();

            if (expectedBytes && buffer.size() >= expectedBytes)
                break;

            if (timeout && timeout < INFINITE) {
                doEvents();
                serial->waitForReadyRead(timeout);
            }
            bytesRx = serial->bytesAvailable();
        }
        m_bytesRecv = buffer.size();
        return (bool)m_bytesRecv;
    }

    bool checkConnProc() override {
        return serial->isOpen();
    }

    static QList<QSerialPortInfo> getAvailablePorts() {
        return QSerialPortInfo::availablePorts();
    }

private:
    // Config hConfig;
    // Config *m_connInfo = nullptr;
    QSerialPort *serial = nullptr;
    bool m_connAvailable = false;
    QList<QSerialPortInfo> comports;

    void handleError(QSerialPort::SerialPortError err) {
        if (m_status == eStatus::inboxing || m_status == eStatus::recving)
            return;
        if (err == 12)
            return;
        if (!this->m_isClosed && err != QSerialPort::NoError) {
            this->setAlert(nullptr, (int)err, serial->errorString());
        }
    }

//     void handleReadyRead() {
//         if (serial->bytesAvailable()) {
//             emit onProgress(this, eProgress::inbox, serial->bytesAvailable());
//             doEvents();
//         }
//     }

};

#endif // COMM_H
