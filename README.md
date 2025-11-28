# LiDAR_Lumomap_4CH
LiDAR_Lumomap_4CH
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

#include <QTableWidget>
#include <QKeyEvent>
#include <QApplication>
#include <QClipboard>
#include <QItemSelectionRange>

class CCopyTableWidget : public QTableWidget
{
    Q_OBJECT
public:
    CCopyTableWidget(QWidget* parent = nullptr) : QTableWidget(parent) {}

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->matches(QKeySequence::Copy)) {
            copySelectionToClipboard();
        }
        else {
            QTableWidget::keyPressEvent(event);
        }
    }

private:
    void copySelectionToClipboard()
    {
        QString textData;
        QList<QTableWidgetSelectionRange> ranges = selectedRanges();
        if (ranges.isEmpty()) return;

        for (int i = 0; i < ranges.count(); ++i) {
            const QTableWidgetSelectionRange& range = ranges.at(i);
            for (int row = range.topRow(); row <= range.bottomRow(); ++row)
            {
                for (int col = range.leftColumn(); col <= range.rightColumn(); ++col)
                {
                    QTableWidgetItem* item = this->item(row, col);
                    if (item) {
                        textData += item->text();
                    }
                    if (col < range.rightColumn()) {
                        textData += "\t";
                    }
                }
                if (row <= range.bottomRow()) {
                    textData += "\n";
                }
            }
        }
        QApplication::clipboard()->setText(textData);
    }
};
#include "CLumoMap"

CLumoMap::CLumoMap(QWidget *parent)
    : QWidget{parent}
{}

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

#include "CLumoMap.h"



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

#include <QtWidgets>
#include <QtGui>
#include <QtCore>
#include <QString>
#include <QMap>

class CLumoMap : public QWidget
{
    Q_OBJECT

public:

    struct HighlightData {
        QPointF pos;
        bool active;
    };

    CLumoMap(QWidget* parent, int channels, int fov, float resolution)
        : QWidget(parent), m_fov(fov), m_channels(channels), m_resolution(resolution)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumSize(800, 600);
        setCursor(Qt::CrossCursor);

        penThin = QPen(lineThin.color, lineThin.thickness, lineThin.pattern);
        penThick = QPen(lineThick.color, lineThick.thickness, lineThick.pattern);
        penGrid = QPen(lineThin.color, lineThin.thickness, lineThick.pattern);
        penFoV = QPen(Qt::darkRed, 1.0, Qt::SolidLine);
        penHighlight = QPen(Qt::yellow, 2.0, Qt::SolidLine);

        m_currentScanIndex = 0;

        m_fadeEnabled = true;
        m_angleOffset = 0.0f;
        m_isClockwise = true;
        m_distanceUnit = "cm";
    }
    ~CLumoMap() {}

    void lumos(const QVector<QPointF>& newScan)
    {
        if (m_fadeEnabled) {
            m_currentScanIndex = (m_currentScanIndex + 1) % 3;
        }
        else {
            m_currentScanIndex = 0;
            m_scanBuffer[1].clear();
            m_scanBuffer[2].clear();
        }
        m_scanBuffer[m_currentScanIndex] = newScan;

        QWidget::update();
        QCoreApplication::processEvents();
    }

    void setSettings(float pixelsPerMeter, int maxConcCircles)
    {
        m_pixelsPerMeter = pixelsPerMeter;
        m_maxConcCircles = maxConcCircles;
        QWidget::update();
    }

    void setHardwareProfile(int channels, int fov, float resolution) {
        m_channels = channels;
        m_fov = fov;
        m_resolution = resolution;
    }

    void setMapOrientation(float angleOffset, bool isCCW)
    {
        m_angleOffset = angleOffset;
        m_isClockwise = isCCW;
    }

    void setFadeEnabled(bool enabled)
    {
        m_fadeEnabled = enabled;
        update();
    }

    void setDistanceUnit(const QString& unit) {
        m_distanceUnit = unit;
    }

    void setHighlight(QPointF pos, Qt::GlobalColor color)
    {
        HighlightData data;
        data.pos = pos;
        data.active = true;

        m_highlights.insert(color, data);
        update();
    }

    void visibleHighlight(Qt::GlobalColor color, bool visible)
    {
        if (m_highlights.contains(color)) {
            m_highlights[color].active = visible;
            update();
        }
    }

    void clearHighlights() {
        m_highlights.clear();
        update();
    }

    void fadeAway(bool fadeEnabled) {
        if (!fadeEnabled) {
            onClearPoints();
        }
        else {
            m_currentScanIndex = (m_currentScanIndex + 1) % 3;
            m_scanBuffer[m_currentScanIndex].clear();
            update();
        }
    }

    void setVisibleLayer(int layerIndex, bool value) {
        m_visibleLayer[layerIndex] = value;
        update();
    }

    float getZoomRate() const { return m_zoomRate; }
    void setZoomRate(float rate) {
        m_zoomRate = rate;
        penThin = QPen(lineThin.color, lineThin.thickness / m_zoomRate, lineThin.pattern);
        penThick = QPen(lineThick.color, lineThick.thickness / m_zoomRate, lineThick.pattern);
        penGrid = QPen(lineThin.color, lineThin.thickness / m_zoomRate, lineThick.pattern);
        penFoV = QPen(penFoV.color(), 1.0 / m_zoomRate, penFoV.style());
        penHighlight.setWidthF(2.0 / m_zoomRate);
        update();
    }

    QPointF getCenterOffset() const { return m_centerOffset; }
    void setCenterOffset(const QPointF& offset) {
        m_centerOffset = offset;
        update();
    }

public slots:
    void onClearPoints() {
        m_scanBuffer[0].clear();
        m_scanBuffer[1].clear();
        m_scanBuffer[2].clear();
        update();
    }

protected:
    typedef struct distanceInfo {
        QString info;
        QRect textRect;
    } distanceInfo;

    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), Qt::black);

        painter.translate(m_centerPoint + m_centerOffset);
        painter.scale(m_zoomRate, m_zoomRate);

        drawCrosshair(painter);
        drawConcCircles(painter);
        drawFieldOfView(painter);
        drawLidarPoints(painter);
        drawHighlight(painter);
        drawInfo();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_mousePressPos = m_lastMousePos = event->pos();
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton) == 0)
            return;

        QPoint currPos = event->pos();
        if (currPos.x() == m_lastMousePos.x() && currPos.y() == m_lastMousePos.y())
            return;

        m_centerOffset += (currPos - m_lastMousePos);
        m_lastMousePos = currPos;
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override 
    {
        if (event->button() == Qt::LeftButton) {
            if (m_mousePressPos == m_lastMousePos) {
                makeInfo(m_lastMousePos, event);
                update();
            }
        }
    }

    void wheelEvent(QWheelEvent* event) override
    {
        float numSteps = event->delta() / (8.0f * 20.0f);
        float scaleFactor = std::pow(1.125f, numSteps);

        QPointF mousePos = event->position();
        QPointF scenePos = (mousePos - (m_centerPoint + m_centerOffset)) / m_zoomRate;

        float newZoomRate = m_zoomRate * scaleFactor;
        QPointF newOffset = mousePos - (scenePos * newZoomRate) - m_centerPoint;

        setCenterOffset(newOffset);
        setZoomRate(newZoomRate);

        update();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        m_centerPoint = QPointF(width() / 2, height() / 2);
        update();
    }

private:

    QColor channelColor[5][3] = {
        // Ch 1: Green
        {QColor(0, 255, 0, 255),   QColor(0, 255, 0, 120),   QColor(0, 255, 0, 80)},
        // Ch 2: Yellow
        {QColor(255, 255, 0, 255), QColor(255, 255, 0, 120), QColor(255, 255, 0, 80)},
        // Ch 3: Cyan
        {QColor(0, 255, 255, 255), QColor(0, 255, 255, 120), QColor(0, 255, 255, 80)},
        // Ch 4: Magenta
        {QColor(255, 0, 255, 255), QColor(255, 0, 255, 120), QColor(255, 0, 255, 80)},
        // Ch 5: Red
        {QColor(255, 0, 0, 255),   QColor(255, 0, 0, 120),   QColor(255, 0, 0, 80)},
    };

    void drawLidarPoints(QPainter& painter)
    {
        const QVector<QPointF>& scan1 = m_scanBuffer[m_currentScanIndex];

        if (!m_fadeEnabled) {
            drawScan(painter, scan1, 0);
            return;
        }

        const QVector<QPointF>& scan2 = m_scanBuffer[(m_currentScanIndex + 2) % 3];
        const QVector<QPointF>& scan3 = m_scanBuffer[(m_currentScanIndex + 1) % 3];

        drawScan(painter, scan3, 2);
        drawScan(painter, scan2, 1);
        drawScan(painter, scan1, 0);
    }

    void drawScan(QPainter& painter, const QVector<QPointF>& scan, int colorStep)
    {
        if (scan.isEmpty()) return;

        if (m_channels == 1) {
            painter.setPen(QPen(channelColor[0][colorStep], m_PointSize / m_zoomRate));
            for (const QPointF& point : qAsConst(scan)) {
                painter.drawPoint(point);
            }
        }
        else {
            int channel = 1;
            for (const QPointF& point : qAsConst(scan)) {
                if (m_visibleLayer[channel - 1]) {
                    painter.setPen(QPen(channelColor[channel - 1][colorStep], m_PointSize / m_zoomRate));
                    painter.drawPoint(point);
                }
                channel++;
                if (channel > m_channels) channel = 1;
            }
        }
    }


    void drawCrosshair(QPainter& painter)
    {
        painter.setPen(penGrid);
        qreal absX = abs(m_centerOffset.x());
        qreal absY = abs(m_centerOffset.y());
        qreal absW = absX >= absY ? absX : absY;
        float m_sceneSizeMax = (absW + width()) / m_zoomRate;

        painter.drawLine(-m_sceneSizeMax, 0, m_sceneSizeMax, 0);
        painter.drawLine(0, -m_sceneSizeMax, 0, m_sceneSizeMax);
    }

    void drawFieldOfView(QPainter& painter)
    {
        painter.setPen(penFoV);

        qreal absX = abs(m_centerOffset.x());
        qreal absY = abs(m_centerOffset.y());
        qreal absW = absX >= absY ? absX : absY;
        float sceneSize = (absW + width()) / m_zoomRate;
        if ((absW + height()) / m_zoomRate > sceneSize)
            sceneSize = (absW + height()) / m_zoomRate;

        float radStart = m_angleOffset * M_PI / 180.0;
        float x1 = sceneSize * std::cos(radStart);
        float y1 = -sceneSize * std::sin(radStart);
        painter.drawLine(QPointF(0, 0), QPointF(x1, y1));

        if (m_fov < 360) {
            float endAngle = m_isClockwise ? (m_angleOffset - m_fov) : (m_angleOffset + m_fov);
            float radEnd = endAngle * M_PI / 180.0;
            float x2 = sceneSize * std::cos(radEnd);
            float y2 = -sceneSize * std::sin(radEnd);
            painter.drawLine(QPointF(0, 0), QPointF(x2, y2));
        }
    }

    void drawHighlight(QPainter& painter)
    {
        if (m_highlights.isEmpty()) return;

        float crossSize = 10.0f / m_zoomRate;

        QMapIterator<Qt::GlobalColor, HighlightData> i(m_highlights);
        while (i.hasNext()) {
            i.next();
            const HighlightData& data = i.value();

            if (!data.active) continue;

            // Ÿ�Ժ� ���� ���� (������ ���� ���)
            penHighlight.setColor(i.key());
            penHighlight.setWidthF(2.0 / m_zoomRate);
            painter.setPen(penHighlight);

            painter.drawLine(data.pos - QPointF(crossSize, 0), data.pos + QPointF(crossSize, 0));
            painter.drawLine(data.pos - QPointF(0, crossSize), data.pos + QPointF(0, crossSize));
        }
    }

    void drawConcCircles(QPainter& painter)
    {
        qreal absX = abs(m_centerOffset.x());
        qreal absY = abs(m_centerOffset.y());
        qreal absW = absX >= absY ? absX : absY;
        float sceneSize = (absW + width()) / m_zoomRate;
        if ((absW + height()) / m_zoomRate > sceneSize)
            sceneSize = (absW + height()) / m_zoomRate;

        int maxVisibleMeter = (int)(sceneSize / m_pixelsPerMeter) + 1;

        for (int i = 1; i <= maxVisibleMeter; ++i) {
            float radius = i * m_pixelsPerMeter;
            int gridType = (i % m_concCircleStep);

            if (gridType == 0) {
                painter.setPen(penThick);
            }
            else if (i <= m_maxConcCircles) {
                painter.setPen(penThin);
            }
            else {
                continue;
            }

            painter.drawEllipse(QPointF(0, 0), radius, radius);
        }
    }

    void makeInfo(QPointF mousePos, QMouseEvent* event)
    {
        QPointF sceneMousePos = (mousePos - (m_centerPoint + m_centerOffset)) / m_zoomRate;
        float dx = sceneMousePos.x();
        float dy = sceneMousePos.y();
        float distance = std::sqrt(dx * dx + dy * dy);

        float mapAngle = std::atan2(-dy, dx) * 180.0 / M_PI;

        float rawAngle = mapAngle - m_angleOffset;
        if (m_isClockwise) rawAngle = -rawAngle;

        float distMeter = distance / m_pixelsPerMeter;
        float distValue = distMeter;

        if (m_distanceUnit == "cm") distValue *= 100.0f;
        else if (m_distanceUnit == "mm") distValue *= 1000.0f;

        distanceInfo* distInfo;
        if (event->modifiers() == Qt::ControlModifier) {
            distInfo = &fixedInfo;
            setHighlight(sceneMousePos, Qt::yellow);
        }
        else if (event->modifiers() == Qt::NoModifier) {
            distInfo = &nomalInfo;
        }
        else {
            fixedInfo.info = "";
            nomalInfo.info = "";
            clearHighlights();
            return;
        }
        distInfo->info = QString("Dist: %1 %2\nAngle: %3\u00B0")
            .arg(QLocale(QLocale::English).toString(distValue, 'f', 1))
            .arg(m_distanceUnit)
            .arg(QLocale(QLocale::English).toString(rawAngle, 'f', 2));
    }

    void drawInfo()
    {
        QPainter painter(this);
        QRect textRect;
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(QFont("Arial", 12));
        QTextOption textOption;
        textOption.setWrapMode(QTextOption::WordWrap);

        if (nomalInfo.info.length()) {
            painter.setPen(Qt::white);
            textRect = QRect(10, 10, 200, 60);
            painter.drawText(textRect, nomalInfo.info, textOption);
        }
        if (fixedInfo.info.length()) {
            painter.setPen(Qt::yellow);
            textRect = QRect(10, 60, 200, 100);
            painter.drawText(textRect, fixedInfo.info, textOption);
        }
    }

    QVector<QPointF> m_scanBuffer[3];
    int m_currentScanIndex;

    bool    m_visibleLayer[8];
    QPointF m_centerOffset;
    QPointF m_centerPoint;
    QPointF m_lastMousePos;
    QPointF m_mousePressPos;
    int     m_PointSize = 2;

    float   m_zoomRate = 1.000f;
    float   m_pixelsPerMeter = 100.0f;
    int     m_maxConcCircles = 100;
    int     m_concCircleStep = 5;
    int     m_channels = 1;
    int     m_fov = 360;
    float   m_resolution = 0.3f;

    struct lineInfo {
        double thickness;
        Qt::GlobalColor color;
        Qt::PenStyle pattern;
    };
    lineInfo lineThin = { 0.5, Qt::gray, Qt::SolidLine };
    lineInfo lineThick = { 1, Qt::darkRed, Qt::SolidLine };
    QPen penThin;
    QPen penThick;
    QPen penGrid;
    QPen penFoV;
    distanceInfo nomalInfo, fixedInfo;

    QPen penHighlight;

    QMap<Qt::GlobalColor, HighlightData> m_highlights;
    bool    m_isHighlightActive; // (���� ���� Data.active ���)

    bool  m_fadeEnabled;
    float m_angleOffset;
    bool  m_isClockwise;
    QString m_distanceUnit;
};
QT += network
QT += core widgets gui
QT += concurrent
QT += serialport


# install
 INSTALLS += widget

HEADERS += \
    CCloudPoints.h \
    CLumoMap.h \
    CComm.h \
    CMainWin.h \
    CProtocol.h \
    crc16.h \
    CCopyTableWidget.h
SOURCES += \
           CLumoMap.cpp \
           CComm.cpp \
           CMainWin.cpp \
           main.cpp
FORMS +=
QMAKE_CXXFLAGS += /utf-8
CONFIG += c++11

CONFIG(debug, debug|release) {
    DESTDIR = antiGravity/debug
}
CONFIG(release, debug|release) {
    DESTDIR = antiGravity/release
}

<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE QtCreatorProject>
<!-- Written by QtCreator 13.0.2, 2025-11-11T18:10:47. -->
<qtcreator>
 <data>
  <variable>EnvironmentId</variable>
  <value type="QByteArray">{609e97e1-8a7e-4e5d-bc4a-3e792115eb77}</value>
 </data>
 <data>
  <variable>ProjectExplorer.Project.ActiveTarget</variable>
  <value type="qlonglong">0</value>
 </data>
 <data>
  <variable>ProjectExplorer.Project.EditorSettings</variable>
  <valuemap type="QVariantMap">
   <value type="bool" key="EditorConfiguration.AutoIndent">true</value>
   <value type="bool" key="EditorConfiguration.AutoSpacesForTabs">false</value>
   <value type="bool" key="EditorConfiguration.CamelCaseNavigation">true</value>
   <valuemap type="QVariantMap" key="EditorConfiguration.CodeStyle.0">
    <value type="QString" key="language">Cpp</value>
    <valuemap type="QVariantMap" key="value">
     <value type="QByteArray" key="CurrentPreferences">CppGlobal</value>
    </valuemap>
   </valuemap>
   <valuemap type="QVariantMap" key="EditorConfiguration.CodeStyle.1">
    <value type="QString" key="language">QmlJS</value>
    <valuemap type="QVariantMap" key="value">
     <value type="QByteArray" key="CurrentPreferences">QmlJSGlobal</value>
    </valuemap>
   </valuemap>
   <value type="qlonglong" key="EditorConfiguration.CodeStyle.Count">2</value>
   <value type="QByteArray" key="EditorConfiguration.Codec">UTF-8</value>
   <value type="bool" key="EditorConfiguration.ConstrainTooltips">false</value>
   <value type="int" key="EditorConfiguration.IndentSize">4</value>
   <value type="bool" key="EditorConfiguration.KeyboardTooltips">false</value>
   <value type="int" key="EditorConfiguration.MarginColumn">80</value>
   <value type="bool" key="EditorConfiguration.MouseHiding">true</value>
   <value type="bool" key="EditorConfiguration.MouseNavigation">true</value>
   <value type="int" key="EditorConfiguration.PaddingMode">1</value>
   <value type="int" key="EditorConfiguration.PreferAfterWhitespaceComments">0</value>
   <value type="bool" key="EditorConfiguration.PreferSingleLineComments">false</value>
   <value type="bool" key="EditorConfiguration.ScrollWheelZooming">true</value>
   <value type="bool" key="EditorConfiguration.ShowMargin">false</value>
   <value type="int" key="EditorConfiguration.SmartBackspaceBehavior">2</value>
   <value type="bool" key="EditorConfiguration.SmartSelectionChanging">true</value>
   <value type="bool" key="EditorConfiguration.SpacesForTabs">true</value>
   <value type="int" key="EditorConfiguration.TabKeyBehavior">0</value>
   <value type="int" key="EditorConfiguration.TabSize">8</value>
   <value type="bool" key="EditorConfiguration.UseGlobal">true</value>
   <value type="bool" key="EditorConfiguration.UseIndenter">false</value>
   <value type="int" key="EditorConfiguration.Utf8BomBehavior">1</value>
   <value type="bool" key="EditorConfiguration.addFinalNewLine">true</value>
   <value type="bool" key="EditorConfiguration.cleanIndentation">true</value>
   <value type="bool" key="EditorConfiguration.cleanWhitespace">true</value>
   <value type="QString" key="EditorConfiguration.ignoreFileTypes">*.md, *.MD, Makefile</value>
   <value type="bool" key="EditorConfiguration.inEntireDocument">false</value>
   <value type="bool" key="EditorConfiguration.skipTrailingWhitespace">true</value>
   <value type="bool" key="EditorConfiguration.tintMarginArea">true</value>
  </valuemap>
 </data>
 <data>
  <variable>ProjectExplorer.Project.PluginSettings</variable>
  <valuemap type="QVariantMap">
   <valuemap type="QVariantMap" key="AutoTest.ActiveFrameworks">
    <value type="bool" key="AutoTest.Framework.Boost">true</value>
    <value type="bool" key="AutoTest.Framework.CTest">false</value>
    <value type="bool" key="AutoTest.Framework.Catch">true</value>
    <value type="bool" key="AutoTest.Framework.GTest">true</value>
    <value type="bool" key="AutoTest.Framework.QtQuickTest">true</value>
    <value type="bool" key="AutoTest.Framework.QtTest">true</value>
   </valuemap>
   <valuemap type="QVariantMap" key="AutoTest.CheckStates"/>
   <value type="int" key="AutoTest.RunAfterBuild">0</value>
   <value type="bool" key="AutoTest.UseGlobal">true</value>
   <valuemap type="QVariantMap" key="ClangTools">
    <value type="bool" key="ClangTools.AnalyzeOpenFiles">true</value>
    <value type="bool" key="ClangTools.BuildBeforeAnalysis">true</value>
    <value type="QString" key="ClangTools.DiagnosticConfig">Builtin.DefaultTidyAndClazy</value>
    <value type="int" key="ClangTools.ParallelJobs">8</value>
    <value type="bool" key="ClangTools.PreferConfigFile">true</value>
    <valuelist type="QVariantList" key="ClangTools.SelectedDirs"/>
    <valuelist type="QVariantList" key="ClangTools.SelectedFiles"/>
    <valuelist type="QVariantList" key="ClangTools.SuppressedDiagnostics"/>
    <value type="bool" key="ClangTools.UseGlobalSettings">true</value>
   </valuemap>
   <valuemap type="QVariantMap" key="ClangdSettings">
    <value type="bool" key="blockIndexing">true</value>
    <value type="bool" key="useGlobalSettings">true</value>
   </valuemap>
  </valuemap>
 </data>
 <data>
  <variable>ProjectExplorer.Project.Target.0</variable>
  <valuemap type="QVariantMap">
   <value type="QString" key="DeviceType">Desktop</value>
   <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Desktop Qt 5.15.0 MSVC2019 64bit</value>
   <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Desktop Qt 5.15.0 MSVC2019 64bit</value>
   <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">qt.qt5.5150.win64_msvc2019_64_kit</value>
   <value type="qlonglong" key="ProjectExplorer.Target.ActiveBuildConfiguration">0</value>
   <value type="qlonglong" key="ProjectExplorer.Target.ActiveDeployConfiguration">0</value>
   <value type="qlonglong" key="ProjectExplorer.Target.ActiveRunConfiguration">0</value>
   <valuemap type="QVariantMap" key="ProjectExplorer.Target.BuildConfiguration.0">
    <value type="int" key="EnableQmlDebugging">0</value>
    <value type="QString" key="ProjectExplorer.BuildConfiguration.BuildDirectory">D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\build\Desktop_Qt_5_15_0_MSVC2019_64bit-Debug</value>
    <value type="QString" key="ProjectExplorer.BuildConfiguration.BuildDirectory.shadowDir">D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/build/Desktop_Qt_5_15_0_MSVC2019_64bit-Debug</value>
    <valuemap type="QVariantMap" key="ProjectExplorer.BuildConfiguration.BuildStepList.0">
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.0">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">QtProjectManager.QMakeBuildStep</value>
      <value type="bool" key="QtProjectManager.QMakeBuildStep.QMakeForced">false</value>
      <valuelist type="QVariantList" key="QtProjectManager.QMakeBuildStep.SelectedAbis"/>
     </valuemap>
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.1">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.MakeStep</value>
     </valuemap>
     <value type="qlonglong" key="ProjectExplorer.BuildStepList.StepsCount">2</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Build</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Build</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.BuildSteps.Build</value>
    </valuemap>
    <valuemap type="QVariantMap" key="ProjectExplorer.BuildConfiguration.BuildStepList.1">
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.0">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.MakeStep</value>
      <value type="QString" key="Qt4ProjectManager.MakeStep.MakeArguments">clean</value>
     </valuemap>
     <value type="qlonglong" key="ProjectExplorer.BuildStepList.StepsCount">1</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Clean</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Clean</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.BuildSteps.Clean</value>
    </valuemap>
    <value type="int" key="ProjectExplorer.BuildConfiguration.BuildStepListCount">2</value>
    <value type="bool" key="ProjectExplorer.BuildConfiguration.ClearSystemEnvironment">false</value>
    <valuelist type="QVariantList" key="ProjectExplorer.BuildConfiguration.CustomParsers"/>
    <value type="bool" key="ProjectExplorer.BuildConfiguration.ParseStandardOutput">false</value>
    <valuelist type="QVariantList" key="ProjectExplorer.BuildConfiguration.UserEnvironmentChanges"/>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Debug</value>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.Qt4BuildConfiguration</value>
    <value type="int" key="Qt4ProjectManager.Qt4BuildConfiguration.BuildConfiguration">2</value>
   </valuemap>
   <valuemap type="QVariantMap" key="ProjectExplorer.Target.BuildConfiguration.1">
    <value type="QString" key="ProjectExplorer.BuildConfiguration.BuildDirectory">D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\build\Desktop_Qt_5_15_0_MSVC2019_64bit-Release</value>
    <value type="QString" key="ProjectExplorer.BuildConfiguration.BuildDirectory.shadowDir">D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/build/Desktop_Qt_5_15_0_MSVC2019_64bit-Release</value>
    <valuemap type="QVariantMap" key="ProjectExplorer.BuildConfiguration.BuildStepList.0">
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.0">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">QtProjectManager.QMakeBuildStep</value>
      <value type="bool" key="QtProjectManager.QMakeBuildStep.QMakeForced">false</value>
      <valuelist type="QVariantList" key="QtProjectManager.QMakeBuildStep.SelectedAbis"/>
     </valuemap>
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.1">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.MakeStep</value>
     </valuemap>
     <value type="qlonglong" key="ProjectExplorer.BuildStepList.StepsCount">2</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Build</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Build</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.BuildSteps.Build</value>
    </valuemap>
    <valuemap type="QVariantMap" key="ProjectExplorer.BuildConfiguration.BuildStepList.1">
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.0">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.MakeStep</value>
      <value type="QString" key="Qt4ProjectManager.MakeStep.MakeArguments">clean</value>
     </valuemap>
     <value type="qlonglong" key="ProjectExplorer.BuildStepList.StepsCount">1</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Clean</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Clean</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.BuildSteps.Clean</value>
    </valuemap>
    <value type="int" key="ProjectExplorer.BuildConfiguration.BuildStepListCount">2</value>
    <value type="bool" key="ProjectExplorer.BuildConfiguration.ClearSystemEnvironment">false</value>
    <valuelist type="QVariantList" key="ProjectExplorer.BuildConfiguration.CustomParsers"/>
    <value type="bool" key="ProjectExplorer.BuildConfiguration.ParseStandardOutput">false</value>
    <valuelist type="QVariantList" key="ProjectExplorer.BuildConfiguration.UserEnvironmentChanges"/>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Release</value>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.Qt4BuildConfiguration</value>
    <value type="int" key="Qt4ProjectManager.Qt4BuildConfiguration.BuildConfiguration">0</value>
    <value type="int" key="QtQuickCompiler">0</value>
   </valuemap>
   <valuemap type="QVariantMap" key="ProjectExplorer.Target.BuildConfiguration.2">
    <value type="int" key="EnableQmlDebugging">0</value>
    <value type="QString" key="ProjectExplorer.BuildConfiguration.BuildDirectory">D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\build\Desktop_Qt_5_15_0_MSVC2019_64bit-Profile</value>
    <value type="QString" key="ProjectExplorer.BuildConfiguration.BuildDirectory.shadowDir">D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/build/Desktop_Qt_5_15_0_MSVC2019_64bit-Profile</value>
    <valuemap type="QVariantMap" key="ProjectExplorer.BuildConfiguration.BuildStepList.0">
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.0">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">QtProjectManager.QMakeBuildStep</value>
      <value type="bool" key="QtProjectManager.QMakeBuildStep.QMakeForced">false</value>
      <valuelist type="QVariantList" key="QtProjectManager.QMakeBuildStep.SelectedAbis"/>
     </valuemap>
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.1">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.MakeStep</value>
     </valuemap>
     <value type="qlonglong" key="ProjectExplorer.BuildStepList.StepsCount">2</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Build</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Build</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.BuildSteps.Build</value>
    </valuemap>
    <valuemap type="QVariantMap" key="ProjectExplorer.BuildConfiguration.BuildStepList.1">
     <valuemap type="QVariantMap" key="ProjectExplorer.BuildStepList.Step.0">
      <value type="bool" key="ProjectExplorer.BuildStep.Enabled">true</value>
      <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.MakeStep</value>
      <value type="QString" key="Qt4ProjectManager.MakeStep.MakeArguments">clean</value>
     </valuemap>
     <value type="qlonglong" key="ProjectExplorer.BuildStepList.StepsCount">1</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Clean</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Clean</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.BuildSteps.Clean</value>
    </valuemap>
    <value type="int" key="ProjectExplorer.BuildConfiguration.BuildStepListCount">2</value>
    <value type="bool" key="ProjectExplorer.BuildConfiguration.ClearSystemEnvironment">false</value>
    <valuelist type="QVariantList" key="ProjectExplorer.BuildConfiguration.CustomParsers"/>
    <value type="bool" key="ProjectExplorer.BuildConfiguration.ParseStandardOutput">false</value>
    <valuelist type="QVariantList" key="ProjectExplorer.BuildConfiguration.UserEnvironmentChanges"/>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Profile</value>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.Qt4BuildConfiguration</value>
    <value type="int" key="Qt4ProjectManager.Qt4BuildConfiguration.BuildConfiguration">0</value>
    <value type="int" key="QtQuickCompiler">0</value>
    <value type="int" key="SeparateDebugInfo">0</value>
   </valuemap>
   <value type="qlonglong" key="ProjectExplorer.Target.BuildConfigurationCount">3</value>
   <valuemap type="QVariantMap" key="ProjectExplorer.Target.DeployConfiguration.0">
    <valuemap type="QVariantMap" key="ProjectExplorer.BuildConfiguration.BuildStepList.0">
     <value type="qlonglong" key="ProjectExplorer.BuildStepList.StepsCount">0</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DefaultDisplayName">Deploy</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">Deploy</value>
     <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.BuildSteps.Deploy</value>
    </valuemap>
    <value type="int" key="ProjectExplorer.BuildConfiguration.BuildStepListCount">1</value>
    <valuemap type="QVariantMap" key="ProjectExplorer.DeployConfiguration.CustomData"/>
    <value type="bool" key="ProjectExplorer.DeployConfiguration.CustomDataEnabled">false</value>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">ProjectExplorer.DefaultDeployConfiguration</value>
   </valuemap>
   <value type="qlonglong" key="ProjectExplorer.Target.DeployConfigurationCount">1</value>
   <valuemap type="QVariantMap" key="ProjectExplorer.Target.RunConfiguration.0">
    <value type="bool" key="Analyzer.Perf.Settings.UseGlobalSettings">true</value>
    <value type="bool" key="Analyzer.QmlProfiler.Settings.UseGlobalSettings">true</value>
    <value type="int" key="Analyzer.Valgrind.Callgrind.CostFormat">0</value>
    <value type="bool" key="Analyzer.Valgrind.Settings.UseGlobalSettings">true</value>
    <valuelist type="QVariantList" key="CustomOutputParsers"/>
    <value type="int" key="PE.EnvironmentAspect.Base">2</value>
    <valuelist type="QVariantList" key="PE.EnvironmentAspect.Changes"/>
    <value type="bool" key="PE.EnvironmentAspect.PrintOnRun">false</value>
    <value type="QString" key="PerfRecordArgsId">-e cpu-cycles --call-graph &quot;dwarf,4096&quot; -F 250</value>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.DisplayName">CLumoMap2</value>
    <value type="QString" key="ProjectExplorer.ProjectConfiguration.Id">Qt4ProjectManager.Qt4RunConfiguration:D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/CLumoMap.pro</value>
    <value type="QString" key="ProjectExplorer.RunConfiguration.BuildKey">D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/CLumoMap.pro</value>
    <value type="bool" key="ProjectExplorer.RunConfiguration.Customized">true</value>
    <value type="bool" key="RunConfiguration.UseCppDebuggerAuto">true</value>
    <value type="bool" key="RunConfiguration.UseLibrarySearchPath">true</value>
    <value type="bool" key="RunConfiguration.UseQmlDebuggerAuto">true</value>
   </valuemap>
   <value type="qlonglong" key="ProjectExplorer.Target.RunConfigurationCount">1</value>
  </valuemap>
 </data>
 <data>
  <variable>ProjectExplorer.Project.TargetCount</variable>
  <value type="qlonglong">1</value>
 </data>
 <data>
  <variable>ProjectExplorer.Project.Updater.FileVersion</variable>
  <value type="int">22</value>
 </data>
 <data>
  <variable>Version</variable>
  <value type="int">22</value>
 </data>
</qtcreator>

<FlowDocument xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" FontFamily="Segoe UI" FontSize="12">
  <FlowDocument.Tag><![CDATA[{
  "timestamp": "2024-11-21 14:16:07",
  "files": {
    "D:\\DeveSW\\_TI-DSP\\_Study_Develope\\___zsc_Libraries\\UI_Project\\LiDAR\\LiDAR_CLumoMap\\CLumoMap.vcxproj": {
      "before": "7V3pc9pIFp/PW7X/g4rKTNlbBQIb23HATGHAXiqAHcDx7BRVlJDaoI2uSC0fs7Of9h/f15cOJGEcA5kkmtRYovv13f2O32t1W//76af6r4+mId0j19Nt66xQKZULErJUW9Ot+VnBx3fFt4VfG3//W/3atf+NVCy10Z3iG3isuHOEvbPCua8bWkGCXCz4tcDYeSfLnrpApuKVTF11bc++wyXVNmUN3SPDdpArm96MJJMPyuXDAuQuSfUuRuala/uO1FNmyDgr8AJbtnWnz31XwVBBjxEDeVqs1LVUw9fQWWGIDKR46M/H46pIAWliWTU4TV2OB5PKkP/q14aC72zXbEAmdTn4xeIhgPVHWuJnKtdGM3++smqUYuMVq8tBH7MuhybAYOCnWLdfGvZMMRIdfenrWuM/5yfV9knn6KJ40Tw/KR5WmuVi8/TksHhx2j5uH17AcJYr/w36hqbh/TW0bTxQTOQ5iooarZ5v2n3FqcvxcE78Hj092K7W+ICrH0f3MCfrsgjiFLe6pdkPHpuFYmw+slncqJRL5dLBcaUMz7q8knRVdn3dekGOEWqSKe2EsHf5HDcd28USnzpnhTd7H1t8IV0reLE/6QfrpeU4Jb7USg6Mk1eQ5LRRg/mn6WRpnBV+URzbq73Zi03J/T/f7In+2WcUZ2fsGVkkLKAgll4sB7GAggUxtm3DQ7hxX6kehAtDhPIevfKx4+O27gLPsN2nhstW5KQuL8fwBM1xD7pQN/U/kNca+tZYN1FPn7mK+3TjKXPUuINZierys3Q8v9ZCcRUVI3cEVR3YGB6woqKBgjDKYMZPDmo0HcfQVcpwlhYhjebpuhZkbiJNVzBKa2d6PE987eomtIz1RWQ5xMMJcepEiq/cV8yBgBdtfwZohO199+MvWrnN0e++jI3E2QdLHGP5nUeMLCL/YY1gkPshr4kSv2KWZXMaMZFHCwTaRMBqki288ZBLZAWstGU2GWFwJR/IBL+MVBg96h729tgkf7P34sxYwv2AB8uRjomydtatkYJFiRAUajEreHLWevxu+kk0JDYByXj0FaIqBj0co6NdDKwQ2CWMv/SK/k3OxLhkguxFYbCCt1kYy14UxrQAoiBtuHWhphUWIQrtzi3bRWwqc1G74dKx64PITilHVKGnW59Aa3eRiSysGBsunqsMS4WIsl8/m5ZXa0Nwf5a1KOj1MymzoG3MokRhO5xDibJXz6BU5YhYOaA6gzJJ9OJXs+QUliE0R6Nlm45uID7QwKGaGtPGFYPbosBCqAasI69RqrXfTT7gyVGpclQqT0zvXj0oV06nx9WJzizXZwkg+a2uEct7HVIwwNYhaw4ue511CAcIg132aR1S4NGq77qwqtehHiFXV4xrMI7WoW4B16pxg6ImP9t15ifPQao3edCtw4Mi6fXaz3urBmofrIxV48iGPzbaVw6FJxrF39V37r1i+KileFgiP3XL0C1EXz3s6ioewV9QsmgIXrg21Gs+QA/0t4vuEHSais7BwIVwGjidqo7he+R/SaaQjFR8OKyCfcueb0/hWa2enh7TZ/VIPE/477dStMG8rrFWivqHTfM8ZM4M5PZAc4KK9GxmEEXEZTOLJMjkHBrnoa5FYBRqTgkjLhkRpKEcIJLkgqYFC84CSZIRGSbWPQUqPYLx1u909VZxLdLVjerRyUmtWj0+gZFvp9NAd2TEhKu786giOtD/VCwNRnXeGD1Zal1Ohgc1upoR1OwCmAQR7pHeW4oIE0ABYASz7uorj9AWpIHVHA0OiK9d5Li2ijzPdkOW5zWmt91B++p2VLsZdFtX7U5tKl4g4vCgNu0Mmue9zrTz27gzaHfa02avezmA52h8NWxedmpAdlytQdT5zWXtw3g6uJoG77fd9mVnPJr2uuck6vKmK14HnfHt1fC9+Nm6GrRuhsPOYCxCRp1ht9m7vhoGIa2rYYfG/ryX0RgYmIyYcGBCgrFN+lrMs0R4pOfsuauYbQUr54AUBgNEJUpqTJAUwAkYIgFONPqAhurjhYsUDWntXg8wrThBkHDkO44LYzXCiot951yxLOQ2mIRLjwuSjiF7fEvgiyluegRzxV2LohEseXZ8kAVfDD0Cwjbo30MAx9gKYYGMFCCPJblWJwpU2Nkhb2wjB1kasCsi3TJlm6HPQGwdcblVgp/ZTJ7TguBai44LpLVoQ4m0FnkoktYiJzLpecLP2FR0i9J5C2QYhwf0Pcqdo30aY9HRiLTB4Bp8VOF4s/dh3O4O9yekz1vvJgD3Wp5niJ83WDe8ifk09T4b8IC/xaPSSengqAgiCVDvOB2Ai3gO0xdonTn5SzKN1jxZgVj9k9FprRCC6JfPvo1rcr856F50RuN255owqkHrX+8wQHQcwaQSnZnYkgXslQeHKOotg4lLoKeZtlWEKYBdAC95EuH1YD+PATSGfzzO8WeAAQLqPLY/IYtnfHx0ejQ7rlaqVVW9q2h3nNZQrLkPECWn+ofIQnDmpqsudAxiwHeXaFgrXy6dCdvqPCLVJ1oHMLl70LPgjfOSrNiAFVwiYDwEtiSQXETWCraZGR/kkGbSrVbVqU9l2RJbaaARg59JPTQUapHHm5gSEdQNjC+AnqkYeLPHLLH9ibBhSugR9IgISZBsCDLdJvCzRgQCMDnCqnlx6XFB0pE/Gz15YHY0+ISry2FQhOrL2H9djvDfel/XjHDhcBcBEQxUGtwAjje3iMKwHBNUo2MRFanjurbbWiD1k8d1q2R4kCQmJYhLJU1qRCpWHyLP9l0VJeyjUCD/wAoLaAgp/cPcc6EWRw1XOtLUN7oU8RqkJGFks4GuL8v9mIWT27PbtWc1wotza7Za2ak1S3sdnFI7tWUB2xYWxgxEzXdg1opuXMuo5Wb21zJqwWT9LizYJSszboYSZpLbokmcFWyWiC2qPW+xgeFKtrGsRcit0fWIQ3N0PfrQHl2Pnhika1Ayi5QR5iapF2rWIcqRm6QU8/Ri1rzolMBGyDI6mVWYFRskzzQ5WfrM6CCDL7RII5ZgbiwGZmTQq0mjkAHxyfAgScw4/Csbi1tTBDhU/uWI9hcZiKG9CHZbYMqFe2JbBAQrqY4j9lhkkPGtmWtQ9gHLBMDjeUoKekZKTuxEjW9MJRXzPWybdG9xtAWG7WvXtm5hr7QQm4WWrFSAfrzXGMcp7t5WK1qu8DxOTFudAp6voTuoTTakPdMtQkqApxhc2rVIVWNMlQWFMoiMGKBSG25OJlAfqakkFdvcZQRvwnsEr9R/RIKe8SAx0uMqPJkXCV4ifiT2K6KHswDuS+K0oTeJBcT9SSws7lESdMynJBWLKnPQu8U7Q7m33TPi/YVg7jOW2u8AJ7tHo1t5Ou4W26NreTrCvvY0JaFkq7o8nU7/8NQpA6/BzSGDtwvsN+LUA1iu3Ryyv1OBL8rcLy3HZodU7EJRH7DMXP+w/z1w/cvcPS1TMJu6pxn1hiu2qgqiO1bUkpNAG7gfZ438gBh05/UI6R6E9Ui5mr0ecahmr0cfqtnr0RM1GygZkA8uFm7bSwR+9sL93NJH3fNhfxOZXbo9OSgfHEzI6vZhk8rT5GNrQrdPT/ojeK1US9Vy6fDwbflE7AvhfoLtlUN2VV+0Nl9c03/UDR12Ok8+jtbMXdp7fHu8P+FIuvRex96kUhaJ4TXc3D/xVRc/0zcZ2aXnN/HNjWbnAS6PtI1mCWxiw20GwfylmYLD/+K3Ufv9pFp6G/Ro0IdxuSkVbYkzRyo6Y7FQB7LVnoq7UP71wQMDfrUNy7/+VUuKFb6oy7yksGhmF2xak1jVflAOeKGgE/C3sD6hOcqUhNf0SQL/j/dGjaF5MQH2V1VvEk3ZtXITNyKY/vEdaDF0CuQ6jIZyHSbYTZHrMLkOk6WBvkzL+lJ1A9TAQMuI6YGTHeowoXSMic2taTAJEbcr/SVRcHbLV+guoNaFWA7XZ7LgHQJQ7RTXoQXmgM4B6C05oJMDOpkYVYj+5IAOO9AiV4ZyZegHVIaowEwiOSR4awpQikeGqUC0Ml8Ju+EtXqH4bNgvldDGmD9vkaM1L9BccrQGPjbNPU4U1sk9TvAlTO5xAjddBnjBld7c4/QteZwCBSUKVmxVPUkI5q0rJ4kSU9q6QjF5ASIj9gLtEpQJysxxmRyXyTfawDkR2XuHclwm32jD9g/lG21+5I02gcxMQDMiZufojCj4K22uEcVDu1eoQlvHaEQ1cpjmJQ6mHKbJYRp6kjfsvslhmhymYUpeDtOwndAZW2C+pU01gVgk+koEvRDhW9NWEtAJBWtEsVvSVRKFprd4hZ7yAshGfJS1S8gmKDOHbHLIJodscsgGLmEB1S3/NmrlJ185ZPMjQzaBzExANiJma0pQ1oYaUfCW1KCUYmPfQ4nivzJkI6qRQzY5ZANXlsFBSvl3UPl3UPm33JNcX8n1Fau0WIJshLjcmraSQE8oZCOK3ZKukig0AtmIolfrKemQjbgWJHKoDZwYgW3VNuDLJ37FHzn/JkmoumrlOCRa4wwdcYx7WFikFdEzbaAlkdN04DYDcoumduGKE3jgEI3NXTLIj1ROlCK+DUseP59ymFFMcdxRUxJzgh1N9rqGREeEb1P7podiy23Y9hgIJPgbXxE7aMa2RyLCZb/lBbGDZmxgJMIvdNNkRXDIWUmd4chgkDOO6KnzbVv1yQVudTkICo4DTLDH15xckgIhrGbC297gkXnciLh06g7ubYFrH7yJBlc2TDTfNJ/Il1Lg61k+0WXbJ8ElpolqSMXzR3Y4UXBTCDkD7rOpfELkvDpxcJFlG/bcpndRPaj0DhqpeDFiF1bt4rKr3+EQsX5bk4q3h+zGqxfcfFV80MjtT5BOIxdAScUOnPqWcQvbM4MmHfwyx7XBTU8iT41AA/FDAEHnA/kHJ/aF5/Zs4AijxMCJw1Cl2BE92/kQLlF4SrM34riManSvYjjL62rD/OZLJ89XWPEp3PLbWfNX4Entt/9aSz51jm5n0aeM3S6XfUrxqY1fsfRB9G5U9ic40WrJn7S/kxbzy+7RhlvbyMWbwkqvR+6AlnrKDBlnheAmbXbZLqeld8qRE0Mb/wc=",
      "after": "7Rtrc+LI8T6nKv9BobzBpILkB7bXC+gKA/ZRAfwAr5MrqighDaCzpNFqRrZJLr80fyY9mockHvba3t1sbXxXZ2Cmp6en390z95/KTz/Vfn7wPe0ORcTFQb2wq+8UNBTY2HGDWb0Q02n5feFn849/qF1E+DdkU62Fplbs0aEVzRAl9cJJ7HpOQQMsAfyaUxp+MAxiz5FvEd137QgTPKW6jX3DQXfIwyGKDJ9M2DJjb2dnvwDYNa3Wocg/i3Acal1rgrx6QWzYxMHUncWRRYFAwoEBfN2s1glsL3ZQvXCFPGQR9PvDYUWugDU5VKaAqRn5YUYM+6d24Vl0iiPfBCQ1Q/3i8zDA+bFu8RPEtdAknj1KWgLxxQmrGYrHnOVwBBAGXeTYfubhieWtMPosdh3zXydHldZR++C0fNo4OSrv7zZ2yo3jo/3y6XHrsLV/CuLc2f234k2yRvDrCmPat3xEQstGZrMb+7hnhTUjPy6A/4YW9zhyzEv6cTC+298B/sshAXHjBg6+J1wLpWw+ci02d3f0HX3vcHcHPmvGo6CPoeu5wTMwZqAF0kvaI4l1aKAkjsv0t14obm2r8VKxXi8WtfMr7U/tB5dQsp2bHX2iOuVmViwVzK3t3iBBJzSv5UZgjzhalEYKY81QX7mImZqmMhaW5oc4oppAUy9sbX9sCnO+sOi8NOopq22GoS4MXg8BEyloxjrdyR8wZxOl37e2pYCSA2dss1iQxp5bIk1WmeAQY48gat7tVvZSU5Sjgt3nMQ1jqrhiRtwHjGrG8oxY0Bh2QWiu7/4TkeZVHAxdH3XdSWRFi2tizZA5BTtANeNJOIGvObciy6YoGgCpfUzhA2w4OygBsy5tuAiR2QhDz7UTF7dk9sm0WNcJALmPHNeiaN0518+LxReR68PJOC8yBpgfZ8CJ/S4rTU6PlhQ6J7wVeSt391Wl7TCn+sPLWp7ya0q68zz3kHcLq4uVQyqBPxs7PHsg0ptkHMcGF7i0BDzhehcksoZLZnYUkhdwVRnk4Fgf19KcV0pcHBjCJe0EhFqeZx7ouwf6ztgnd/bezu7xmKUE6awwMTgqdmIPEdPGEaoGiEIUu63OYrd67zosXaoSFLmWx/xv1caBHUcRCsBLpCs/0wBfedqMTX6PZ+XRSGP5Qr2gFAhc6imOA0g2TxAkZkjErHqhGROK/SQ2VpteE/uh66Gc+J+Or5uj8CXlyspisBB0DxEWIDSu7VZgA5m/uLN5QRuiB5oh+YNm49hztABTzcPg4JGWBvW/su+J+VQ1+Eiya99aJMBJegyLQT9s6i10pfQGPzUPwnz/XPrWfqAoYLl8agYiYmeBX2wZMl7LcDCYI1BrxZhV878GjWc5HsSr5cQikxfoMYCt8QlIpUXPxpM6CjDkVbqUVrHsSe4sOGVkWLXK6OfwLmNn/6eck4qSU1ImzJ7FSkOl1zk4aWeQrIDevMKP53JAQCURQ/z8Uog5KomYWydzXK+gOq2OUnRyg84sANfHFVQkq6/YiUYxJLhrcMrtum5wC1V1hHyIU5b3iq1EMr2EUO7zAklnbMuUeRFHI5G+QMrrkH4RCWcRf035Zvd5XLpr03zWIYCCD8oiVq5yo32Ow1ubRqmYLAQDDrnh8ILY8kTPBswxqWZdyJ7OUICg34OcU4jjZLSUujHjKlXzMFW9Kky9ari8CVR9t/3YJiWo6x6jgUf6HKXnYdKCMsu/2h+iO8uLUdMiVGM/3cBzA5R8JTRybTqAv5CDJiN0HuF7+NVH98nvCE0RpH02OoEmBowng+OxHXoxYf9pRtJ208r3+xXoYfDP98fwWakcHx8mn5UD+Xkkfr/XsgcWtOZOKelPj0YI8iceirrQfABCuixDAbFnSufGJhCF5AQOR1AnYK0yvlpY+uqEWpMoaWbJabIWauYAvNGGyXSxSywgehAi25269o0VBYzVZuXg6KhaqRwegeRb62GAHRtmUs1sP9goEfQvVuCAVGfmYBHYNWN1XFF0PmG5G1NWppsZ7i1NpAtgA2g7cHb1rAc4C3KgT5EdVsAXEYLs0IaUE0epaRJzfNPpt85vBtXrfqd53mpXx/ILTOzvVcftfuOk2x63/z5s91vt1rjR7Zz14XMwPL9qnLWrAHZYqcLUyfVZ9XI47p+P+fd32xv2BP5tmEn5lwIMMWOJ7KKsjGcOiGeR5bcsap1A01bxMXFQa2fUUujaACdl18bsQWPaHc4jZDnIaXW70F7MA6iFgzgMI2DpgFoRjcMTKwCXY3KHuX5OLR0CenrD+jpj2kgafFAlJm0avnzzvEIhdLbL+uFm8ncf+pRckfmgAk2OBMkRlz+vbbje8N02z3MU0E6SFZEMiywEp/JKvWALhShwwDExH0zmyPP293TPneQ8aRYo512yE+uwi2Ql6+ebH0bQnAwI8UZsG/h5TV2PjPzFmHzy4AP+lg/0I33voAwOFPrwebgQEzoDKQJsOGN/l4ld3TNH8ur0OsKl2ywYvUa/c9oeDFvtC2ZR/eY/PlDo3tWLQNz+XlELWL1aTLunovOsg9R8HJQhjtIIGplFdd1SPIRGNfxb1MJ4Ag1AaHIP8S0K6sXDg+ODyWFlt1Kx7emuMy1qnhXMYig668W/ALhUiEZkz10K3ieOkpnC86MAs7v2A7Jj5vrBSu8g02NBgOvXplmloDIQL7ttafcb5xWGdekn33zdjFq2nEk+mmBCIBfeFV3J8EvEEddMqE0goYSmcuLHtrZ5dlkayfxNRw8QrzIgatkVxA7MGssO82hgYszXiO3Wz6mlg3gyWBBIw0yhPjUjHcpAvcx/1YyM9dd6ruOlKi8a/cyzJe7sGvoIs4AFpuUZRUY7YKG4HUU4as6RfUtEDF8dV0tybo5dz6y6vZqRIax2hQiOIxuJvk5KbxpRvv/ACJFo7TGgxdjDdnomccjo1LPucGSyfiO7EEg6WnJQsVKkzqZuLKXIJcPH9hgCnIOmRJ9Dhcdz4nQfZfAtROzITRIeEygpsnz03XYHwgB16aKk6zpLfYR3yAIrKloL8HuuPUiEZOLEYEBlcqMKODkuK8SXKeY9UJtN5YFVOsCO9G6b/WR+tqTbIVwfJvgUCF8pRhM8/M4zTZuSiobPvLbUyVZaYuPlUJsrH75UoeOwm463MgdClOWJ2JyL6jJeKzXaWMMknIQ7o40ACsVqLfPiIgdyOZnTTiA2/AD1jmTjZ1U7ov76X1U737i0WSo/8vUJs+O3IiV5a/NWpCwyhVHOnb0VKewx1qYy5LVFCl//1WqUTG3wVj6owkKF1dUygbcAV8fVkly58D2XD6/upb2VDCw0sEcYUJd8PyVDWkEAbSrhT5+eNlm3hxUn8mpzA5h4AfkZkD3LDaAX8DSkD4BZqJUHn/n3n5K3Wdo9HDsX2A0olI2ZAyQyyMKxMz4GIE/3GIw8VxamKe+DMptBxkyxjb3chmsA7cjePUyBnj595t1Iuh3PZnP1s25PqHziAExj5WbSIGlhO2b3ojVDDSk/lbZVOwF0kMizbk5b8oVwkT37vBy2OlelkX9L4MqDjKbQfoZuHxk50LgbObHvL5jQc11avmcuj+BDaXHN1BS6VC+my/a08skDPB4V5E3cYPTJt24R64oVtHKAPTzDyd3WvZ00y7Xy6YBfgH2Ly7NfXa3cazla+Waf36A94yatfO+w2yRY57ALJa3c1tQ5nxCDtvfnGa32r7sa+9za5pfHIL18Q0ZwPxWHfFL0nMvWrJrIFEZb2kjgTTfiKclr9HHtkUD9BGbQOvFNbAoNrPSBlhzLDKW2J59r5M7wza0vf4f9uYL/FvaXp+x7t8DzPWaBP7IB5uXxVU0wv9UGv/IsI1yNj897+As3p+xJq8wSHn9jl756zL3L3PDmN/O/PcjHvpknefIlnXrqyF9JCUqSW2N2H2/+Fw=="
    },
    "D:\\DeveSW\\_TI-DSP\\_Study_Develope\\___zsc_Libraries\\UI_Project\\LiDAR\\LiDAR_CLumoMap\\CLumoMap.vcxproj.filters": {
      "before": "5Vddb5swFN3zpP4HxLvDZ74GSRUodJVWqdI07WXSRMxNcYVthk0Vadrv2d+c05bglk7N2qRSNQQPXPC5Pvf4Hszvo3fvwuM1LY1rqAXhbGY6A9s0gGGeE3Y5Mxu5QhPzeH70Pryo+RVgaajXmZiZhZTVB8sSuACaiQEluOaCr+QAc2rlcA0lr6C2qFg2pMwt17Y9U8EYRngmgZ7WvKlublUgJaWE2jhjuGxymJmnwKDOJOSGegLidpihjvALIz8aOMuBSbIiUM9/jp3kZKJOtIijKfLjZIqiKHFQYtuRZzu+nzjOr9DqDdzM5AYyWUtgG+5ijqsqwAFerwPKcVAEOawCnpcBUVcNIggt7eVbgNC6nfx/QOUjZLlS6WlJpt50OvQmNppMoxPkL20fTSZJhEbuMI2SoZtEabSrJEVQKFEKJUlBA8KUFgwHa5E/Q4q3Pv/PvKkx7FB/P43H3jh1UDxeDJHvjUdoYY9GyF147sl46C7cNN21/l1L6M1QVFfBMpNBJujmWj9DjTfMJrTuO1jf0OIy5rRS3tV5WqwidKDKqbnZnXXopXjoJ9YWqnWYbUDD/tRQfp5Vh4I/zwj7StiB4KlCfzH0Dpo0QnIabT5GWuXikjf5BSdMikHRl0b3jL40HeJWnC6kJ9lIfzD0VvyDJWjl30eC8u4jr1VH7Sskx7z89wq1YNvqt4EOHdfYGb0QeYe11e/3HJbN5Te1j/ge62vsUQN4sN/pL7QWviPa85caSsgEvGJGneFfvW3/1A6aSuPUttXrKHbwbBqztp9fh9mesz1msB23qga1T1FWPsBL2bfzJ1djh75ttS7UuYrebHtOed9sQuvur2v+Bw==",
      "after": "5ZZdb5swGIV7Xan/AXHv8JmvmaQKFLpIq9RpmnYzaSLmTXGFbWabCmnaf5/TNgktk9o17UU1BBcYeGyfYx/ek+Ojo+i0ZZV1A1JRwWe2N3BtCzgRBeVXM7vRazSxT+cnx9GlFNdAtGVe52pml1rXHxxHkRJYrgaMEimUWOsBEcwp4AYqUYN0mFo1tCoc33UD22AsK1pqYOdSNPXtrWnIaKVBWktOqqaAmX0OHGSuobDME1B3n1nmiL5y+rOBZQFc0zUFOf819tKziTnRIomnKEzSKYrj1EOp68aB64Vh6nm/I6f34WYkt8i01cA3c1dzUteYYNK2mAmCS1zAGouiwtRcEhSOnM7Ld4DIuRv8fzCVj5AXxqWnLZkG0+kwmLhoMo3PULhyQzSZpDEa+cMsTod+Gmfxcy0pcWlMKY0lJcOUGy84wa0qXmDFex//F9FIAs/QP8yScTDOPJSMF0MUBuMRWrijEfIXgX82HvoLP8ueq/9+S3Q3Q1lf41Wuca7Y5mpf4MY7nk3kPEywfqAlVSJYbbJrn2mJaWEDI2cnze6joyvF4zxxdqhtwuwaOuxPDRMXef1W+Iuc8m+UvxGeGfrB6Kc9+awvBOlollSiKS4F5VoNyr4p3bTomXLL2hrSA2+MflXi1t5XhW5NPQiaVPc/7Y6wpk7QgojqnzXYwbbK7hr2dCKJNzqQ/PRaSRqlBYs3hcu+6wJWzdV3Uxn8qCWYNDTLZkBWur90HpUvvdXToe9m+rcOJVSQK3iLLh9KEDn3td38Dw=="
    },
    "D:\\DeveSW\\_TI-DSP\\_Study_Develope\\___zsc_Libraries\\UI_Project\\LiDAR\\LiDAR_CLumoMap\\CLumoMap.vcxproj.user": {
      "before": "tZE9a8MwEIbTtdD/IETAyVDL/aCUYiVDCl06JG3obstnW0XWGekUUsgf7r+o7OItYzvee8f7PHDfF7NZvj52hh3AeY1W8ps04wyswkrbRvJA9fUjX6+uLvOtw09QxPaIxn9M95vgHFjiLLZYL3lL1D8J4VULXeHTTiuHHmtKFXaiggMY7MGJzpdBm0rcZtkdj+2MDf1xQ18vDkPPxLnwtSjBSL6jdyCKfp6zDdpK06iezBdxqnUTXDEky9N8sTUF1ei6ZSJl8gxlaE7Hh/vkFxmhO9pjUO3EHqExFufyXExno+I/CL6BgcLDXyqO0sPfVj8=",
      "after": "tZHNSsNAFIXrVvAdhqGQdmEm/iAimXRRwY2LVov7ZHKTjMzkhpk7pUJf1XdxEulGXOrynns454PzeTab5auDNWwPzmvsJb9KM86gV1jrvpU8UHN5z1fFxXm+cfgOitgO0fi3k38dnIOeOIspvZe8IxoehPCqA1v61Grl0GNDqUIratiDwQGcsL4K2tTiOstueExnbMyPH/p4chgGJn4Tn8sKjORbegWiyOc5W2Nfa5rQk/kiXo1ugytHZXmcLzampAadXSZSJo9QhfZ4uLtNvitj6ZZ2GFR36i5y8VOZ2MTJMMH9A9oLGCg9/A1cPuKOWxVf"
    }
  }
}]]></FlowDocument.Tag>
  <Section Margin="0,24" TextAlignment="Center">
    <Paragraph FontSize="24" FontWeight="Bold" Margin="12,0">
      <LineBreak />
      <Span Foreground="Gray">Qt Visual Studio Tools</Span>
    </Paragraph>
    <Paragraph FontSize="42" Margin="12,0" FontWeight="Bold">
      <Span TextDecorations="Underline">Project Format Conversion</Span>
    </Paragraph>
    <Paragraph Margin="12,8" FontSize="18">
      <Span>Report generated on 2024-11-21 14:16:07</Span>
    </Paragraph>
  </Section>
  <Section>
    <Paragraph FontSize="32" FontWeight="Bold" Margin="12,0">
      <Span>Files</Span>
    </Paragraph>
    <Paragraph Margin="24,12,0,0">
      <Span FontFamily="Consolas" FontSize="14" Background="WhiteSmoke"><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
      <LineBreak />
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj?before">[Before]</Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj?after">[After]</Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj?diff&amp;before&amp;after">
                        [Diff before/after]
                    </Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj?diff&amp;before&amp;current">
                        [Diff before/current]
                    </Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj?diff&amp;after&amp;current">
                        [Diff after/current]
                    </Hyperlink>
      <LineBreak />
    </Paragraph>
    <Paragraph Margin="24,12,0,0">
      <Span FontFamily="Consolas" FontSize="14" Background="WhiteSmoke"><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.filters]]></Span>
      <LineBreak />
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.filters?before">[Before]</Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.filters?after">[After]</Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.filters?diff&amp;before&amp;after">
                        [Diff before/after]
                    </Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.filters?diff&amp;before&amp;current">
                        [Diff before/current]
                    </Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.filters?diff&amp;after&amp;current">
                        [Diff after/current]
                    </Hyperlink>
      <LineBreak />
    </Paragraph>
    <Paragraph Margin="24,12,0,0">
      <Span FontFamily="Consolas" FontSize="14" Background="WhiteSmoke"><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.user]]></Span>
      <LineBreak />
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.user?before">[Before]</Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.user?after">[After]</Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.user?diff&amp;before&amp;after">
                        [Diff before/after]
                    </Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.user?diff&amp;before&amp;current">
                        [Diff before/current]
                    </Hyperlink>
      <Hyperlink NavigateUri="file://D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.user?diff&amp;after&amp;current">
                        [Diff after/current]
                    </Hyperlink>
      <LineBreak />
    </Paragraph>
  </Section>
  <Section>
    <Paragraph FontSize="32" FontWeight="Bold" Margin="12,0">
      <Span>Changes</Span>
    </Paragraph>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Replacing paths with "$(QTDIR)"]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>.;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtWidgets]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtGui]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtANGLE]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtNetwork]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtCore]]></Span><Span><![CDATA[;release;/include;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>.;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtWidgets]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtGui]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtANGLE]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtNetwork]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtConcurrent]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtSerialPort]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtCore]]></Span><Span><![CDATA[;release;/include;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\mkspecs\win32-msvc]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalDependencies>D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\lib\Qt5Widgets.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[15.0\msvc2019_64\lib\Qt5Gui.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[15.0\msvc2019_64\lib\Qt5Network.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[15.0\msvc2019_64\lib\Qt5Concurrent.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPort]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\lib\Qt5Core]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\lib\qtmain]]></Span><Span><![CDATA[.lib;shell32.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalDependencies>$(QTDIR)\lib\Qt5Widgets]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Gui]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Network]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Concurrent]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5SerialPort]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Core]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\qtmain]]></Span><Span><![CDATA[.lib;shell32.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>.;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtWidgets]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtGui]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtANGLE]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtNetwork]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort]]></Span><Span><![CDATA[;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\include\QtCore]]></Span><Span><![CDATA[;debug;/include;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>.;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtWidgets]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtGui]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtANGLE]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtNetwork]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtConcurrent]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtSerialPort]]></Span><Span><![CDATA[;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\include\QtCore]]></Span><Span><![CDATA[;debug;/include;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\mkspecs\win32-msvc]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalDependencies>D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\lib\Qt5Widgetsd.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[15.0\msvc2019_64\lib\Qt5Guid.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[15.0\msvc2019_64\lib\Qt5Networkd.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[15.0\msvc2019_64\lib\Qt5Concurrentd.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPortd]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\lib\Qt5Cored]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\lib\qtmaind]]></Span><Span><![CDATA[.lib;shell32.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalDependencies>$(QTDIR)\lib\Qt5Widgetsd]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Guid]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Networkd]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Concurrentd]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5SerialPortd]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\Qt5Cored]]></Span><Span><![CDATA[.lib;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\lib\qtmaind]]></Span><Span><![CDATA[.lib;shell32.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CCloudPoints.h;release\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CCloudPoints.h;release\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Release|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o release\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Release|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o release\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CCloudPoints.h;debug\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CCloudPoints.h;debug\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Debug|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o debug\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Debug|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o debug\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CComm.h;release\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CComm.h;release\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Release|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o release\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Release|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o release\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CComm.h;debug\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CComm.h;debug\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Debug|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o debug\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Debug|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o debug\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CLumoMap.h;release\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CLumoMap.h;release\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Release|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o release\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Release|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o release\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CLumoMap.h;debug\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CLumoMap.h;debug\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Debug|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o debug\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Debug|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o debug\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CMainWin.h;release\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CMainWin.h;release\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Release|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o release\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Release|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o release\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CMainWin.h;debug\moc_predefs.h;]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CMainWin.h;debug\moc_predefs.h;]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Debug|x64'">D:\Qt\5.15]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[0\msvc2019_64\bin\moc.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[-ID:/Qt/5.15.0/msvc2019_64/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o debug\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Debug|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs.h ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtWidgets]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtGui]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtANGLE]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtNetwork]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtConcurrent]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtSerialPort]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I$(QTDIR)/include/QtCore]]></Span><Span><![CDATA[ -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o debug\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Debug|x64'">D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Debug|x64'">$(QTDIR)\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -Zi -MDd -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E ]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp 2&gt;NUL &gt;debug\moc_predefs.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -Zi -MDd -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E ]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp 2&gt;NUL &gt;debug\moc_predefs.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Release|x64'">D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Release|x64'">$(QTDIR)\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -O2 -MD -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E ]]></Span><Span Background="LightCoral"><![CDATA[D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp 2&gt;NUL &gt;release\moc_predefs.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -O2 -MD -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E ]]></Span><Span Background="LightGreen"><![CDATA[$(QTDIR)\mkspecs\features\data\dummy]]></Span><Span><![CDATA[.cpp 2&gt;NUL &gt;release\moc_predefs.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Replacing paths with "."]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o release\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o release\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o debug\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o debug\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o release\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o release\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o debug\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o debug\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o release\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o release\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o debug\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o debug\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o release\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./release/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o release\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightCoral"><![CDATA[ D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap/debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span Background="LightCoral"><![CDATA[ -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap]]></Span><Span><![CDATA[ ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o debug\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include]]></Span><Span Background="LightGreen"><![CDATA[ ./debug/moc_predefs]]></Span><Span><![CDATA[.h -I$(QTDIR)/mkspecs/win32-msvc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[-I. ]]></Span><Span><![CDATA[-I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o debug\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Converting custom build steps to Qt/MSBuild items]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalIncludeDirectories>.;]]></Span><Span><![CDATA[$(QTDIR)\include;$(QTDIR)\include\QtWidgets;$(QTDIR)\include\QtGui;$(QTDIR)\include\QtANGLE;$(QTDIR)\include\QtNetwork;$(QTDIR)\include\QtConcurrent;$(QTDIR)\include\QtSerialPort;$(QTDIR)\include\QtCore;release;/include;$(QTDIR)\mkspecs\win32-msvc;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;]]></Span><Span><![CDATA[$(QTDIR)\include;$(QTDIR)\include\QtWidgets;$(QTDIR)\include\QtGui;$(QTDIR)\include\QtANGLE;$(QTDIR)\include\QtNetwork;$(QTDIR)\include\QtConcurrent;$(QTDIR)\include\QtSerialPort;$(QTDIR)\include\QtCore;release;/include;$(QTDIR)\mkspecs\win32-msvc;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <QTDIR>$(QTDIR)</QTDIR>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <OutputFile>$(Configuration)\moc_%(Filename).cpp</OutputFile>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <Define>UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB</Define>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <CompilerFlavor>msvc</CompilerFlavor>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <Include>./$(Configuration)/moc_predefs.h</Include>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <ExecutionDescription>Moc'ing %(Identity)...</ExecutionDescription>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <InputFile>%(FullPath)</InputFile>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <DynamicSource>output</DynamicSource>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <IncludePath>GeneratedFiles\$(ConfigurationName);GeneratedFiles;$(QTDIR)/mkspecs/win32-msvc;.;$(QTDIR)/include;$(QTDIR)/include/QtWidgets;$(QTDIR)/include/QtGui;$(QTDIR)/include/QtANGLE;$(QTDIR)/include/QtNetwork;$(QTDIR)/include/QtConcurrent;$(QTDIR)/include/QtSerialPort;$(QTDIR)/include/QtCore;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include;C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt;C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um</IncludePath>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    </QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalIncludeDirectories>.;]]></Span><Span><![CDATA[$(QTDIR)\include;$(QTDIR)\include\QtWidgets;$(QTDIR)\include\QtGui;$(QTDIR)\include\QtANGLE;$(QTDIR)\include\QtNetwork;$(QTDIR)\include\QtConcurrent;$(QTDIR)\include\QtSerialPort;$(QTDIR)\include\QtCore;debug;/include;$(QTDIR)\mkspecs\win32-msvc;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;]]></Span><Span><![CDATA[$(QTDIR)\include;$(QTDIR)\include\QtWidgets;$(QTDIR)\include\QtGui;$(QTDIR)\include\QtANGLE;$(QTDIR)\include\QtNetwork;$(QTDIR)\include\QtConcurrent;$(QTDIR)\include\QtSerialPort;$(QTDIR)\include\QtCore;debug;/include;$(QTDIR)\mkspecs\win32-msvc;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <QTDIR>$(QTDIR)</QTDIR>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <OutputFile>$(Configuration)\moc_%(Filename).cpp</OutputFile>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <Define>UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB</Define>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <CompilerFlavor>msvc</CompilerFlavor>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <Include>./$(Configuration)/moc_predefs.h</Include>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <ExecutionDescription>Moc'ing %(Identity)...</ExecutionDescription>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <InputFile>%(FullPath)</InputFile>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <DynamicSource>output</DynamicSource>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <IncludePath>GeneratedFiles\$(ConfigurationName);GeneratedFiles;$(QTDIR)/mkspecs/win32-msvc;.;$(QTDIR)/include;$(QTDIR)/include/QtWidgets;$(QTDIR)/include/QtGui;$(QTDIR)/include/QtANGLE;$(QTDIR)/include/QtNetwork;$(QTDIR)/include/QtConcurrent;$(QTDIR)/include/QtSerialPort;$(QTDIR)/include/QtCore;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include;C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt;C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um</IncludePath>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    </QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[<CustomBuild]]></Span><Span><![CDATA[ Include="CCloudPoints.]]></Span><Span Background="LightCoral"><![CDATA[h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ Include="CCloudPoints.]]></Span><Span Background="LightGreen"><![CDATA[h" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalInputs]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CCloudPoints]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[h;release\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[Include="CComm]]></Span><Span><![CDATA[.]]></Span><Span Background="LightGreen"><![CDATA[h" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<Command]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc]]></Span><Span><![CDATA[.]]></Span><Span Background="LightCoral"><![CDATA[exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[--compiler-flavor=msvc --include ./release/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o release\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[Include="CLumoMap]]></Span><Span><![CDATA[.]]></Span><Span Background="LightGreen"><![CDATA[h"]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[/>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<Message]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[Condition="'$(Configuration)|$(Platform)'=='Release|x64'">MOC]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightCoral"><![CDATA[CCloudPoints.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[Include="CMainWin.h"]]></Span><Span><![CDATA[ ]]></Span><Span Background="LightGreen"><![CDATA[/>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">release\moc_CCloudPoints.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CCloudPoints.h;debug\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include ./debug/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o debug\moc_CCloudPoints.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Message Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">MOC CCloudPoints.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">debug\moc_CCloudPoints.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <CustomBuild Include="CComm.h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CComm.h;release\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include ./release/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o release\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">MOC CComm.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">release\moc_CComm.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CComm.h;debug\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include ./debug/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o debug\moc_CComm.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Message Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">MOC CComm.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">debug\moc_CComm.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <CustomBuild Include="CLumoMap.h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CLumoMap.h;release\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include ./release/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o release\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">MOC CLumoMap.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">release\moc_CLumoMap.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CLumoMap.h;debug\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include ./debug/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o debug\moc_CLumoMap.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Message Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">MOC CLumoMap.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">debug\moc_CLumoMap.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <CustomBuild Include="CMainWin.h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CMainWin.h;release\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include ./release/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o release\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">MOC CMainWin.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">release\moc_CMainWin.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CMainWin.h;debug\moc_predefs.h;$(QTDIR)\bin\moc.exe;%(AdditionalInputs)</AdditionalInputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\bin\moc.exe  -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB --compiler-flavor=msvc --include ./debug/moc_predefs.h -I$(QTDIR)/mkspecs/win32-msvc -I. -I$(QTDIR)/include -I$(QTDIR)/include/QtWidgets -I$(QTDIR)/include/QtGui -I$(QTDIR)/include/QtANGLE -I$(QTDIR)/include/QtNetwork -I$(QTDIR)/include/QtConcurrent -I$(QTDIR)/include/QtSerialPort -I$(QTDIR)/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o debug\moc_CMainWin.cpp</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Message Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">MOC CMainWin.h</Message>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">debug\moc_CMainWin.cpp;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CCloudPoints.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Release|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CCloudPoints.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CComm.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Release|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CComm.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CLumoMap.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Release|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CLumoMap.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CMainWin.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Release|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CMainWin.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Release|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -Zi -MDd -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E $(QTDIR)\mkspecs\features\data\dummy.cpp 2&gt;NUL &gt;]]></Span><Span Background="LightCoral"><![CDATA[debug\moc_predefs]]></Span><Span><![CDATA[.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -Zi -MDd -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E $(QTDIR)\mkspecs\features\data\dummy.cpp 2&gt;NUL &gt;]]></Span><Span Background="LightGreen"><![CDATA[$(IntDir)\moc_predefs]]></Span><Span><![CDATA[.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Debug|x64'">debug\moc_predefs]]></Span><Span><![CDATA[.h;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Debug|x64'">$(IntDir)\moc_predefs]]></Span><Span><![CDATA[.h;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -O2 -MD -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E $(QTDIR)\mkspecs\features\data\dummy.cpp 2&gt;NUL &gt;]]></Span><Span Background="LightCoral"><![CDATA[release\moc_predefs]]></Span><Span><![CDATA[.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -O2 -MD -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E $(QTDIR)\mkspecs\features\data\dummy.cpp 2&gt;NUL &gt;]]></Span><Span Background="LightGreen"><![CDATA[$(IntDir)\moc_predefs]]></Span><Span><![CDATA[.h</Command>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Release|x64'">release\moc_predefs]]></Span><Span><![CDATA[.h;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <Outputs Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Release|x64'">$(IntDir)\moc_predefs]]></Span><Span><![CDATA[.h;%(Outputs)</Outputs>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">true</ExcludedFromBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj.filters]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[<CustomBuild]]></Span><Span><![CDATA[ Include="CCloudPoints.h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ Include="CCloudPoints.h">]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[</CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[</QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[<CustomBuild]]></Span><Span><![CDATA[ Include="CComm.h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ Include="CComm.h">]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[</CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[</QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[<CustomBuild]]></Span><Span><![CDATA[ Include="CLumoMap.h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ Include="CLumoMap.h">]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[</CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[</QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[<CustomBuild]]></Span><Span><![CDATA[ Include="CMainWin.h">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtMoc]]></Span><Span><![CDATA[ Include="CMainWin.h">]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[</CustomBuild>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[</QtMoc>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CCloudPoints.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CCloudPoints.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CComm.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CComm.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CLumoMap.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CLumoMap.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="debug\moc_CMainWin.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    <ClCompile Include="release\moc_CMainWin.cpp">]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Filter>Generated Files</Filter>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[    </ClCompile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Enabling multi-processor compilation]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <MultiProcessorCompilation>true</MultiProcessorCompilation>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <MultiProcessorCompilation>true</MultiProcessorCompilation>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Project format version]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightCoral"><![CDATA[<Keyword>Qt4VSv1.0</Keyword>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<Keyword>QtVS_v304</Keyword>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Fallback for QTMSBUILD environment variable]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <QtMsBuild Condition="'$(QtMsBuild)'=='' OR !Exists('$(QtMsBuild)\qt.targets')">$(MSBuildProjectDirectory)\QtMsBuild</QtMsBuild>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Default Qt properties]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  <Import Project="$(QtMsBuild)\qt_defaults.props" Condition="Exists('$(QtMsBuild)\qt_defaults.props')" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Qt build settings]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Release|x64'" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Warn if Qt/MSBuild is not found]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  <Target Name="QtMsBuildNotFound" BeforeTargets="CustomBuild;ClCompile" Condition="!Exists('$(QtMsBuild)\qt.targets') OR !Exists('$(QtMsBuild)\Qt.props')">]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <Message Importance="High" Text="QtMsBuild: could not locate qt.targets, qt.props; project may not build correctly." />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  </Target>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Qt property sheet]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <Import Project="$(QtMsBuild)\Qt.props" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <Import Project="$(QtMsBuild)\Qt.props" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Qt targets]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  <Import Project="$(QtMsBuild)\qt.targets" Condition="Exists('$(QtMsBuild)\qt.targets')" />]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Copying Qt build reference to QtInstall project property]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightCoral"><![CDATA['=='Release|x64'" />]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)]]></Span><Span Background="LightGreen"><![CDATA['=='Release|x64'">]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[  ]]></Span><Span Background="LightCoral"><![CDATA[<PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" />]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[    ]]></Span><Span Background="LightGreen"><![CDATA[<QtInstall>5.15.0_msvc2019_64</QtInstall>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  </PropertyGroup>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <QtInstall>5.15.0_msvc2019_64</QtInstall>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[  </PropertyGroup>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Removing Qt module macros from compiler properties]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;]]></Span><Span Background="LightCoral"><![CDATA[QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB;]]></Span><Span><![CDATA[%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;]]></Span><Span><![CDATA[%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;]]></Span><Span Background="LightCoral"><![CDATA[QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB;]]></Span><Span><![CDATA[%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;]]></Span><Span><![CDATA[%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Removing Qt module include paths from compiler properties]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;]]></Span><Span Background="LightCoral"><![CDATA[$(QTDIR)\include;$(QTDIR)\include\QtWidgets;$(QTDIR)\include\QtGui;$(QTDIR)\include\QtANGLE;$(QTDIR)\include\QtNetwork;$(QTDIR)\include\QtConcurrent;$(QTDIR)\include\QtSerialPort;$(QTDIR)\include\QtCore;]]></Span><Span><![CDATA[release;/include]]></Span><Span Background="LightCoral"><![CDATA[;$(QTDIR)\mkspecs\win32-msvc]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;]]></Span><Span><![CDATA[release;/include]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;]]></Span><Span Background="LightCoral"><![CDATA[$(QTDIR)\include;$(QTDIR)\include\QtWidgets;$(QTDIR)\include\QtGui;$(QTDIR)\include\QtANGLE;$(QTDIR)\include\QtNetwork;$(QTDIR)\include\QtConcurrent;$(QTDIR)\include\QtSerialPort;$(QTDIR)\include\QtCore;]]></Span><Span><![CDATA[debug;/include]]></Span><Span Background="LightCoral"><![CDATA[;$(QTDIR)\mkspecs\win32-msvc]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;]]></Span><Span><![CDATA[debug;/include]]></Span><Span><![CDATA[;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Removing Qt module libraries from linker properties]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalDependencies>$(QTDIR)\lib\Qt5Widgets.lib;$(QTDIR)\lib\Qt5Gui.lib;$(QTDIR)\lib\Qt5Network.lib;$(QTDIR)\lib\Qt5Concurrent.lib;$(QTDIR)\lib\Qt5SerialPort.lib;$(QTDIR)\lib\Qt5Core.lib;$(QTDIR)\lib\qtmain.lib;shell32]]></Span><Span><![CDATA[.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalDependencies>shell32]]></Span><Span><![CDATA[.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalDependencies>$(QTDIR)\lib\Qt5Widgetsd.lib;$(QTDIR)\lib\Qt5Guid.lib;$(QTDIR)\lib\Qt5Networkd.lib;$(QTDIR)\lib\Qt5Concurrentd.lib;$(QTDIR)\lib\Qt5SerialPortd.lib;$(QTDIR)\lib\Qt5Cored.lib;$(QTDIR)\lib\qtmaind.lib;shell32]]></Span><Span><![CDATA[.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalDependencies>shell32]]></Span><Span><![CDATA[.lib;%(AdditionalDependencies)</AdditionalDependencies>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Removing Qt lib path from linker properties]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalLibraryDirectories>$(QTDIR)\lib;C:\openssl\lib]]></Span><Span><![CDATA[;C:\Utils\my_sql\mysql-5.7.25-winx64\lib;C:\Utils\postgresql\pgsql\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalLibraryDirectories>C:\openssl\lib]]></Span><Span><![CDATA[;C:\Utils\my_sql\mysql-5.7.25-winx64\lib;C:\Utils\postgresql\pgsql\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightCoral"><![CDATA[<AdditionalLibraryDirectories>$(QTDIR)\lib;C:\openssl\lib]]></Span><Span><![CDATA[;C:\Utils\my_sql\mysql-5.7.25-winx64\lib;C:\Utils\postgresql\pgsql\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      ]]></Span><Span Background="LightGreen"><![CDATA[<AdditionalLibraryDirectories>C:\openssl\lib]]></Span><Span><![CDATA[;C:\Utils\my_sql\mysql-5.7.25-winx64\lib;C:\Utils\postgresql\pgsql\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Removing Qt module macros from resource compiler properties]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;]]></Span><Span Background="LightCoral"><![CDATA[QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB;]]></Span><Span><![CDATA[%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;]]></Span><Span><![CDATA[%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;]]></Span><Span Background="LightCoral"><![CDATA[QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB;]]></Span><Span><![CDATA[_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span><![CDATA[      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;]]></Span><Span><![CDATA[_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Adding Qt module names to QtModules project property]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <QtModules>core;network;gui;widgets;serialport;concurrent</QtModules>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[    <QtModules>core;network;gui;widgets;serialport;concurrent</QtModules>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Converting OutputFile to <tool>Dir and <tool>FileName]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <QtMocDir>$(Configuration)</QtMocDir>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <QtMocFileName>moc_%(Filename).cpp</QtMocFileName>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <QtMocDir>$(Configuration)</QtMocDir>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightGreen"><![CDATA[      <QtMocFileName>moc_%(Filename).cpp</QtMocFileName>]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
    <Paragraph FontSize="20" FontWeight="Bold" Margin="12">
      <Span><![CDATA[🡺 Removing old properties from project items]]></Span>
    </Paragraph>
    <Table CellSpacing="0" BorderBrush="Gray" BorderThickness="0.5">
      <Table.Columns>
        <TableColumn />
        <TableColumn />
      </Table.Columns>
      <TableRowGroup>
        <TableRow Background="Orange">
          <TableCell ColumnSpan="2" BorderThickness="0.5" BorderBrush="Gray">
            <Paragraph Margin="10,5" FontWeight="Bold">
              <Span><![CDATA[D:\DeveSW\_TI-DSP\_Study_Develope\___zsc_Libraries\UI_Project\LiDAR\LiDAR_CLumoMap\CLumoMap.vcxproj]]></Span>
            </Paragraph>
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <QTDIR>$(QTDIR)</QTDIR>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <OutputFile>$(Configuration)\moc_%(Filename).cpp</OutputFile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Define>UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB</Define>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <InputFile>%(FullPath)</InputFile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <IncludePath>GeneratedFiles\$(ConfigurationName);GeneratedFiles;$(QTDIR)/mkspecs/win32-msvc;.;$(QTDIR)/include;$(QTDIR)/include/QtWidgets;$(QTDIR)/include/QtGui;$(QTDIR)/include/QtANGLE;$(QTDIR)/include/QtNetwork;$(QTDIR)/include/QtConcurrent;$(QTDIR)/include/QtSerialPort;$(QTDIR)/include/QtCore;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include;C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt;C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um</IncludePath>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <QTDIR>$(QTDIR)</QTDIR>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <OutputFile>$(Configuration)\moc_%(Filename).cpp</OutputFile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <Define>UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;QT_WIDGETS_LIB;QT_GUI_LIB;QT_NETWORK_LIB;QT_CONCURRENT_LIB;QT_SERIALPORT_LIB;QT_CORE_LIB</Define>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <InputFile>%(FullPath)</InputFile>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
        <TableRow>
          <TableCell ColumnSpan="2" Background="WhiteSmoke" BorderBrush="Gray" BorderThickness="0.5" />
        </TableRow>
        <TableRow>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0">
              <Span Background="LightCoral"><![CDATA[      <IncludePath>GeneratedFiles\$(ConfigurationName);GeneratedFiles;$(QTDIR)/mkspecs/win32-msvc;.;$(QTDIR)/include;$(QTDIR)/include/QtWidgets;$(QTDIR)/include/QtGui;$(QTDIR)/include/QtANGLE;$(QTDIR)/include/QtNetwork;$(QTDIR)/include/QtConcurrent;$(QTDIR)/include/QtSerialPort;$(QTDIR)/include/QtCore;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include;C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include;C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt;C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt;C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um</IncludePath>]]></Span>
            </Paragraph>
          </TableCell>
          <TableCell BorderThickness="0, 0, 0.5, 0" BorderBrush="Gray">
            <Paragraph FontFamily="Consolas" Margin="4, 0" />
          </TableCell>
        </TableRow>
      </TableRowGroup>
    </Table>
  </Section>
  <Section>
    <Paragraph />
  </Section>
</FlowDocument>
<!--nG+C7y44g0H6Ube7bVjOtAdfu5fKAr63ZpCz6MMZvf0=-->


Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
VisualStudioVersion = 17.10.35004.147
MinimumVisualStudioVersion = 10.0.40219.1
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "CLumoMap", "CLumoMap.vcxproj", "{B74D7E5F-FAB7-31A0-A973-F9D6D3F20001}"
EndProject
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|x64 = Debug|x64
		Release|x64 = Release|x64
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		{B74D7E5F-FAB7-31A0-A973-F9D6D3F20001}.Debug|x64.ActiveCfg = Debug|x64
		{B74D7E5F-FAB7-31A0-A973-F9D6D3F20001}.Debug|x64.Build.0 = Debug|x64
		{B74D7E5F-FAB7-31A0-A973-F9D6D3F20001}.Release|x64.ActiveCfg = Release|x64
		{B74D7E5F-FAB7-31A0-A973-F9D6D3F20001}.Release|x64.Build.0 = Release|x64
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
	GlobalSection(ExtensibilityGlobals) = postSolution
		SolutionGuid = {71CFAC13-BE2E-481C-A63D-65C424504650}
	EndGlobalSection
EndGlobal

<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{B74D7E5F-FAB7-31A0-A973-F9D6D3F20001}</ProjectGuid>
    <RootNamespace>CLumoMap</RootNamespace>
    <Keyword>QtVS_v304</Keyword>
    <WindowsTargetPlatformVersion>10.0.26100.0</WindowsTargetPlatformVersion>
    <WindowsTargetPlatformMinVersion>10.0.26100.0</WindowsTargetPlatformMinVersion>
    <QtMsBuild Condition="'$(QtMsBuild)'=='' OR !Exists('$(QtMsBuild)\qt.targets')">$(MSBuildProjectDirectory)\QtMsBuild</QtMsBuild>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration">
    <PlatformToolset>v142</PlatformToolset>
    <OutputDirectory>release\</OutputDirectory>
    <ATLMinimizesCRunTimeLibraryUsage>false</ATLMinimizesCRunTimeLibraryUsage>
    <CharacterSet>NotSet</CharacterSet>
    <ConfigurationType>Application</ConfigurationType>
    <IntermediateDirectory>release\</IntermediateDirectory>
    <PrimaryOutput>CLumoMap</PrimaryOutput>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <PlatformToolset>v142</PlatformToolset>
    <OutputDirectory>debug\</OutputDirectory>
    <ATLMinimizesCRunTimeLibraryUsage>false</ATLMinimizesCRunTimeLibraryUsage>
    <CharacterSet>NotSet</CharacterSet>
    <ConfigurationType>Application</ConfigurationType>
    <IntermediateDirectory>debug\</IntermediateDirectory>
    <PrimaryOutput>CLumoMap</PrimaryOutput>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <Import Project="$(QtMsBuild)\qt_defaults.props" Condition="Exists('$(QtMsBuild)\qt_defaults.props')" />
  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <QtInstall>5.15.0_msvc2019_64</QtInstall>
    <QtModules>core;network;gui;widgets;serialport;concurrent</QtModules>
  </PropertyGroup>
  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <QtInstall>5.15.0_msvc2019_64</QtInstall>
    <QtModules>core;network;gui;widgets;serialport;concurrent</QtModules>
  </PropertyGroup>
  <Target Name="QtMsBuildNotFound" BeforeTargets="CustomBuild;ClCompile" Condition="!Exists('$(QtMsBuild)\qt.targets') OR !Exists('$(QtMsBuild)\Qt.props')">
    <Message Importance="High" Text="QtMsBuild: could not locate qt.targets, qt.props; project may not build correctly." />
  </Target>
  <ImportGroup Label="ExtensionSettings" />
  <ImportGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="PropertySheets">
    <Import Project="$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props')" />
    <Import Project="$(QtMsBuild)\Qt.props" />
  </ImportGroup>
  <ImportGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="PropertySheets">
    <Import Project="$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props')" />
    <Import Project="$(QtMsBuild)\Qt.props" />
  </ImportGroup>
  <PropertyGroup Label="UserMacros" />
  <PropertyGroup>
    <OutDir Condition="'$(Configuration)|$(Platform)'=='Release|x64'">release\</OutDir>
    <IntDir Condition="'$(Configuration)|$(Platform)'=='Release|x64'">release\</IntDir>
    <TargetName Condition="'$(Configuration)|$(Platform)'=='Release|x64'">CLumoMap</TargetName>
    <IgnoreImportLibrary Condition="'$(Configuration)|$(Platform)'=='Release|x64'">true</IgnoreImportLibrary>
    <LinkIncremental Condition="'$(Configuration)|$(Platform)'=='Release|x64'">false</LinkIncremental>
    <OutDir Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">debug\</OutDir>
    <IntDir Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">debug\</IntDir>
    <TargetName Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">CLumoMap</TargetName>
    <IgnoreImportLibrary Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">true</IgnoreImportLibrary>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;release;/include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <AdditionalOptions>-Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 %(AdditionalOptions)</AdditionalOptions>
      <AssemblerListingLocation>release\</AssemblerListingLocation>
      <BrowseInformation>false</BrowseInformation>
      <DebugInformationFormat>None</DebugInformationFormat>
      <DisableSpecificWarnings>4577;4467;%(DisableSpecificWarnings)</DisableSpecificWarnings>
      <ExceptionHandling>Sync</ExceptionHandling>
      <ObjectFileName>release\</ObjectFileName>
      <Optimization>MaxSpeed</Optimization>
      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <PreprocessToFile>false</PreprocessToFile>
      <ProgramDataBaseFileName>
      </ProgramDataBaseFileName>
      <RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>
      <SuppressStartupBanner>true</SuppressStartupBanner>
      <TreatWChar_tAsBuiltInType>true</TreatWChar_tAsBuiltInType>
      <WarningLevel>Level3</WarningLevel>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
    </ClCompile>
    <Link>
      <AdditionalDependencies>shell32.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <AdditionalLibraryDirectories>C:\openssl\lib;C:\Utils\my_sql\mysql-5.7.25-winx64\lib;C:\Utils\postgresql\pgsql\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalOptions>"/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'" %(AdditionalOptions)</AdditionalOptions>
      <DataExecutionPrevention>true</DataExecutionPrevention>
      <GenerateDebugInformation>false</GenerateDebugInformation>
      <IgnoreImportLibrary>true</IgnoreImportLibrary>
      <LinkIncremental>false</LinkIncremental>
      <OptimizeReferences>true</OptimizeReferences>
      <OutputFile>$(OutDir)\CLumoMap.exe</OutputFile>
      <RandomizedBaseAddress>true</RandomizedBaseAddress>
      <SubSystem>Windows</SubSystem>
      <SuppressStartupBanner>true</SuppressStartupBanner>
    </Link>
    <Midl>
      <DefaultCharType>Unsigned</DefaultCharType>
      <EnableErrorChecks>None</EnableErrorChecks>
      <WarningLevel>0</WarningLevel>
    </Midl>
    <ResourceCompile>
      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;NDEBUG;QT_NO_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ResourceCompile>
    <QtMoc>
      <CompilerFlavor>msvc</CompilerFlavor>
      <Include>./$(Configuration)/moc_predefs.h</Include>
      <ExecutionDescription>Moc'ing %(Identity)...</ExecutionDescription>
      <DynamicSource>output</DynamicSource>
      <QtMocDir>$(Configuration)</QtMocDir>
      <QtMocFileName>moc_%(Filename).cpp</QtMocFileName>
    </QtMoc>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile>
      <AdditionalIncludeDirectories>GeneratedFiles\$(ConfigurationName);GeneratedFiles;.;debug;/include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <AdditionalOptions>-Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 %(AdditionalOptions)</AdditionalOptions>
      <AssemblerListingLocation>debug\</AssemblerListingLocation>
      <BrowseInformation>false</BrowseInformation>
      <DebugInformationFormat>ProgramDatabase</DebugInformationFormat>
      <DisableSpecificWarnings>4577;4467;%(DisableSpecificWarnings)</DisableSpecificWarnings>
      <ExceptionHandling>Sync</ExceptionHandling>
      <ObjectFileName>debug\</ObjectFileName>
      <Optimization>Disabled</Optimization>
      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <PreprocessToFile>false</PreprocessToFile>
      <RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>
      <SuppressStartupBanner>true</SuppressStartupBanner>
      <TreatWChar_tAsBuiltInType>true</TreatWChar_tAsBuiltInType>
      <WarningLevel>Level3</WarningLevel>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
    </ClCompile>
    <Link>
      <AdditionalDependencies>shell32.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <AdditionalLibraryDirectories>C:\openssl\lib;C:\Utils\my_sql\mysql-5.7.25-winx64\lib;C:\Utils\postgresql\pgsql\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalOptions>"/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'" %(AdditionalOptions)</AdditionalOptions>
      <DataExecutionPrevention>true</DataExecutionPrevention>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <IgnoreImportLibrary>true</IgnoreImportLibrary>
      <OutputFile>$(OutDir)\CLumoMap.exe</OutputFile>
      <RandomizedBaseAddress>true</RandomizedBaseAddress>
      <SubSystem>Windows</SubSystem>
      <SuppressStartupBanner>true</SuppressStartupBanner>
    </Link>
    <Midl>
      <DefaultCharType>Unsigned</DefaultCharType>
      <EnableErrorChecks>None</EnableErrorChecks>
      <WarningLevel>0</WarningLevel>
    </Midl>
    <ResourceCompile>
      <PreprocessorDefinitions>_WINDOWS;UNICODE;_UNICODE;WIN32;_ENABLE_EXTENDED_ALIGNED_STORAGE;WIN64;_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ResourceCompile>
    <QtMoc>
      <CompilerFlavor>msvc</CompilerFlavor>
      <Include>./$(Configuration)/moc_predefs.h</Include>
      <ExecutionDescription>Moc'ing %(Identity)...</ExecutionDescription>
      <DynamicSource>output</DynamicSource>
      <QtMocDir>$(Configuration)</QtMocDir>
      <QtMocFileName>moc_%(Filename).cpp</QtMocFileName>
    </QtMoc>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="CComm.cpp" />
    <ClCompile Include="CLumoMap.cpp" />
    <ClCompile Include="CMainWin.cpp" />
    <ClCompile Include="main.cpp" />
  </ItemGroup>
  <ItemGroup>
    <QtMoc Include="CCloudPoints.h" />
    <QtMoc Include="CComm.h" />
    <QtMoc Include="CLumoMap.h" />
    <QtMoc Include="CMainWin.h" />
    <QtMoc Include="CCopyTableWidget.h" />
    <ClInclude Include="CProtocol.h" />
    <ClInclude Include="crc16.h" />
  </ItemGroup>
  <ItemGroup>
    <CustomBuild Include="debug\moc_predefs.h.cbt">
      <FileType>Document</FileType>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(QTDIR)\mkspecs\features\data\dummy.cpp;%(AdditionalInputs)</AdditionalInputs>
      <Command Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -Zi -MDd -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E $(QTDIR)\mkspecs\features\data\dummy.cpp 2&gt;NUL &gt;$(IntDir)\moc_predefs.h</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">Generate moc_predefs.h</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">$(IntDir)\moc_predefs.h;%(Outputs)</Outputs>
    </CustomBuild>
    <CustomBuild Include="release\moc_predefs.h.cbt">
      <FileType>Document</FileType>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR)\mkspecs\features\data\dummy.cpp;%(AdditionalInputs)</AdditionalInputs>
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">cl -Bx"$(QTDIR)\bin\qmake.exe" -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -O2 -MD -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E $(QTDIR)\mkspecs\features\data\dummy.cpp 2&gt;NUL &gt;$(IntDir)\moc_predefs.h</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Generate moc_predefs.h</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(IntDir)\moc_predefs.h;%(Outputs)</Outputs>
    </CustomBuild>
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <Import Project="$(QtMsBuild)\qt.targets" Condition="Exists('$(QtMsBuild)\qt.targets')" />
  <ImportGroup Label="ExtensionTargets" />
</Project>
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
    <Filter Include="Generated Files">
      <UniqueIdentifier>{71ED8ED8-ACB9-4CE9-BBE1-E00B30144E11}</UniqueIdentifier>
      <Extensions>cpp;c;cxx;moc;h;def;odl;idl;res;</Extensions>
    </Filter>
    <Filter Include="Generated Files">
      <UniqueIdentifier>{71ED8ED8-ACB9-4CE9-BBE1-E00B30144E11}</UniqueIdentifier>
      <Extensions>cpp;c;cxx;moc;h;def;odl;idl;res;</Extensions>
    </Filter>
    <Filter Include="Header Files">
      <UniqueIdentifier>{93995380-89BD-4b04-88EB-625FBE52EBFB}</UniqueIdentifier>
      <Extensions>h;hpp;hxx;hm;inl;inc;xsd</Extensions>
    </Filter>
    <Filter Include="Header Files">
      <UniqueIdentifier>{93995380-89BD-4b04-88EB-625FBE52EBFB}</UniqueIdentifier>
      <Extensions>h;hpp;hxx;hm;inl;inc;xsd</Extensions>
    </Filter>
    <Filter Include="Source Files">
      <UniqueIdentifier>{4FC737F1-C7A5-4376-A066-2A32D752A2FF}</UniqueIdentifier>
      <Extensions>cpp;c;cxx;def;odl;idl;hpj;bat;asm;asmx</Extensions>
    </Filter>
    <Filter Include="Source Files">
      <UniqueIdentifier>{4FC737F1-C7A5-4376-A066-2A32D752A2FF}</UniqueIdentifier>
      <Extensions>cpp;c;cxx;def;odl;idl;hpj;bat;asm;asmx</Extensions>
    </Filter>
  </ItemGroup>
  <ItemGroup>
    <ClCompile Include="CComm.cpp">
      <Filter>Source Files</Filter>
    </ClCompile>
    <ClCompile Include="CLumoMap.cpp">
      <Filter>Source Files</Filter>
    </ClCompile>
    <ClCompile Include="CMainWin.cpp">
      <Filter>Source Files</Filter>
    </ClCompile>
    <ClCompile Include="main.cpp">
      <Filter>Source Files</Filter>
    </ClCompile>
  </ItemGroup>
  <ItemGroup>
    <QtMoc Include="CCloudPoints.h">
      <Filter>Header Files</Filter>
    </QtMoc>
    <QtMoc Include="CComm.h">
      <Filter>Header Files</Filter>
    </QtMoc>
    <ClInclude Include="CProtocol.h">
      <Filter>Header Files</Filter>
    </ClInclude>
    <ClInclude Include="crc16.h">
      <Filter>Header Files</Filter>
    </ClInclude>
    <QtMoc Include="CLumoMap.h">
      <Filter>Header Files</Filter>
    </QtMoc>
    <QtMoc Include="CMainWin.h">
      <Filter>Header Files</Filter>
    </QtMoc>
    <QtMoc Include="CCopyTableWidget.h">
      <Filter>Header Files</Filter>
    </QtMoc>
  </ItemGroup>
  <ItemGroup>
    <CustomBuild Include="debug\moc_predefs.h.cbt">
      <Filter>Generated Files</Filter>
    </CustomBuild>
    <CustomBuild Include="release\moc_predefs.h.cbt">
      <Filter>Generated Files</Filter>
    </CustomBuild>
  </ItemGroup>
</Project>
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="Current" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup />
  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <QtTouchProperty>
    </QtTouchProperty>
  </PropertyGroup>
  <PropertyGroup Label="QtSettings" Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <QtTouchProperty>
    </QtTouchProperty>
  </PropertyGroup>
</Project>
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

#ifndef CMAINWIN_H
#define CMAINWIN_H

#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QTimer>
#include <QtCore/QMutex>
#include <QCombobox>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QHeaderView>
#include <QCheckBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QMessageBox>
#include <QLabel>

#include "CLumoMap.h"
#include "CCloudPoints.h"
#include "CComm.h"
#include "CProtocol.h"
#include "CCopyTableWidget.h"


class CMainWin : public QMainWindow, public ICloudPointGetter {
    Q_OBJECT
        Q_INTERFACES(ICloudPointGetter)

public:
    // [신규] 설정값 구조체 정의
    struct LidarConfig {
        QString name;
        int channels;
        int fov;
        float resolution;
        bool isClockwise;
        float angleOffset;
        float distanceRate;
        QString distanceUnit;
        int mesuresPerScan;
    };

    CMainWin(QWidget* parent = nullptr) : QMainWindow(parent) {

        loadConfig();
        m_settings = loadAppSettings();

        createPointViewerDock();
        setUI();

        int modelIndex = m_settings["lastModelIndex"].toInt(0);
        if (modelIndex < 0 || modelIndex >= m_lidarConfigArray.count()) modelIndex = 0;

        QJsonObject config = m_lidarConfigArray[modelIndex].toObject();
        int channels = config["channels"].toInt(1);
        int fov = config["fov"].toInt(360);
        float res = (float)config["resolution"].toDouble(0.33);

        cloudPoints = new CCloudPoints(this);
        lumoMap = new CLumoMap(this, channels, fov, res);
        
        applyAppSettings(m_settings);

        setCentralWidget(lumoMap);
        this->showMaximized();

        setLidarCfg(lidarCfgs->currentData().toInt());

        for (int i = 0; i < 4; ++i) {
            lumoMap->setVisibleLayer(i, visibleLayer[i]->isChecked());
        }
    }
    ~CMainWin() {
        runRepeat = false;
        if (comm)
            comm->close();
    }

public:
    // ICloudPointGetter 구현
    void clearPoints() override {
        m_pointTable->clearContents();
        m_pointTable->setRowCount(0);
        m_pointTableCaptureRow = 0;
    }

    void setPoint(quint16 angle, quint16 distance, int layerNo) override {
        m_pointTable->insertRow(m_pointTableCaptureRow);

        // [수정] Rate 적용하여 단위 값으로 변환 (예: Raw 100 * 0.1 = 10.0 cm)
        float finalDist = distance * m_curConfig.distanceRate;
        // 정수면 소수점 없이, 실수면 소수점 1자리 표시
        int decimals = (m_curConfig.distanceRate == (int)m_curConfig.distanceRate) ? 0 : 1;

        m_pointTable->setItem(m_pointTableCaptureRow, 0, new QTableWidgetItem(QString::number(angle / 100.0, 'f', 2)));
        m_pointTable->setItem(m_pointTableCaptureRow, 1, new QTableWidgetItem(QString::number(finalDist, 'f', decimals)));
        m_pointTable->setItem(m_pointTableCaptureRow, 2, new QTableWidgetItem(QString::number(layerNo + 1)));
        m_pointTableCaptureRow++;
    }

public slots:
    void onStatus(Comm* sender, Comm::eStatus status) {
        switch (status) {
        case Comm::eStatus::connected:
            btnRunRepeat->setEnabled(true);
            btnRunSingle->setEnabled(true);
            connStatus->setText("Connected");
            if (!btnConnect->isChecked())
                btnConnect->setChecked(true);
            chkCommType->setEnabled(false);
            connStringAction->setEnabled(false);
            connNumAction->setEnabled(false);
            lidarCfgsAction->setEnabled(false);
        case Comm::eStatus::ready:
            connStatus->setStyleSheet("color: white; background-color: darkBlue");
            break;
        case Comm::eStatus::sending:
            connStatus->setStyleSheet("color: black; background-color: lightGreen");
            break;
        case Comm::eStatus::recving:
            connStatus->setStyleSheet("color: white; background-color: Green");
            break;
        case Comm::eStatus::sent:
            onAlert(nullptr, 0, QString::asprintf("sent: %d", comm->bytesSent()));
            buff.clear();
            break;
        case Comm::eStatus::recved:
            onAlert(nullptr, 0, QString::asprintf("received: %d", comm->bytesRecv()));
            break;
        case Comm::eStatus::connFailed:
            onAlert(nullptr, (int)Comm::eStatus::connFailed, "Connection Failed");
            break;
        case Comm::eStatus::connLost:
            onAlert(nullptr, (int)Comm::eStatus::connLost, "Connection Error");
            break;
        case Comm::eStatus::closed:
            btnRunRepeat->setEnabled(false);
            btnRunSingle->setEnabled(false);
            connStatus->setStyleSheet("color: white; background-color: darkRed");
            connStatus->setText("Disconnected");
            if (btnConnect->isChecked())
                btnConnect->setChecked(false);
            chkCommType->setEnabled(true);
            comPortsAction->setEnabled(true);
            connStringAction->setEnabled(true);
            connNumAction->setEnabled(true);
            lidarCfgsAction->setEnabled(true);
            this->setWindowTitle("LumoMap");
            break;
        case Comm::eStatus::connecting:
            onAlert(nullptr, 0, "Connecting...");
            chkCommType->setEnabled(false);
            comPortsAction->setEnabled(false);
            lidarCfgsAction->setEnabled(false);
            break;
        case Comm::eStatus::closing:
            onAlert(nullptr, 0, "Close");
            break;
        case Comm::eStatus::disconnFailed:
            onAlert(nullptr, (int)Comm::eStatus::disconnFailed, "Disconnting Failed.");
            break;
        case Comm::eStatus::sendFailed:
            onAlert(nullptr, (int)Comm::eStatus::sendFailed, "Sending Failed.");
            break;
        case Comm::eStatus::recvFailed:
            onAlert(nullptr, (int)Comm::eStatus::recvFailed, "Receiving Failed.");
            break;
        }

        if (sender && sender->isOnError()) {
            m_statusBar->setStyleSheet("color: red");
            if (comm)
                comm->checkConn();
        }
    }
    void onProgress(Comm* sender, Comm::eProgress progress, quint32 bytes) {
        Q_UNUSED(sender);
        if (comm->isBusy()) {
            m_statusBar->setStyleSheet("");
            QString progMsg((progress == Comm::eProgress::sending ? "Sending: " : "Receiving: ") + QString::number(bytes));
            m_statusBar->showMessage(progMsg, waitForMsgDone);
        }
    }

    void onAlert(Comm* sender, int alertCode, const QString msg) {
        Q_UNUSED(sender);
        if (sender) m_statusBar->clearMessage();
        if (alertCode) {
            m_statusBar->setStyleSheet("color: red;");
            m_statusBar->showMessage("Error: " + msg, waitForMsgDone);
        }
        else {
            m_statusBar->setStyleSheet("");
            m_statusBar->showMessage(msg, waitForMsgDone);
        }
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if (runRepeat) {
            if (QMessageBox::question(this, "LumoMap", "Are you sure you want to close the LumoMap?")) {
                runRepeat = false;
                saveAppSettings(m_settings);
                event->accept();
            }
            else {
                event->ignore();
            }
        }
        else {
            saveAppSettings(m_settings);
            event->accept();
        }
    }

protected slots:
    void updatePoints() {
    DoAgain:
        isRunning = true;
        stopwatch.start();
        if (!comm)
            return;

        if (comm->inbox()) {
            qDebug() << "What!!!!!!!!!!!!!!!!!!!!";
            comm->recv(buff, 0);
            buff.clear();
            goto DoAgain;
        }

        onAlert(nullptr, 0, "");
        bool isOK = false;

        isOK = runProtocol(Protocol::eCmd::getBulk, true, true, 4, 0, reqWrdSize, &buff);

        if (!isOK) {
            lumoMap->fadeAway(m_fadeEnabled);
        }
        else {
            processPayload(buff, cloudPoints);
            lumoMap->lumos(cloudPoints->getPoints());
        }

        comm->doEvents();
        if (!runRepeat) {
            comm->doEvents();
            goto GoOut;
        }
        else {
            int nextCoolTime = interval->text().toInt() - stopwatch.elapsed();
            if (nextCoolTime > 0)
                comm->doEvents(nextCoolTime);
            goto DoAgain;
        }
    GoOut:
        isRunning = false;
        return;
    }

    void onLayerToggled(int) {
        if (m_curConfig.channels > 1) {
            for (int i = 0; i < m_curConfig.channels; ++i) {
                lumoMap->setVisibleLayer(i, visibleLayer[i]->isChecked());
            }
        }
    }

    void onClearPoints() {
        lumoMap->onClearPoints();
    }

    void onFadeToggle(bool checked) {
        m_fadeEnabled = checked;
        lumoMap->setFadeEnabled(checked);
    }

    void onCapturePoints() {
        m_pointTable->clearContents();
        m_pointTable->setRowCount(0);

        if (buff.isEmpty()) return;

        QByteArray payload;
        if (!ptc.unpack(buff, nullptr, nullptr, nullptr, nullptr, &payload, nullptr))
        {
            payload = buff;
        }

        processPayload(payload, this);
    }

    void onPointHighlight(int row, int column) {
        Q_UNUSED(column);
        if (row < 0 || row >= m_pointTable->rowCount()) return;

        float angle = m_pointTable->item(row, 0)->text().toFloat();
        float dist = m_pointTable->item(row, 1)->text().toFloat(); // (이미 Rate가 적용된 값)

        float fAngle = angle;

        // m_curConfig에서 직접 값을 가져옴
        if (m_curConfig.isClockwise) fAngle = -fAngle;
        fAngle += m_curConfig.angleOffset;

        float radian = fAngle * M_PI / 180.0;

        // [수정] 거리(Unit) -> 픽셀 변환 스케일 가져오기 (Raw -> Pixel 아님)
        float scale = cloudPoints->getUnitToPixelScale();

        float fDistance = dist * scale; // dist는 이미 Unit 단위이므로 UnitToPixelScale 사용
        float x = fDistance * std::cos(radian);
        float y = -fDistance * std::sin(radian); // Y축 반전

        lumoMap->setHighlight(QPointF(x, y), Qt::red);
    }

    void onPointSelectionChanged() {
        int rowCount = m_pointTable->selectionModel()->selectedRows().count();
        m_selectionCountLabel->setText(QString("%1 point(s) selected").arg(rowCount));
    }

    void onCurrentPointChanged(QTableWidgetItem* current, QTableWidgetItem* previous) {
        Q_UNUSED(previous);
        if (!current) return;
        onPointHighlight(current->row(), 0);
    }
private:
    int m_pointTableCaptureRow;
    QByteArray buff;
    QMutex runMtx;
    CCloudPoints* cloudPoints;
    CLumoMap* lumoMap;
    QLabel* statusIndicator;
    QElapsedTimer stopwatch;
    QLabel* connStatus;
    QString ipAddress;
    int port;
    enum class eCommType { None, TCP, UDP, COM };
    eCommType m_commType = eCommType::TCP;
    QLineEdit* connString;
    QLineEdit* connNum;
    QComboBox* comPorts;
    QComboBox* baudRates;
    QAction* connStringAction;
    QAction* connNumAction;
    QAction* comPortsAction;
    QAction* baudRatesAction;
    QPushButton* btnConnect;
    QLineEdit* interval;
    QPushButton* btnRunRepeat;
    QPushButton* btnRunSingle;
    QComboBox* lidarCfgs; // (이름 변경됨)
    QAction* lidarCfgsAction; // (이름 변경됨)
#define MAX_LAYERCNT    4
    QCheckBox* visibleLayer[MAX_LAYERCNT];
    QAction* visibleLayerAction[MAX_LAYERCNT];
    QAction* chkTCP;
    QAction* chkUDP;
    QAction* chkSerial;
    QActionGroup* chkCommType;
    QStatusBar* m_statusBar;
    Comm* comm = nullptr;
    bool isCOM = false;
    bool isDeviceOK = false;
    bool runRepeat = false;
    bool isRunning = false;
    int reqWrdSize = 2400;
    bool virtualDataEnabled = false;
    Protocol ptc;
    const quint32 waitForComm = 1000;
    const quint32 waitForConn = 1000;
    const quint32 waitForMsgDone = 3000;

    // [신규] 설정값 구조체 인스턴스
    LidarConfig m_curConfig;
    QJsonObject m_settings;
    QDockWidget* m_pointViewerDock;
    CCopyTableWidget* m_pointTable;
    QLabel* m_selectionCountLabel;
    QPushButton* m_btnClear;
    QPushButton* m_btnCapture;
    QCheckBox* m_fadeCheck;
    QAction* m_viewPointsAction;
    bool m_fadeEnabled;

    QJsonArray m_lidarConfigArray;

    bool runProtocolVirtual(QByteArray* recvData) {
        if (!recvData) return false;
        // m_curConfig 사용, 페이로드 생성 요청
        QByteArray payload = cloudPoints->generateVirtualPayload(
            m_curConfig.channels, m_curConfig.resolution, m_curConfig.mesuresPerScan);

        if (payload.isEmpty()) return false;

        // Pack (setBulk)
        *recvData = ptc.pack(Protocol::eCmd::setBulk, 0, 0, reqWrdSize, &payload, nullptr);
        return !recvData->isEmpty();
    }

    bool runProtocol(Protocol::eCmd cmd, bool needRecv = false, bool needUnpack = false,
        quint16 dataType = 0, quint16 startAddr = 0, quint16 reqWordCnt = 0,
        QByteArray* recvData = nullptr,
        Protocol::eCmd* recvCmd = nullptr, quint16* recvDataType = nullptr)
    {
        bool isOK = false;
        QByteArray sendBuff;

        if (virtualDataEnabled) {
            isOK = runProtocolVirtual(recvData);
            if (!isOK) return false;
        }
        else {
            if (!comm) return false;
            if (!comm->isIdle()) return false;

            sendBuff = ptc.pack(cmd, dataType, startAddr, reqWordCnt);
            isOK = comm->send(sendBuff, 1000);
            if (!isOK) return false;

            sendBuff.clear();
            comm->waitForReady();
            if (!needRecv) return true;

            for (int i = 0; i < 50; i++) {
                comm->doEvents();
                isOK = comm->inbox(30);
                if (isOK) break;
                comm->doEvents();
            }

            int recvTimeout = isOK ? 300 : 100;
            int expectedBytes = (reqWrdSize * 2) + 11;

            isOK = comm->recv(*recvData, recvTimeout, expectedBytes);
            if (!isOK) return false;
        }

        if (needUnpack) {
            QByteArray payload;
            isOK = ptc.unpack(*recvData, recvCmd, recvDataType, nullptr, nullptr, &payload, nullptr);
            if (isOK) {
                *recvData = payload;
            }
        }
        return isOK;
    }

    typedef union {
        quint16 word;
        struct {
            char low;
            char hi;
        } bytes;
    } wordBytes;

    void processPayload(QByteArray& payload, ICloudPointGetter* processor)
    {
        if (!processor) return;

        processor->clearPoints();

        // 패킷 크기: Angle(2) + (Channels * Dist(2))
        int numChannels = m_curConfig.channels;
        int packetSize = 2 + (numChannels * 2);

        int cnt = payload.count() / packetSize;
        if (cnt <= 0) return;

        quint16 wdAngle, wdDist;

        const quint8* dataPtr = (const quint8*)payload.constData();

        for (int i = 0; i < cnt; i++) {
            int offset = i * packetSize;

            // Big Endian (MSB first)
            quint8 angleHi = dataPtr[offset];
            quint8 angleLo = dataPtr[offset + 1];
            wdAngle = (quint16)((angleHi << 8) | angleLo);

            for (int j = 0; j < numChannels; j++) {
                int distOffset = offset + 2 + (j * 2);

                // Distance (2 bytes) - Big Endian
                quint8 distHi = dataPtr[distOffset];
                quint8 distLo = dataPtr[distOffset + 1];
                wdDist = (quint16)((distHi << 8) | distLo);

                processor->setPoint(wdAngle, wdDist, j);
            }
        }
    }

    void clickCommType() {
        isCOM = (chkSerial->isChecked());
        connStringAction->setVisible(!isCOM);
        connNumAction->setVisible(!isCOM);
        if (isCOM) {
            comPorts->clear();
            // for order by name.
            QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
            std::sort(ports.begin(), ports.end(), [](const QSerialPortInfo& a, const QSerialPortInfo& b) {
                return a.portName() < b.portName();
                });
            foreach(const QSerialPortInfo& port, ports) {
                comPorts->addItem(port.portName());
            }
        }
        comPortsAction->setVisible(isCOM);
        baudRatesAction->setVisible(isCOM);
    }

    bool setCommType() {
        if (comm) {
            return false;
            while (!comm->close(waitForConn));
            delete comm;
            comm = nullptr;
        }

        if (chkTCP->isChecked()) {
            m_commType = eCommType::TCP;
            comm = new CommTCP(this);
        }
        else if (chkUDP->isChecked()) {
            m_commType = eCommType::UDP;
            comm = new CommUDP(this);
        }
        else if (chkSerial->isChecked()) {
            m_commType = eCommType::COM;
            comm = new CommSerial(this);
        }
        QObject::connect(comm, &Comm::onStatus, this, &CMainWin::onStatus);
        QObject::connect(comm, &Comm::onAlert, this, &CMainWin::onAlert);
        QObject::connect(comm, &Comm::onProgress, this, &CMainWin::onProgress);
        return true;
    }

    void toggleConn() {
        btnConnect->setEnabled(false);
        if (btnConnect->isChecked()) {
            if (comm) {
                comm->close();
                comm->deleteLater();
                comm = nullptr;
            }
            setCommType();
            virtualDataEnabled = (m_commType == eCommType::UDP) && connString->text().right(2) == ".0";
            if (!comm->checkConn()) {
                if (isCOM)
                    comm->setConnInfo(comPorts->currentText(), this->baudRates->currentText().toInt());
                else {
                    comm->setConnInfo(connString->text(), connNum->text().toInt());
                }
                comm->connect(waitForConn);
            }
        }
        else {
            if (runRepeat) {
                runRepeat = false;
                QTimer::singleShot(0, this, [&]() {
                    toggleConn();
                    });
                return;
            }
            btnRunRepeat->setChecked(false);
            if (comm) {
                if (!comm->isOnError()) {
                    comm->close(waitForConn);
                }
                if (cloudPoints) {
                    onClearPoints();
                }
            }
        }
        btnConnect->setEnabled(true);
    }


    void toggleRun() {
        if (!comm) {
            if (btnRunRepeat->isChecked())
                btnRunRepeat->setChecked(false);
            return;
        }

        if (btnRunRepeat->isChecked()) {
            runRepeat = true;
            btnRunSingle->setEnabled(false);
            QTimer::singleShot(0, this, [&]() {
                updatePoints();
                });
        }
        else {
            runRepeat = false;
            btnRunSingle->setEnabled(true);
        }
    }

    void singleShot() {
        if (!comm) {
            return;
        }

        if (btnRunSingle->isChecked()) {
            qDebug() << "Single Shot!!";
            runRepeat = false;
            updatePoints();
            btnRunSingle->setChecked(false);
        }
    }

    // --- JSON Config 로직 ---
    void loadConfig() {
        QFile file("config.json");
        if (!file.open(QIODevice::ReadOnly)) {
            qDebug() << "config.json not found. Creating default config.";
            createDefaultConfig();
            return;
        }

        QByteArray data = file.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull() || !doc.isObject() || !doc.object().contains("Types") || !doc.object()["Types"].isArray()) {
            qDebug() << "config.json is invalid. Creating default config.";
            createDefaultConfig();
        }
        else {
            m_lidarConfigArray = doc.object()["Types"].toArray();
            qDebug() << "Config loaded.";
        }
        file.close();
    }

    void createDefaultConfig() {
        QJsonObject type1, type2;
        QJsonArray typesArray;

        type1["name"] = "1ch / 360° (Standard)";
        type1["channels"] = 1;
        type1["fov"] = 360;
        type1["resolution"] = 0.33;
        type1["isClockwise"] = true;
        type1["angleOffset"] = 180.0;
        type1["fovStart"] = 0.0;
        type1["distanceRate"] = 0.1; // Raw 1 = 0.1cm (1mm)
        type1["distanceUnit"] = "cm";
        type1["mesuresPerScan"] = (int)(360 / 0.33) + 1;

        type2["name"] = "4ch / 90° (Front-Facing)";
        type2["channels"] = 4;
        type2["fov"] = 90;
        type2["resolution"] = 0.33;
        type2["isClockwise"] = true;
        type2["angleOffset"] = 135.0;
        type2["fovStart"] = 45.0;
        type2["distanceRate"] = 0.1;
        type2["distanceUnit"] = "cm";
        type2["mesuresPerScan"] = (int)(90 / 0.33) + 1;

        typesArray.append(type1);
        typesArray.append(type2);

        QJsonObject rootObj;
        rootObj["Types"] = typesArray;
        m_lidarConfigArray = typesArray;

        QFile file("config.json");
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning("Couldn't write config.json");
            return;
        }
        file.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
        file.close();
    }

    void saveAppSettings(QJsonObject& settings) {
        settings["ip"] = connString->text();
        settings["port"] = connNum->text();
        settings["interval"] = interval->text();
        settings["fadeEffect"] = m_fadeCheck->isChecked();
        settings["lastModelIndex"] = lidarCfgs->currentIndex();

        // 통신 타입 저장
        QString commType = "TCP";
        if (chkUDP->isChecked()) commType = "UDP";
        else if (chkSerial->isChecked()) commType = "COM";
        settings["commType"] = commType;

        // 화면 상태 저장 (Zoom, Offset)
        settings["zoomRate"] = (double)lumoMap->getZoomRate();
        settings["offsetX"] = (double)lumoMap->getCenterOffset().x();
        settings["offsetY"] = (double)lumoMap->getCenterOffset().y();

        QFile file("init.json");
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning("Couldn't write init.json");
            return;
        }
        file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented));
        file.close();
    }

    QJsonObject loadAppSettings() {
        QFile file("init.json");
        if (!file.open(QIODevice::ReadOnly)) {
            qDebug() << "init.json not found. Using default UI settings.";
            return QJsonObject();
        }

        QByteArray data = file.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull() || !doc.isObject()) {
            qDebug() << "init.json is invalid.";
            return QJsonObject();
        }

        return doc.object();
    }

    void applyAppSettings(QJsonObject& settings) {
        if (settings.isEmpty()) return;

        connString->setText(settings["ip"].toString(connString->text()));
        connNum->setText(settings["port"].toString(connNum->text()));
        interval->setText(settings["interval"].toString(interval->text()));
        m_fadeCheck->setChecked(settings["fadeEffect"].toBool(m_fadeCheck->isChecked()));
        lidarCfgs->setCurrentIndex(settings["lastModelIndex"].toInt(lidarCfgs->currentIndex()));

        // 통신 타입 복원
        QString commType = settings["commType"].toString("TCP");
        if (commType == "UDP") chkUDP->setChecked(true);
        else if (commType == "COM") chkSerial->setChecked(true);
        else chkTCP->setChecked(true);
        clickCommType();

        // 화면 상태 복원
        if (!lumoMap) return;
        if (settings.contains("zoomRate")) {
            lumoMap->setZoomRate((float)settings["zoomRate"].toDouble(1.0));
        }
        if (settings.contains("offsetX") && settings.contains("offsetY")) {
            lumoMap->setCenterOffset(QPointF(
                settings["offsetX"].toDouble(0.0),
                settings["offsetY"].toDouble(0.0)
            ));
        }
    }
    // --- ---

    void setLidarCfg(int index) {

        if (index < 0 || index >= m_lidarConfigArray.count()) {
            qWarning() << "Invalid LiDAR type index:" << index;
            if (m_lidarConfigArray.isEmpty()) createDefaultConfig();
            index = 0;
        }

        QJsonObject config = m_lidarConfigArray[index].toObject();
        m_curConfig.name = config["name"].toString();
        m_curConfig.channels = config["channels"].toInt(1);
        m_curConfig.fov = config["fov"].toInt(360);
        m_curConfig.resolution = (float)config["resolution"].toDouble(0.33);
        m_curConfig.isClockwise = config["isClockwise"].toBool(true);
        m_curConfig.angleOffset = (float)config["angleOffset"].toDouble(0.0);
        m_curConfig.distanceRate = (float)config["distanceRate"].toDouble(0.1);
        m_curConfig.distanceUnit = config["distanceUnit"].toString("cm");
        m_curConfig.mesuresPerScan = config["mesuresPerScan"].toInt((int)(m_curConfig.fov / m_curConfig.resolution) + 1);

        // Unit -> Meter 변환
        float unitToMeter = 0.01f; // 기본 cm
        if (m_curConfig.distanceUnit == "mm") unitToMeter = 0.001f;
        else if (m_curConfig.distanceUnit == "m") unitToMeter = 1.0f;
        else if (m_curConfig.distanceUnit == "cm") unitToMeter = 0.01f;

        // reqWrdSize(스캔 당 측정 횟수) 계산
        if (m_curConfig.mesuresPerScan <= 0) m_curConfig.mesuresPerScan = 1;
        int bytesPerPacket = 2 + (m_curConfig.channels * 2);
        int totalDataBytes = m_curConfig.mesuresPerScan * bytesPerPacket;
        reqWrdSize = (totalDataBytes + 1) / 2;

        // 설정 전파
        cloudPoints->setOrientation(m_curConfig.angleOffset, m_curConfig.isClockwise);
        cloudPoints->setDistanceSettings(m_curConfig.distanceRate, unitToMeter);

        // lumoMap 설정
        lumoMap->setHardwareProfile(m_curConfig.channels, m_curConfig.fov, m_curConfig.resolution);
        lumoMap->setMapOrientation(m_curConfig.angleOffset, m_curConfig.isClockwise);
        lumoMap->setDistanceUnit(m_curConfig.distanceUnit);

        // 테이블 헤더 업데이트
        m_pointTable->setHorizontalHeaderItem(1, new QTableWidgetItem("Dist (" + m_curConfig.distanceUnit + ")"));

        // UI 가시성
        bool showCheckboxes = (m_curConfig.channels > 1);
        for (int i = 0; i < MAX_LAYERCNT; ++i) {
            visibleLayerAction[i]->setVisible(showCheckboxes);
        }

        if (!showCheckboxes) {
            lumoMap->setVisibleLayer(0, true);
            for (int i = 1; i < MAX_LAYERCNT; ++i) {
                lumoMap->setVisibleLayer(i, false);
            }
        }
        else {
            for (int i = 0; i < m_curConfig.channels; ++i) {
                lumoMap->setVisibleLayer(i, visibleLayer[i]->isChecked());
            }
        }
    }

    void createPointViewerDock() {
        m_pointViewerDock = new QDockWidget("Point Viewer", this);
        m_pointViewerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

        QWidget* dockContainer = new QWidget();
        QVBoxLayout* layout = new QVBoxLayout(dockContainer);

        m_btnCapture = new QPushButton("Capture Current Scan");
        layout->addWidget(m_btnCapture);

        m_pointTable = new CCopyTableWidget();
        m_pointTable->setColumnCount(3);
        m_pointTable->setHorizontalHeaderLabels(QStringList() << "Angle (°)" << "Dist (cm)" << "Layer");
        m_pointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

        m_pointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_pointTable->setSelectionMode(QAbstractItemView::ExtendedSelection);

        m_pointTable->verticalHeader()->setVisible(false);
        m_pointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        layout->addWidget(m_pointTable);

        m_selectionCountLabel = new QLabel("0 point(s) selected", this);
        layout->addWidget(m_selectionCountLabel);

        m_pointViewerDock->setWidget(dockContainer);
        addDockWidget(Qt::RightDockWidgetArea, m_pointViewerDock);

        connect(m_btnCapture, &QPushButton::clicked, this, &CMainWin::onCapturePoints);
        //connect(m_pointTable, &CCopyTableWidget::cellClicked, this, &CMainWin::onPointHighlight);
        connect(m_pointTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &CMainWin::onPointSelectionChanged);
        connect(m_pointTable, &CCopyTableWidget::currentItemChanged,
            this, &CMainWin::onCurrentPointChanged);
    }

    void setUI() {
        this->resize(1280, 720);
        this->setWindowTitle("LumoMap");

        QToolBar* toolBar = addToolBar("Comm");

        // (통신 설정)
        btnConnect = new QPushButton("Connect", this);
        btnConnect->setCheckable(true);
        toolBar->addWidget(btnConnect);
        connect(btnConnect, &QPushButton::toggled, this, &CMainWin::toggleConn);

        chkCommType = new QActionGroup(this);
        chkTCP = new QAction("TCP", chkCommType);
        chkTCP->setCheckable(true); chkTCP->setChecked(true);
        chkUDP = new QAction("UDP", chkCommType);
        chkUDP->setCheckable(true);
        chkSerial = new QAction("COM", chkCommType);
        chkSerial->setCheckable(true);
        toolBar->addActions(chkCommType->actions());
        connect(chkTCP, &QAction::triggered, this, &CMainWin::clickCommType);
        connect(chkUDP, &QAction::triggered, this, &CMainWin::clickCommType);
        connect(chkSerial, &QAction::triggered, this, &CMainWin::clickCommType);
        connString = new QLineEdit("127.0.0.1", this);
        connString->setFixedWidth(100); connString->setAlignment(Qt::AlignCenter);
        connStringAction = toolBar->addWidget(connString);
        connNum = new QLineEdit("47777", this);
        connNum->setFixedWidth(100); connNum->setAlignment(Qt::AlignCenter);
        connNum->setValidator(new QIntValidator(0, 65535, this));
        connNumAction = toolBar->addWidget(connNum);
        comPorts = new QComboBox(this);
        comPorts->setVisible(false); comPorts->setEditable(true);
        comPorts->setFixedWidth(100);
        comPortsAction = toolBar->addWidget(comPorts);
        baudRates = new QComboBox(this);
        QList<int> baudRateList = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
        for (int baudRate : baudRateList) baudRates->addItem(QString::number(baudRate));
        baudRates->setCurrentIndex(baudRateList.indexOf(115200));
        baudRates->setVisible(false); baudRates->setFixedWidth(100);
        baudRatesAction = toolBar->addWidget(baudRates);
        baudRatesAction->setEnabled(false);
        toolBar->addSeparator();
        // (통신 설정 끝)

        toolBar = addToolBar("Run");

        // (Run)
        btnRunRepeat = new QPushButton("Run", this);
        btnRunRepeat->setCheckable(true); btnRunRepeat->setEnabled(false);
        toolBar->addWidget(btnRunRepeat);
        connect(btnRunRepeat, &QPushButton::toggled, this, &CMainWin::toggleRun);

        //toolBar->addWidget(new QLabel(" Interval: ", this));
        interval = new QLineEdit("100", this);
        interval->setFixedWidth(50); interval->setAlignment(Qt::AlignCenter);
        toolBar->addWidget(interval);
        toolBar->addWidget(new QLabel("ms", this));

        toolBar->addSeparator();

        btnRunSingle = new QPushButton("Single shot", this);
        btnRunSingle->setCheckable(true); btnRunSingle->setEnabled(false);
        toolBar->addWidget(btnRunSingle);
        connect(btnRunSingle, &QPushButton::toggled, this, &CMainWin::singleShot);

        //toolBar->addSeparator();
        // (End of Run)

        toolBar = addToolBar("Device");

        // LiDAR 모델(타입) - JSON 배열에서 이름 로드
        toolBar->addWidget(new QLabel("Model: ", this));
        lidarCfgs = new QComboBox(this); // (이름 변경됨)
        for (int i = 0; i < m_lidarConfigArray.count(); ++i) {
            QJsonObject obj = m_lidarConfigArray[i].toObject();
            lidarCfgs->addItem(obj["name"].toString("Unknown"), i);
        }
        lidarCfgs->setCurrentIndex(1);
        lidarCfgs->setFixedWidth(150);
        lidarCfgsAction = toolBar->addWidget(lidarCfgs); // (이름 변경됨)
        connect(lidarCfgs, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { setLidarCfg(lidarCfgs->currentData().toInt()); });

        // Visible Layer
        for (int i = 0; i < 4; ++i) {
            visibleLayer[i] = new QCheckBox(QString::number(i + 1), this);
            visibleLayer[i]->setChecked(true);
            connect(visibleLayer[i], &QCheckBox::toggled, this, &CMainWin::onLayerToggled);
            visibleLayerAction[i] = toolBar->addWidget(visibleLayer[i]);
        }
        //toolBar->addSeparator();

        toolBar = addToolBar("Display");

        // Clear Map
        
        m_btnClear = new QPushButton("Clear Map", this);
        toolBar->addWidget(m_btnClear);
        connect(m_btnClear, &QPushButton::clicked, this, &CMainWin::onClearPoints);
        toolBar->addSeparator();

        // Fade Effect
        m_fadeCheck = new QCheckBox("Fade Effect", this);
        m_fadeCheck->setChecked(true);
        m_fadeEnabled = true;
        toolBar->addWidget(m_fadeCheck);
        connect(m_fadeCheck, &QCheckBox::toggled, this, &CMainWin::onFadeToggle);


        toolBar = addToolBar("Tool");


        m_viewPointsAction = toolBar->addAction("Point Viewer");
        connect(m_viewPointsAction, &QAction::triggered, this, [this]() {
            m_pointViewerDock->setVisible(true);
            m_pointViewerDock->raise();
            });



        // 상태표시줄
        m_statusBar = new QStatusBar(this);
        setStatusBar(m_statusBar);
        connStatus = new QLabel("Disconnected", this);
        connStatus->setFixedWidth(100);
        connStatus->setAlignment(Qt::AlignCenter);
        m_statusBar->addPermanentWidget(connStatus);

        if (comm)
            this->onStatus(comm, Comm::eStatus::closed);
    }
};


#include <QApplication>
#include <QFile>
#include <QStyleFactory>
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // (선택) 'Fusion' 스타일을 적용하면 QSS가 더 일관되게 보입니다.
    app.setStyle(QStyleFactory::create("Fusion"));
    //QFile file("qdarkstyle.qss");

    //if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    //    QString styleSheet = QLatin1String(file.readAll());
    //    app.setStyleSheet(styleSheet);
    //    file.close();
    //}

    QPalette darkPalette;

    // 공통 색상 정의
    QColor darkGray(53, 53, 53);
    QColor gray65(65, 65, 65);
    QColor gray80(80, 80, 80);
    QColor gray100(100, 100, 100);
    QColor gray(128, 128, 128);
    QColor lightGray(180, 180, 180);
    QColor black(25, 25, 25);
    QColor blue(42, 130, 218);

    // --- 활성(Active) 그룹 ---
    // (창이 활성화되었을 때의 색상)
    darkPalette.setColor(QPalette::Active, QPalette::Window, gray65);
    darkPalette.setColor(QPalette::Active, QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Active, QPalette::Base, black);
    darkPalette.setColor(QPalette::Active, QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::Active, QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::Active, QPalette::ToolTipText, Qt::black);
    darkPalette.setColor(QPalette::Active, QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Active, QPalette::Button, gray80);
    darkPalette.setColor(QPalette::Active, QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Active, QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Active, QPalette::Link, blue);
    darkPalette.setColor(QPalette::Active, QPalette::Highlight, blue);
    darkPalette.setColor(QPalette::Active, QPalette::HighlightedText, Qt::black);

    // --- 비활성(Inactive) 그룹 ---
    // (다른 창을 클릭해서 이 창이 포커스를 잃었을 때)
    darkPalette.setColor(QPalette::Inactive, QPalette::Window, gray65);
    darkPalette.setColor(QPalette::Inactive, QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Inactive, QPalette::Base, black);
    darkPalette.setColor(QPalette::Inactive, QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::Inactive, QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::Inactive, QPalette::ToolTipText, Qt::black);
    darkPalette.setColor(QPalette::Inactive, QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Inactive, QPalette::Button, gray80);
    darkPalette.setColor(QPalette::Inactive, QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Inactive, QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Inactive, QPalette::Link, blue);
    darkPalette.setColor(QPalette::Inactive, QPalette::Highlight, blue);
    darkPalette.setColor(QPalette::Inactive, QPalette::HighlightedText, Qt::black);

    // --- 비활성화(Disabled) 그룹 ---
    // (btnRun->setEnabled(false) 됐을 때의 색상)
    darkPalette.setColor(QPalette::Disabled, QPalette::Window, gray65);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Base, black);
    darkPalette.setColor(QPalette::Disabled, QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::Disabled, QPalette::ToolTipText, Qt::black);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);      // 비활성 텍스트
    darkPalette.setColor(QPalette::Disabled, QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray); // 비활성 버튼 텍스트
    darkPalette.setColor(QPalette::Disabled, QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Disabled, QPalette::Link, blue);
    darkPalette.setColor(QPalette::Disabled, QPalette::Highlight, black); // 비활성 하이라이트
    darkPalette.setColor(QPalette::Disabled, QPalette::HighlightedText, gray);

    app.setPalette(darkPalette);

    QColor highlightColor = darkPalette.color(QPalette::Highlight); // Blue
    QColor highlightedText = darkPalette.color(QPalette::HighlightedText); // Black
    QColor buttonColor = darkPalette.color(QPalette::Button); // DarkGray
    QColor buttonText = darkPalette.color(QPalette::ButtonText); // White
    QColor disabledText = darkPalette.color(QPalette::Disabled, QPalette::ButtonText); // Gray

    // 2. QSS를 사용하여 "눌린(toggled)" 및 "비활성(disabled)" 버튼 스타일 적용
    app.setStyleSheet(QString(

        // --- 활성 상태 (Normal) ---
        "QPushButton:checked, QPushButton:pressed {" // (1) 눌린 상태
        "    background-color: %1;"                   //     (Blue)
        "    color: %2;"                              //     (Black)
        "    border: 1px solid %1;"
        "}"
        "QPushButton:!checked {"                    // (2) 안 눌린 상태
        "    background-color: %3;"                   //     (DarkGray)
        "    color: %4;"                              //     (White)
        "}"

        // --- 비활성 상태 (Disabled) ---
        "QPushButton:disabled {"                    // (3) 안 눌린 + 비활성 상태
        "    background-color: %3;"                   //     (DarkGray)
        "    color: %5;"                              //     (Gray)
        //"    border: 1px solid %3;"
        "}"
        "QPushButton:checked:disabled {"            // (4) 눌린 + 비활성 상태
        "    background-color: %2;"                   //     (black)
        "    color: %5;"                              //     (Gray)
        "    border: 1px solid %1;"                   //     (테두리만 Blue로 남겨서 눌렸었음을 표시)
        "}"

    ).arg(highlightColor.name()) // %1
    .arg(highlightedText.name()) // %2
    .arg(buttonColor.name())     // %3
    .arg(buttonText.name())      // %4
    .arg(disabledText.name())    // %5
    );

    CMainWin mainWindow;
    mainWindow.show();
    return app.exec();
}

#endif // CMAINWIN_H
{
    "Types": [
        {
            "channels": 1,
            "distanceRate": 0.1,
            "distanceUnit": "cm",
            "fov": 360,
            "isClockwise": true,
            "mesuresPerScan": 1091,
            "name": "1ch / 360° (Standard)",
            "resolution": 0.33,
            "angleOffset": 180
        },
        {
            "channels": 4,
            "distanceRate": 0.1,
            "distanceUnit": "cm",
            "fov": 90,
            "isClockwise": true,
            "mesuresPerScan": 273,
            "name": "4ch / 90° (Front-Facing)",
            "resolution": 0.33,
            "angleOffset": 135
        }
    ]
}

{
    "Types": [
        {
            "channels": 1,
            "distanceRate": 0.1,
            "distanceUnit": "cm",
            "fovStart": 180,
            "frontAngle": 360,
            "isCounterClockwise": false,
            "name": "1ch / 360° (Standard)",
            "resolution": 0.33,
            "rotationOffset": 0,
	    "mesuresPerScan" = 1091
        },
        {
            "channels": 4,
            "distanceRate": 0.1,
            "distanceUnit": "cm",
            "fovStart": 45,
            "frontAngle": 90,
            "isCounterClockwise": false,
            "name": "4ch / 90° (Front-Facing)",
            "resolution": 0.33,
            "rotationOffset": 135,
	    "mesuresPerScan" = 1091
        }
    ]
}

#ifndef CPROTOCOL_H
#define CPROTOCOL_H

//#include <QtCore/QObject>
#include <QByteArray>
#include <QDataStream>
#include "crc16.h"

class Protocol {

public:
    Protocol() {}

    static enum class eCmd : unsigned int {
        typeHCS = 0xA1,
        getVersion,
        stop,
        start,
        sw1,
        sw2,
        sw3,
        sw4,
        typePA2 = 0xAA,
        getParam,
        setParam,
        getBulk,
        setBulk
    };

    static QByteArray pack(eCmd command, quint16 dType = 0,
                          quint16 sAddr = 0, quint16 wCnt = 0,
                          QByteArray* data = nullptr, QByteArray* result = nullptr) {
        QByteArray cmd;
        QDataStream in(&cmd, QIODevice::WriteOnly);

        if (command < eCmd::typePA2)
            in << quint8(0xA1);
        else {
            in << quint8(0xAA);
            in << quint8(0x00);
        }

        switch (command) {
        case eCmd::stop:
            cmd.append('Q');
            break;
        case eCmd::start:
            cmd.append('S');
            break;
        case eCmd::getVersion:
            cmd.append('V');
            break;
        case eCmd::sw1:
            cmd.append('A');
            break;
        case eCmd::sw2:
            cmd.append('B');
            break;
        case eCmd::sw3:
            cmd.append('C');
            break;
        case eCmd::sw4:
            cmd.append('D');
            break;
        case eCmd::getParam:
            in.writeRawData("MB", 2);
            in << quint16(7);
            in << dType;
            in << sAddr;
            in << wCnt;
            in << getCrc16(cmd);
            break;
        case eCmd::setParam:
            in.writeRawData("AE", 2);
            in << quint16(7 + (data->count() / 2));
            in << dType;
            in << sAddr;
            in << quint16(data->count());
            cmd.append(*data);
            in << getCrc16(cmd);
            break;
        case eCmd::getBulk:
            in.writeRawData("GA", 2);
            in << quint16(6);
            in << dType;
            in << sAddr;
            in << wCnt;
            break;
        case eCmd::setBulk:
            in.writeRawData("GB", 2);
            in << quint16(5 + (data->count() / 2) + (result)? 10: 0);
            /*in << dType;*/ // Eliminated for this command.
            in << sAddr;
            in << wCnt;
            cmd.append(*data);
            if (result)
                cmd.append(*result);
            break;
        default:
            break;
        }
        cmd.append('\r'); // Add 'CR' in the end.

        return cmd;
    }

    static bool unpack(const QByteArray& data, eCmd* command,
                       quint16* dataType, quint16* startAddr, quint16* reqWordCnt,
                       QByteArray* payload, QByteArray* resultData) {
        eCmd cmd;
        quint16 dSize = data.size(), dType = 0, sAddr = 0, wCnt = 0;
        // Check header to find out the command type.
        if (data[0] == char(0xA1)) {
            cmd = eCmd::typeHCS;
        } else if (data[0] == char(0xAA)) {
            cmd = eCmd::typePA2;
        } else {
            return false; // Invalid header
        }

        // Extract command based on header and data size
        if (cmd == eCmd::typeHCS) {
            if (data.size() < 3) {
                return false; // Packet too small for HCS commands
            }
            switch (data[1]) {
            case 'V':
                cmd = eCmd::getVersion;
                break;
            case 'Q':
                cmd = eCmd::stop;
                break;
            case 'S':
                cmd = eCmd::start;
                break;
            case 'A':
                cmd = eCmd::sw1;
                break;
            case 'B':
                cmd = eCmd::sw2;
                break;
            case 'C':
                cmd = eCmd::sw3;
                break;
            case 'D':
                cmd = eCmd::sw4;
                break;
            default:
                return false; // Invalid command for HCS
            }
        } else { // PA2 commands
            quint16 dataSize = data.size();
            if (data.size() < 12) {
                return false; // Packet too small for PA2 command data
            }
            quint16 readSize = (data[4] << 8) | data[5];
            if (data.size() < 100) {
                return false; // Packet too small for PA2 command data
            }
            dType = (data[6] << 8) | data[7];
            sAddr = (data[8] << 8) | data[9];
            wCnt = (data[10] << 8) | data[11];
            switch (data[2]) {
            case 'M': // GetParam
                cmd = eCmd::getParam;
                break;
            case 'A': // SetParam
                cmd = eCmd::setParam;
                if (payload) {
                    payload->clear();
                    payload->append(data.mid(12, wCnt));
                }
                break;
            case 'G': // GetBulk
                switch (data[3]) {
                case 'A':
                    cmd = eCmd::getBulk;
                    break;
                case 'B': {
                    cmd = eCmd::setBulk;
                    dType = 0;
                    sAddr = (data[6] << 8) | data[7];
                    wCnt = (data[8] << 8) | data[9];
                    bool isDataShort = dSize - 11 < wCnt;
                    if (isDataShort)
                        wCnt = int((dSize - 11) / 2);
                    if (payload) {
                        payload->clear();
                        payload->append(data.mid(10, (wCnt * 2)));
                    }
                    if (resultData && !isDataShort) {
                        dType = 4;
                        resultData->clear();
                        resultData->append(data.mid(10 + (wCnt * 2), 20));
                    }
                    break;
                }
                case 'C':
                    break;
                case 'D':
                    break;
                default:
                    return false;
                    break;
                }
                break;
            default:
                return false; // Invalid command for PA2
            }
        }
        if (command)
            *command = cmd;
        if (dataType)
            *dataType = dType;
        if (startAddr)
            *startAddr = sAddr;
        if (reqWordCnt)
            *reqWordCnt = wCnt;
        return true;
    }

private:

#ifndef CRC16_H_
    static quint16 calculateCrc16(const QByteArray &data) {
        quint16 crc = 0xFFFF;
        for (int i = 4; i < data.size(); ++i) {
            crc ^= (quint16)data[i];
            for (int j = 0; j < 8; ++j) {
                if (crc & 0x0001)
                    crc = (crc >> 1) ^ 0xA001;
                else
                    crc >>= 1;
            }
        }
        return crc;
    }

    static void addCrc16(QByteArray &data) {
        quint16 crc = calculateCrc16(data);
        data.append((char)(crc & 0xFF)); // LSB
        data.append((char)((crc >> 8) & 0xFF)); // MSB
    }
#else
    static quint16 getCrc16(QByteArray &data) {
        return MakeCRC16(&data.data()[4], data.size() - 4);
    }
#endif
};

#endif // CPROTOCOL_H

/*
 * crc16.h
 *
 *  Created on: 2017. 3. 22.
 *      Author: PARK
 */

#ifndef CRC16_H_
#define CRC16_H_

#ifdef __cplusplus
extern "C" {
#endif
/*
 * crc16.c
 *
 *  Created on: 2017. 3. 21.
 *      Author: PARK
 */


/* CRC-CCITT : X^16+X^12+X^5+1 ==> 0x8811 */

/* X^16+X^3+1 */
#define POLYNOMIAL	0x8005

/* */

/* 1000 0100 0000 1000 ==> X^16+X^11+X^4 */
/*#define POLYNOMIAL	0x8408*/
/*#define USE_CRC_TABLE*/

#define USE_CRC_TABLE	1

#ifndef USE_CRC_TABLE
    static unsigned short crc_table[256];
#else
#if 0
    static unsigned short crc_table[256] = {
	0x0000,
	0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
	0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027,
	0x0022, 0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D,
	0x8077, 0x0072, 0x0050, 0x8055, 0x805F, 0x005A, 0x804B,
	0x004E, 0x0044, 0x8041, 0x80C3, 0x00C6, 0x00CC, 0x80C9,
	0x00D8, 0x80DD, 0x80D7, 0x00D2, 0x00F0, 0x80F5, 0x80FF,
	0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1, 0x00A0, 0x80A5,
	0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1, 0x8093,
	0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
	0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197,
	0x0192, 0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE,
	0x01A4, 0x81A1, 0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB,
	0x01FE, 0x01F4, 0x81F1, 0x81D3, 0x01D6, 0x01DC, 0x81D9,
	0x01C8, 0x81CD, 0x81C7, 0x01C2, 0x0140, 0x8145, 0x814F,
	0x014A, 0x815B, 0x015E, 0x0154, 0x8151, 0x8173, 0x0176,
	0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162, 0x8123,
	0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
	0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104,
	0x8101, 0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D,
	0x8317, 0x0312, 0x0330, 0x8335, 0x833F, 0x033A, 0x832B,
	0x032E, 0x0324, 0x8321, 0x0360, 0x8365, 0x836F, 0x036A,
	0x837B, 0x037E, 0x0374, 0x8371, 0x8353, 0x0356, 0x035C,
	0x8359, 0x0348, 0x834D, 0x8347, 0x0342, 0x03C0, 0x83C5,
	0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1, 0x83F3,
	0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
	0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7,
	0x03B2, 0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E,
	0x0384, 0x8381, 0x0280, 0x8285, 0x828F, 0x028A, 0x829B,
	0x029E, 0x0294, 0x8291, 0x82B3, 0x02B6, 0x02BC, 0x82B9,
	0x02A8, 0x82AD, 0x82A7, 0x02A2, 0x82E3, 0x02E6, 0x02EC,
	0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2, 0x02D0, 0x82D5,
	0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1, 0x8243,
	0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
	0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264,
	0x8261, 0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E,
	0x0234, 0x8231, 0x8213, 0x0216, 0x021C, 0x8219, 0x0208,
	0x820D, 0x8207, 0x0202
};
#endif
    static unsigned short crc_table[] = {
    0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387,
    0x8408, 0x9489, 0xa50a, 0xb58b, 0xc60c, 0xd68d, 0xe70e, 0xf78f
    };
#endif

void gen_crc_table();
unsigned short update_crc(unsigned short crc_accum, char *data_blk_ptr,int data_blk_size);


void gen_crc_table()
{
#ifndef USE_CRC_TABLE
	register int i,j;
	register unsigned int		crc_accum;

	for(i=0; i<256; i++){
		crc_accum = ( (unsigned int) i << 8 );
		for(j=0; j<8; j++){
			if ( crc_accum & 0x8000L )
				crc_accum = ( crc_accum << 1 ) ^ POLYNOMIAL;
			else
				crc_accum = ( crc_accum << 1 );
		}
		crc_table[i] = crc_accum;

	}
#endif
	return;
}

unsigned short CalcCRC16(unsigned short crc, char data)
{
    unsigned short index;

    index = (crc ^ data) & 0x000f;
    crc = ((crc >> 4) & 0x0fff) ^ crc_table[index];

    data >>=4;
    index = (crc ^ data) & 0x000f;
    crc = ((crc >> 4) & 0x0fff) ^ crc_table[index];

    return crc;
}



unsigned short MakeCRC16(char *data, int len)
{
    unsigned short  crc = 0;
    int loop;

    for (loop = 0; loop < len; loop++) {
        crc = CalcCRC16(crc, data[loop]);
    }

    return crc;
}

unsigned short update_crc(unsigned short crc_accum, char *data,int len)
{
#if 0
    crc_accum = MakeCRC16(data, len);
#endif
    crc_accum = CalcCRC16(crc_accum, data[0]);
    return crc_accum;
}
#if 0
#if 0
unsigned int update_crc(unsigned int crc_accum, char *data_blk_ptr,int data_blk_size)
#endif
unsigned int update_crc(unsigned int crc_accum, char *data,int len)
{
#if 0
	register int i, j;

	for(j=0; j<data_blk_size; j++){
		i = ( (int) ( crc_accum >> 8) ^ *data_blk_ptr++ ) & 0xff;
		crc_accum = ( crc_accum << 8 ) ^ crc_table[i];
	}
	return crc_accum;
#endif

    unsigned int    index;
    int loop;

    for (loop = 0; loop < len; loop++) {
        index = (crc_accum ^ data[loop]) & 0x000f;
        crc_accum = ((crc_accum >> 4) & 0x0fff) ^ crc_table[index];

        data[loop] >>=4;
        index = (crc_accum ^ data[loop]) & 0x000f;
        crc_accum = ((crc_accum >> 4) & 0x0fff) ^ crc_table[index];
    }

    return crc_accum;

}
#endif




#endif /* CRC16_H_ */

#ifdef __cplusplus
}
#endif

{
    "commType": "UDP",
    "fadeEffect": true,
    "interval": "100",
    "ip": "127.0.0.0",
    "lastModelIndex": 1,
    "offsetX": -68.62702212524982,
    "offsetY": 344.39401464658874,
    "port": "47777",
    "zoomRate": 0.203908309340477
}

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

// #include <QApplication>
// #include "CMainWin.h"

// int main(int argc, char *argv[]) {
//     QApplication app(argc, argv);
//     CMainWin mainWindow;
//     mainWindow.show();
//     return app.exec();
// }

//#include "main.moc"

#############################################################################
# Makefile for building: CLumoMap
# Generated by qmake (3.1) (Qt 5.15.0)
# Project:  CLumoMap.pro
# Template: app
# Command: D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -o Makefile CLumoMap.pro -spec win32-msvc "CONFIG+=debug" "CONFIG+=qml_debug"
#############################################################################

MAKEFILE      = Makefile

EQ            = =

first: debug
install: debug-install
uninstall: debug-uninstall
QMAKE         = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe
DEL_FILE      = del
CHK_DIR_EXISTS= if not exist
MKDIR         = mkdir
COPY          = copy /y
COPY_FILE     = copy /y
COPY_DIR      = xcopy /s /q /y /i
INSTALL_FILE  = copy /y
INSTALL_PROGRAM = copy /y
INSTALL_DIR   = xcopy /s /q /y /i
QINSTALL      = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -install qinstall
QINSTALL_PROGRAM = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -install qinstall -exe
DEL_FILE      = del
SYMLINK       = $(QMAKE) -install ln -f -s
DEL_DIR       = rmdir
MOVE          = move
IDC           = idc
IDL           = midl
ZIP           = zip -r -9
DEF_FILE      = 
RES_FILE      = 
SED           = $(QMAKE) -install sed
MOVE          = move
SUBTARGETS    =  \
		debug \
		release


debug: $(MAKEFILE) FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug
debug-make_first: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug 
debug-all: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug all
debug-clean: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug clean
debug-distclean: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug distclean
debug-install: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug install
debug-uninstall: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug uninstall
release: $(MAKEFILE) FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release
release-make_first: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release 
release-all: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release all
release-clean: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release clean
release-distclean: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release distclean
release-install: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release install
release-uninstall: FORCE
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release uninstall

Makefile: CLumoMap.pro D:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc\qmake.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_pre.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\common\angle.conf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-desktop.conf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows_vulkan_sdk.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-vulkan.conf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-desktop.conf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\qconfig.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_accessibility_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bodymovin_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bootstrap_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designercomponents_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_devicediscovery_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_edid_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_egl_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_eventdispatcher_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fb_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fontdatabase_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_packetprotocol_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_platformcompositor_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldebug_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldevtools_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qtmultimediaquicktools_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickparticles_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickshapes_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_theme_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uiplugin.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_vulkan_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecoreheaders_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_windowsuiautomation_support_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_zlib_private.pri \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_functions.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_config.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc\qmake.conf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_post.prf \
		.qmake.stash \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-version.conf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\toolchain.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_pre.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\default_pre.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resolve_config.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds_post.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_post.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qml_debug.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\precompile_header.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\warn_on.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources_functions.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\moc.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\opengl.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\uic.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qmake_use.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\file_copies.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\testcase_targets.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exceptions.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\yacc.prf \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\lex.prf \
		CLumoMap.pro \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgets.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Gui.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Network.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrent.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPort.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Core.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\qtmain.prl \
		D:\Qt\5.15.0\msvc2019_64\mkspecs\features\build_pass.prf \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgetsd.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Guid.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Networkd.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrentd.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPortd.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\Qt5Cored.prl \
		D:\Qt\5.15.0\msvc2019_64\lib\qtmaind.prl
	$(QMAKE) -o Makefile CLumoMap.pro -spec win32-msvc "CONFIG+=debug" "CONFIG+=qml_debug"
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_pre.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\common\angle.conf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-desktop.conf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows_vulkan_sdk.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-vulkan.conf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-desktop.conf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\qconfig.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_accessibility_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bodymovin_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bootstrap_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designercomponents_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_devicediscovery_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_edid_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_egl_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_eventdispatcher_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fb_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fontdatabase_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_packetprotocol_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_platformcompositor_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldebug_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldevtools_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qtmultimediaquicktools_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickparticles_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickshapes_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_theme_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uiplugin.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_vulkan_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecoreheaders_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_windowsuiautomation_support_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_zlib_private.pri:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_functions.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_config.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc\qmake.conf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_post.prf:
.qmake.stash:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-version.conf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\toolchain.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_pre.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\default_pre.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resolve_config.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds_post.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_post.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qml_debug.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\precompile_header.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\warn_on.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources_functions.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\moc.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\opengl.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\uic.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qmake_use.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\file_copies.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\testcase_targets.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exceptions.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\yacc.prf:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\lex.prf:
CLumoMap.pro:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgets.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Gui.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Network.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrent.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPort.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Core.prl:
D:\Qt\5.15.0\msvc2019_64\lib\qtmain.prl:
D:\Qt\5.15.0\msvc2019_64\mkspecs\features\build_pass.prf:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgetsd.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Guid.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Networkd.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrentd.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPortd.prl:
D:\Qt\5.15.0\msvc2019_64\lib\Qt5Cored.prl:
D:\Qt\5.15.0\msvc2019_64\lib\qtmaind.prl:
qmake: FORCE
	@$(QMAKE) -o Makefile CLumoMap.pro -spec win32-msvc "CONFIG+=debug" "CONFIG+=qml_debug"

qmake_all: FORCE

make_first: debug-make_first release-make_first  FORCE
all: debug-all release-all  FORCE
clean: debug-clean release-clean  FORCE
	-$(DEL_FILE) CLumoMap.vc.pdb
	-$(DEL_FILE) antiGravity\debug\CLumoMap.ilk
	-$(DEL_FILE) antiGravity\debug\CLumoMap.idb
distclean: debug-distclean release-distclean  FORCE
	-$(DEL_FILE) Makefile
	-$(DEL_FILE) .qmake.stash antiGravity\debug\CLumoMap.pdb

debug-mocclean:
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug mocclean
release-mocclean:
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release mocclean
mocclean: debug-mocclean release-mocclean

debug-mocables:
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Debug mocables
release-mocables:
	@set MAKEFLAGS=$(MAKEFLAGS)
	$(MAKE) -f $(MAKEFILE).Release mocables
mocables: debug-mocables release-mocables

check: first

benchmark: first
FORCE:

$(MAKEFILE).Debug: Makefile
$(MAKEFILE).Release: Makefile

#############################################################################
# Makefile for building: CLumoMap
# Generated by qmake (3.1) (Qt 5.15.0)
# Project:  CLumoMap.pro
# Template: app
#############################################################################

MAKEFILE      = Makefile.Debug

EQ            = =

####### Compiler, tools and options

CC            = cl
CXX           = cl
DEFINES       = -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DQT_QML_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB
CFLAGS        = -nologo -Zc:wchar_t -FS -Zc:strictStrings -Zi -MDd -W3 -w44456 -w44457 -w44458 /Fddebug\CLumoMap.vc.pdb $(DEFINES)
CXXFLAGS      = -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -Zi -MDd -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -EHsc /Fddebug\CLumoMap.vc.pdb $(DEFINES)
INCPATH       = -I. -ID:\Qt\5.15.0\msvc2019_64\include -ID:\Qt\5.15.0\msvc2019_64\include\QtWidgets -ID:\Qt\5.15.0\msvc2019_64\include\QtGui -ID:\Qt\5.15.0\msvc2019_64\include\QtANGLE -ID:\Qt\5.15.0\msvc2019_64\include\QtNetwork -ID:\Qt\5.15.0\msvc2019_64\include\QtConcurrent -ID:\Qt\5.15.0\msvc2019_64\include\QtSerialPort -ID:\Qt\5.15.0\msvc2019_64\include\QtCore -Idebug -I/include -ID:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc 
LINKER        = link
LFLAGS        = /NOLOGO /DYNAMICBASE /NXCOMPAT /DEBUG /SUBSYSTEM:WINDOWS "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'"
LIBS          = D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgetsd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Guid.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Networkd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrentd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPortd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Cored.lib  D:\Qt\5.15.0\msvc2019_64\lib\qtmaind.lib /LIBPATH:C:\openssl\lib /LIBPATH:C:\Utils\my_sql\mysql-5.7.25-winx64\lib /LIBPATH:C:\Utils\postgresql\pgsql\lib shell32.lib 
QMAKE         = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe
DEL_FILE      = del
CHK_DIR_EXISTS= if not exist
MKDIR         = mkdir
COPY          = copy /y
COPY_FILE     = copy /y
COPY_DIR      = xcopy /s /q /y /i
INSTALL_FILE  = copy /y
INSTALL_PROGRAM = copy /y
INSTALL_DIR   = xcopy /s /q /y /i
QINSTALL      = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -install qinstall
QINSTALL_PROGRAM = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -install qinstall -exe
DEL_FILE      = del
SYMLINK       = $(QMAKE) -install ln -f -s
DEL_DIR       = rmdir
MOVE          = move
IDC           = idc
IDL           = midl
ZIP           = zip -r -9
DEF_FILE      = 
RES_FILE      = 
SED           = $(QMAKE) -install sed
MOVE          = move

####### Output directory

OBJECTS_DIR   = debug

####### Files

SOURCES       = CLumoMap.cpp \
		CComm.cpp \
		CMainWin.cpp \
		main.cpp debug\moc_CCloudPoints.cpp \
		debug\moc_CLumoMap.cpp \
		debug\moc_CComm.cpp \
		debug\moc_CMainWin.cpp \
		debug\moc_CCopyTableWidget.cpp
OBJECTS       = debug\CLumoMap.obj \
		debug\CComm.obj \
		debug\CMainWin.obj \
		debug\main.obj \
		debug\moc_CCloudPoints.obj \
		debug\moc_CLumoMap.obj \
		debug\moc_CComm.obj \
		debug\moc_CMainWin.obj \
		debug\moc_CCopyTableWidget.obj

DIST          =  CCloudPoints.h \
		CLumoMap.h \
		CComm.h \
		CMainWin.h \
		CProtocol.h \
		crc16.h \
		CCopyTableWidget.h CLumoMap.cpp \
		CComm.cpp \
		CMainWin.cpp \
		main.cpp
QMAKE_TARGET  = CLumoMap
DESTDIR        = antiGravity\debug\ #avoid trailing-slash linebreak
TARGET         = CLumoMap.exe
DESTDIR_TARGET = antiGravity\debug\CLumoMap.exe

####### Implicit rules

.SUFFIXES: .c .cpp .cc .cxx

{debug}.cpp{debug\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

{debug}.cc{debug\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

{debug}.cxx{debug\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

{debug}.c{debug\}.obj::
	$(CC) -c $(CFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

{.}.cpp{debug\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

{.}.cc{debug\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

{.}.cxx{debug\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

{.}.c{debug\}.obj::
	$(CC) -c $(CFLAGS) $(INCPATH) -Fodebug\ @<<
	$<
<<

####### Build rules

first: all
all: Makefile.Debug  antiGravity\debug\CLumoMap.exe

antiGravity\debug\CLumoMap.exe: D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgetsd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Guid.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Networkd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrentd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPortd.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Cored.lib D:\Qt\5.15.0\msvc2019_64\lib\qtmaind.lib $(OBJECTS) 
	$(LINKER) $(LFLAGS) /MANIFEST:embed /OUT:$(DESTDIR_TARGET) @<<
debug\CLumoMap.obj debug\CComm.obj debug\CMainWin.obj debug\main.obj debug\moc_CCloudPoints.obj debug\moc_CLumoMap.obj debug\moc_CComm.obj debug\moc_CMainWin.obj debug\moc_CCopyTableWidget.obj
$(LIBS)
<<

qmake: FORCE
	@$(QMAKE) -o Makefile.Debug CLumoMap.pro -spec win32-msvc "CONFIG+=debug" "CONFIG+=qml_debug"

qmake_all: FORCE

dist:
	$(ZIP) CLumoMap.zip $(SOURCES) $(DIST) CLumoMap.pro D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_pre.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\angle.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-desktop.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows_vulkan_sdk.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-vulkan.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-desktop.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\qconfig.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_accessibility_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bodymovin_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bootstrap_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designercomponents_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_devicediscovery_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_edid_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_egl_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_eventdispatcher_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fb_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fontdatabase_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_packetprotocol_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_platformcompositor_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldebug_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldevtools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qtmultimediaquicktools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickparticles_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickshapes_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_theme_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uiplugin.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_vulkan_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecoreheaders_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_windowsuiautomation_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_zlib_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_functions.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_config.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc\qmake.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_post.prf .qmake.stash D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-version.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\toolchain.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_pre.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\default_pre.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resolve_config.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds_post.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_post.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\build_pass.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qml_debug.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\precompile_header.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\warn_on.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources_functions.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\moc.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\opengl.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\uic.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qmake_use.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\file_copies.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\testcase_targets.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exceptions.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\yacc.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\lex.prf CLumoMap.pro D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgetsd.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Guid.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Networkd.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrentd.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPortd.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Cored.prl D:\Qt\5.15.0\msvc2019_64\lib\qtmaind.prl    D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy.cpp CCloudPoints.h CLumoMap.h CComm.h CMainWin.h CProtocol.h crc16.h CCopyTableWidget.h  CLumoMap.cpp CComm.cpp CMainWin.cpp main.cpp     

clean: compiler_clean 
	-$(DEL_FILE) debug\CLumoMap.obj debug\CComm.obj debug\CMainWin.obj debug\main.obj debug\moc_CCloudPoints.obj debug\moc_CLumoMap.obj debug\moc_CComm.obj debug\moc_CMainWin.obj debug\moc_CCopyTableWidget.obj
	-$(DEL_FILE) debug\CLumoMap.vc.pdb antiGravity\debug\CLumoMap.ilk antiGravity\debug\CLumoMap.idb

distclean: clean 
	-$(DEL_FILE) .qmake.stash antiGravity\debug\CLumoMap.pdb
	-$(DEL_FILE) $(DESTDIR_TARGET)
	-$(DEL_FILE) Makefile.Debug

mocclean: compiler_moc_header_clean compiler_moc_objc_header_clean compiler_moc_source_clean

mocables: compiler_moc_header_make_all compiler_moc_objc_header_make_all compiler_moc_source_make_all

check: first

benchmark: first

compiler_no_pch_compiler_make_all:
compiler_no_pch_compiler_clean:
compiler_rcc_make_all:
compiler_rcc_clean:
compiler_moc_predefs_make_all: debug\moc_predefs.h
compiler_moc_predefs_clean:
	-$(DEL_FILE) debug\moc_predefs.h
debug\moc_predefs.h: D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy.cpp
	cl -BxD:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -Zi -MDd -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy.cpp 2>NUL >debug\moc_predefs.h

compiler_moc_header_make_all: debug\moc_CCloudPoints.cpp debug\moc_CLumoMap.cpp debug\moc_CComm.cpp debug\moc_CMainWin.cpp debug\moc_CCopyTableWidget.cpp
compiler_moc_header_clean:
	-$(DEL_FILE) debug\moc_CCloudPoints.cpp debug\moc_CLumoMap.cpp debug\moc_CComm.cpp debug\moc_CMainWin.cpp debug\moc_CCopyTableWidget.cpp
debug\moc_CCloudPoints.cpp: CCloudPoints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVector \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtMath \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDataStream \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		debug\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/debug/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o debug\moc_CCloudPoints.cpp

debug\moc_CLumoMap.cpp: CLumoMap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgets \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgetsDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGui \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGuiDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qabstracttextdocumentlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qglyphrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrawfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontdatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessible.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessiblebridge.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbackingstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMargins \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QRect \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurfaceformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbitmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolorspace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolortransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdesktopservices.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdrag.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericpluginfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengineplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimageiohandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagereader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagewriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix4x4.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector3d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector4d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qquaternion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmovie.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qoffscreensurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qt_windows.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\KHR\khrplatform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengles2ext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglcontext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QScopedPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QSurfaceFormat \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglversionfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengldebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglextrafunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglframebufferobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpixeltransferoptions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSharedDataPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglshaderprogram.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltexture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltextureblitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix3x3 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix4x4 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltimerquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglvertexarrayobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDeviceWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevicewindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDevice \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QOpenGLContext \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QImage \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagedpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagelayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagesize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainterpath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpdfwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpicture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpictureformatplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmapcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrasterwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSize \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSizeF \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QTransform \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsessionmanager.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstandarditemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstatictext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstylehints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsyntaxhighlighter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentfragment.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtexttable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaccessiblewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qactiongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qboxlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qbuttongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcalendarwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcheckbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolordialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolumnview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommandlinkbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qpushbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommonstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcompleter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatawidgetmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatetimeedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdial.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialogbuttonbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdirmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfileiconprovider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdockwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdrawutil.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qerrormessage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfiledialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfilesystemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfocusframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qformlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesturerecognizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsanchorlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicseffect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitemanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslinearlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsproxywidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicswidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsscene.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicssceneevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicstransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QVector3D \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgroupbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qheaderview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qinputdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlineedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemeditorfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeyeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeysequenceedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlabel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlcdnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmainwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdiarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdisubwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenu.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenubar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmessagebox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmouseeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qopenglwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qplaintextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qproxystyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCommonStyle \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qradiobutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscroller.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QScrollerProperties \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollerproperties.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMetaType \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVariant \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qshortcut.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizegrip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplashscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstatusbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleditemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylefactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylepainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsystemtrayicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextbrowser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtooltip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreeview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidgetitemiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundogroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundostack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundoview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwhatsthis.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidgetaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwizard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QString \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMap \
		debug\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/debug/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o debug\moc_CLumoMap.cpp

debug\moc_CComm.cpp: CComm.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFuture \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFutureWatcher \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMutex \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrent \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrentDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentcompilertest.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrent_global.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilterkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentiteratekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmedian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentthreadengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmapkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentreducekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfunctionwrappers.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrunbase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentstoredfunctioncall.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QByteArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QHostAddress \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qhostaddress.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetworkglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetwork-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qabstractsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QTcpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtcpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QUdpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qudpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPort \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialport.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPortInfo \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		debug\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/debug/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o debug\moc_CComm.cpp

debug\moc_CMainWin.cpp: CMainWin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QMainWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmainwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QMenuBar \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenubar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenu.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qactiongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QStatusBar \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstatusbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMutex \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCombobox \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QDockWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdockwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QVBoxLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qboxlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QPushButton \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qpushbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QHeaderView \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qheaderview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCheckBox \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcheckbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QJsonDocument \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QJsonObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QJsonArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFile \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDir \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDebug \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QMessageBox \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmessagebox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLabel \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlabel.h \
		CLumoMap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgets \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgetsDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGui \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGuiDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qabstracttextdocumentlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qglyphrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrawfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontdatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessible.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessiblebridge.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbackingstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMargins \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QRect \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurfaceformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbitmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolorspace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolortransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdesktopservices.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdrag.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericpluginfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengineplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimageiohandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagereader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagewriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix4x4.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector3d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector4d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qquaternion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmovie.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qoffscreensurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qt_windows.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\KHR\khrplatform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengles2ext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglcontext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QScopedPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QSurfaceFormat \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglversionfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengldebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglextrafunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglframebufferobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpixeltransferoptions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSharedDataPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglshaderprogram.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltexture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltextureblitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix3x3 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix4x4 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltimerquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglvertexarrayobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDeviceWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevicewindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDevice \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QOpenGLContext \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QImage \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagedpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagelayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagesize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainterpath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpdfwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpicture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpictureformatplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmapcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrasterwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSize \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSizeF \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QTransform \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsessionmanager.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstandarditemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstatictext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstylehints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsyntaxhighlighter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentfragment.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtexttable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaccessiblewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qbuttongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcalendarwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolordialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolumnview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommandlinkbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommonstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcompleter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatawidgetmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatetimeedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdial.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialogbuttonbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdirmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfileiconprovider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdrawutil.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qerrormessage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfiledialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfilesystemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfocusframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qformlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesturerecognizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsanchorlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicseffect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitemanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslinearlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsproxywidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicswidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsscene.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicssceneevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicstransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QVector3D \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgroupbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qinputdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlineedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemeditorfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeyeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeysequenceedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlcdnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdiarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdisubwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmouseeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qopenglwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qplaintextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qproxystyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCommonStyle \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qradiobutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscroller.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QScrollerProperties \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollerproperties.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMetaType \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVariant \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qshortcut.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizegrip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplashscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleditemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylefactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylepainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsystemtrayicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextbrowser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtooltip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreeview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidgetitemiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundogroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundostack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundoview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwhatsthis.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidgetaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwizard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QString \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMap \
		CCloudPoints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVector \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtMath \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDataStream \
		CComm.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFuture \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFutureWatcher \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrent \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrentDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentcompilertest.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrent_global.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilterkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentiteratekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmedian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentthreadengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmapkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentreducekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfunctionwrappers.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrunbase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentstoredfunctioncall.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QByteArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QHostAddress \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qhostaddress.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetworkglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetwork-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qabstractsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QTcpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtcpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QUdpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qudpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPort \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialport.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPortInfo \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportinfo.h \
		CProtocol.h \
		crc16.h \
		CCopyTableWidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QTableWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QKeyEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QApplication \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QClipboard \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QItemSelectionRange \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QStyleFactory \
		debug\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/debug/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o debug\moc_CMainWin.cpp

debug\moc_CCopyTableWidget.cpp: CCopyTableWidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QTableWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QKeyEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QApplication \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QClipboard \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QItemSelectionRange \
		debug\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/debug/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCopyTableWidget.h -o debug\moc_CCopyTableWidget.cpp

compiler_moc_objc_header_make_all:
compiler_moc_objc_header_clean:
compiler_moc_source_make_all:
compiler_moc_source_clean:
compiler_uic_make_all:
compiler_uic_clean:
compiler_yacc_decl_make_all:
compiler_yacc_decl_clean:
compiler_yacc_impl_make_all:
compiler_yacc_impl_clean:
compiler_lex_make_all:
compiler_lex_clean:
compiler_clean: compiler_moc_predefs_clean compiler_moc_header_clean 



####### Compile

debug\CLumoMap.obj: CLumoMap.cpp CLumoMap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgets \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgetsDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGui \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGuiDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qabstracttextdocumentlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qglyphrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrawfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontdatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessible.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessiblebridge.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbackingstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMargins \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QRect \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurfaceformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbitmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolorspace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolortransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdesktopservices.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdrag.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericpluginfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengineplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimageiohandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagereader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagewriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix4x4.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector3d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector4d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qquaternion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmovie.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qoffscreensurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qt_windows.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\KHR\khrplatform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengles2ext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglcontext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QScopedPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QSurfaceFormat \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglversionfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengldebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglextrafunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglframebufferobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpixeltransferoptions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSharedDataPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglshaderprogram.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltexture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltextureblitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix3x3 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix4x4 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltimerquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglvertexarrayobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDeviceWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevicewindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDevice \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QOpenGLContext \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QImage \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagedpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagelayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagesize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainterpath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpdfwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpicture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpictureformatplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmapcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrasterwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSize \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSizeF \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QTransform \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsessionmanager.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstandarditemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstatictext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstylehints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsyntaxhighlighter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentfragment.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtexttable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaccessiblewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qactiongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qboxlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qbuttongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcalendarwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcheckbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolordialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolumnview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommandlinkbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qpushbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommonstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcompleter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatawidgetmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatetimeedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdial.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialogbuttonbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdirmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfileiconprovider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdockwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdrawutil.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qerrormessage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfiledialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfilesystemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfocusframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qformlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesturerecognizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsanchorlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicseffect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitemanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslinearlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsproxywidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicswidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsscene.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicssceneevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicstransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QVector3D \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgroupbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qheaderview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qinputdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlineedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemeditorfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeyeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeysequenceedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlabel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlcdnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmainwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdiarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdisubwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenu.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenubar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmessagebox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmouseeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qopenglwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qplaintextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qproxystyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCommonStyle \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qradiobutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscroller.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QScrollerProperties \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollerproperties.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMetaType \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVariant \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qshortcut.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizegrip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplashscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstatusbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleditemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylefactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylepainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsystemtrayicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextbrowser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtooltip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreeview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidgetitemiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundogroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundostack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundoview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwhatsthis.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidgetaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwizard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QString \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMap

debug\CComm.obj: CComm.cpp CComm.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFuture \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFutureWatcher \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMutex \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrent \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrentDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentcompilertest.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrent_global.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilterkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentiteratekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmedian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentthreadengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmapkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentreducekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfunctionwrappers.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrunbase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentstoredfunctioncall.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QByteArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QHostAddress \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qhostaddress.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetworkglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetwork-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qabstractsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QTcpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtcpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QUdpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qudpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPort \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialport.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPortInfo \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList

debug\CMainWin.obj: CMainWin.cpp 

debug\main.obj: main.cpp 

debug\moc_CCloudPoints.obj: debug\moc_CCloudPoints.cpp 

debug\moc_CLumoMap.obj: debug\moc_CLumoMap.cpp 

debug\moc_CComm.obj: debug\moc_CComm.cpp 

debug\moc_CMainWin.obj: debug\moc_CMainWin.cpp 

debug\moc_CCopyTableWidget.obj: debug\moc_CCopyTableWidget.cpp 

####### Install

install:  FORCE

uninstall:  FORCE

FORCE:


#############################################################################
# Makefile for building: CLumoMap
# Generated by qmake (3.1) (Qt 5.15.0)
# Project:  CLumoMap.pro
# Template: app
#############################################################################

MAKEFILE      = Makefile.Release

EQ            = =

####### Compiler, tools and options

CC            = cl
CXX           = cl
DEFINES       = -DUNICODE -D_UNICODE -DWIN32 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -DNDEBUG -DQT_QML_DEBUG -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CONCURRENT_LIB -DQT_SERIALPORT_LIB -DQT_CORE_LIB
CFLAGS        = -nologo -Zc:wchar_t -FS -Zc:strictStrings -O2 -MD -W3 -w44456 -w44457 -w44458 $(DEFINES)
CXXFLAGS      = -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -O2 -MD -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -EHsc $(DEFINES)
INCPATH       = -I. -ID:\Qt\5.15.0\msvc2019_64\include -ID:\Qt\5.15.0\msvc2019_64\include\QtWidgets -ID:\Qt\5.15.0\msvc2019_64\include\QtGui -ID:\Qt\5.15.0\msvc2019_64\include\QtANGLE -ID:\Qt\5.15.0\msvc2019_64\include\QtNetwork -ID:\Qt\5.15.0\msvc2019_64\include\QtConcurrent -ID:\Qt\5.15.0\msvc2019_64\include\QtSerialPort -ID:\Qt\5.15.0\msvc2019_64\include\QtCore -Irelease -I/include -ID:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc 
LINKER        = link
LFLAGS        = /NOLOGO /DYNAMICBASE /NXCOMPAT /OPT:REF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'"
LIBS          = D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgets.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Gui.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Network.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrent.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPort.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Core.lib  D:\Qt\5.15.0\msvc2019_64\lib\qtmain.lib /LIBPATH:C:\openssl\lib /LIBPATH:C:\Utils\my_sql\mysql-5.7.25-winx64\lib /LIBPATH:C:\Utils\postgresql\pgsql\lib shell32.lib 
QMAKE         = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe
DEL_FILE      = del
CHK_DIR_EXISTS= if not exist
MKDIR         = mkdir
COPY          = copy /y
COPY_FILE     = copy /y
COPY_DIR      = xcopy /s /q /y /i
INSTALL_FILE  = copy /y
INSTALL_PROGRAM = copy /y
INSTALL_DIR   = xcopy /s /q /y /i
QINSTALL      = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -install qinstall
QINSTALL_PROGRAM = D:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -install qinstall -exe
DEL_FILE      = del
SYMLINK       = $(QMAKE) -install ln -f -s
DEL_DIR       = rmdir
MOVE          = move
IDC           = idc
IDL           = midl
ZIP           = zip -r -9
DEF_FILE      = 
RES_FILE      = 
SED           = $(QMAKE) -install sed
MOVE          = move

####### Output directory

OBJECTS_DIR   = release

####### Files

SOURCES       = CLumoMap.cpp \
		CComm.cpp \
		CMainWin.cpp \
		main.cpp release\moc_CCloudPoints.cpp \
		release\moc_CLumoMap.cpp \
		release\moc_CComm.cpp \
		release\moc_CMainWin.cpp \
		release\moc_CCopyTableWidget.cpp
OBJECTS       = release\CLumoMap.obj \
		release\CComm.obj \
		release\CMainWin.obj \
		release\main.obj \
		release\moc_CCloudPoints.obj \
		release\moc_CLumoMap.obj \
		release\moc_CComm.obj \
		release\moc_CMainWin.obj \
		release\moc_CCopyTableWidget.obj

DIST          =  CCloudPoints.h \
		CLumoMap.h \
		CComm.h \
		CMainWin.h \
		CProtocol.h \
		crc16.h \
		CCopyTableWidget.h CLumoMap.cpp \
		CComm.cpp \
		CMainWin.cpp \
		main.cpp
QMAKE_TARGET  = CLumoMap
DESTDIR        = antiGravity\release\ #avoid trailing-slash linebreak
TARGET         = CLumoMap.exe
DESTDIR_TARGET = antiGravity\release\CLumoMap.exe

####### Implicit rules

.SUFFIXES: .c .cpp .cc .cxx

{release}.cpp{release\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

{release}.cc{release\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

{release}.cxx{release\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

{release}.c{release\}.obj::
	$(CC) -c $(CFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

{.}.cpp{release\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

{.}.cc{release\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

{.}.cxx{release\}.obj::
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

{.}.c{release\}.obj::
	$(CC) -c $(CFLAGS) $(INCPATH) -Forelease\ @<<
	$<
<<

####### Build rules

first: all
all: Makefile.Release  antiGravity\release\CLumoMap.exe

antiGravity\release\CLumoMap.exe: D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgets.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Gui.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Network.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrent.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPort.lib D:\Qt\5.15.0\msvc2019_64\lib\Qt5Core.lib D:\Qt\5.15.0\msvc2019_64\lib\qtmain.lib $(OBJECTS) 
	$(LINKER) $(LFLAGS) /MANIFEST:embed /OUT:$(DESTDIR_TARGET) @<<
release\CLumoMap.obj release\CComm.obj release\CMainWin.obj release\main.obj release\moc_CCloudPoints.obj release\moc_CLumoMap.obj release\moc_CComm.obj release\moc_CMainWin.obj release\moc_CCopyTableWidget.obj
$(LIBS)
<<

qmake: FORCE
	@$(QMAKE) -o Makefile.Release CLumoMap.pro -spec win32-msvc "CONFIG+=debug" "CONFIG+=qml_debug"

qmake_all: FORCE

dist:
	$(ZIP) CLumoMap.zip $(SOURCES) $(DIST) CLumoMap.pro D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_pre.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\angle.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-desktop.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows_vulkan_sdk.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\windows-vulkan.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-desktop.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\qconfig.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3danimation_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dcore_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dextras_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dinput_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dlogic_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquick_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickanimation_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickextras_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickinput_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickrender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3dquickscene2d_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_3drender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_accessibility_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axbase_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axcontainer_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_axserver_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bluetooth_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bodymovin_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_bootstrap_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_charts_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_concurrent_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_core_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_datavisualization_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_dbus_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designer_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_designercomponents_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_devicediscovery_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_edid_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_egl_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_eventdispatcher_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fb_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_fontdatabase_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gamepad_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_gui_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_help_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_location_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimedia_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_multimediawidgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_network_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_networkauth_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_nfc_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_opengl_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_openglextensions_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_packetprotocol_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_platformcompositor_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioning_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_positioningquick_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_printsupport_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qml_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldebug_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmldevtools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlmodels_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmltest_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qmlworkerscript_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_qtmultimediaquicktools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3d_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dassetimport_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3drender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3druntimerender_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick3dutils_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quick_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickcontrols2_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickparticles_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickshapes_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quicktemplates2_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_quickwidgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_remoteobjects_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_repparser_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_script_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scripttools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_scxml_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sensors_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialbus_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_serialport_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_sql_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_svg_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_testlib_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_texttospeech_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_theme_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uiplugin.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_uitools_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_vulkan_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webchannel_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webengine_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecore_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginecoreheaders_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webenginewidgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_websockets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_webview_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_widgets_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_windowsuiautomation_support_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_winextras_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xml_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_xmlpatterns_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\modules\qt_lib_zlib_private.pri D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_functions.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt_config.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\win32-msvc\qmake.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\spec_post.prf .qmake.stash D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\common\msvc-version.conf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\toolchain.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_pre.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\default_pre.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resolve_config.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exclusive_builds_post.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\default_post.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\build_pass.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qml_debug.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\precompile_header.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\warn_on.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qt.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources_functions.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\resources.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\moc.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\opengl.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\uic.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\qmake_use.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\file_copies.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\win32\windows.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\testcase_targets.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\exceptions.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\yacc.prf D:\Qt\5.15.0\msvc2019_64\mkspecs\features\lex.prf CLumoMap.pro D:\Qt\5.15.0\msvc2019_64\lib\Qt5Widgets.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Gui.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Network.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Concurrent.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5SerialPort.prl D:\Qt\5.15.0\msvc2019_64\lib\Qt5Core.prl D:\Qt\5.15.0\msvc2019_64\lib\qtmain.prl    D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy.cpp CCloudPoints.h CLumoMap.h CComm.h CMainWin.h CProtocol.h crc16.h CCopyTableWidget.h  CLumoMap.cpp CComm.cpp CMainWin.cpp main.cpp     

clean: compiler_clean 
	-$(DEL_FILE) release\CLumoMap.obj release\CComm.obj release\CMainWin.obj release\main.obj release\moc_CCloudPoints.obj release\moc_CLumoMap.obj release\moc_CComm.obj release\moc_CMainWin.obj release\moc_CCopyTableWidget.obj

distclean: clean 
	-$(DEL_FILE) .qmake.stash
	-$(DEL_FILE) $(DESTDIR_TARGET)
	-$(DEL_FILE) Makefile.Release

mocclean: compiler_moc_header_clean compiler_moc_objc_header_clean compiler_moc_source_clean

mocables: compiler_moc_header_make_all compiler_moc_objc_header_make_all compiler_moc_source_make_all

check: first

benchmark: first

compiler_no_pch_compiler_make_all:
compiler_no_pch_compiler_clean:
compiler_rcc_make_all:
compiler_rcc_clean:
compiler_moc_predefs_make_all: release\moc_predefs.h
compiler_moc_predefs_clean:
	-$(DEL_FILE) release\moc_predefs.h
release\moc_predefs.h: D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy.cpp
	cl -BxD:\Qt\5.15.0\msvc2019_64\bin\qmake.exe -nologo -Zc:wchar_t -FS -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus /utf-8 -O2 -MD -W3 -w34100 -w34189 -w44996 -w44456 -w44457 -w44458 -wd4577 -wd4467 -E D:\Qt\5.15.0\msvc2019_64\mkspecs\features\data\dummy.cpp 2>NUL >release\moc_predefs.h

compiler_moc_header_make_all: release\moc_CCloudPoints.cpp release\moc_CLumoMap.cpp release\moc_CComm.cpp release\moc_CMainWin.cpp release\moc_CCopyTableWidget.cpp
compiler_moc_header_clean:
	-$(DEL_FILE) release\moc_CCloudPoints.cpp release\moc_CLumoMap.cpp release\moc_CComm.cpp release\moc_CMainWin.cpp release\moc_CCopyTableWidget.cpp
release\moc_CCloudPoints.cpp: CCloudPoints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVector \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtMath \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDataStream \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		release\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/release/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCloudPoints.h -o release\moc_CCloudPoints.cpp

release\moc_CLumoMap.cpp: CLumoMap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgets \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgetsDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGui \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGuiDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qabstracttextdocumentlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qglyphrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrawfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontdatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessible.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessiblebridge.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbackingstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMargins \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QRect \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurfaceformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbitmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolorspace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolortransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdesktopservices.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdrag.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericpluginfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengineplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimageiohandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagereader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagewriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix4x4.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector3d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector4d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qquaternion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmovie.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qoffscreensurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qt_windows.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\KHR\khrplatform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengles2ext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglcontext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QScopedPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QSurfaceFormat \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglversionfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengldebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglextrafunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglframebufferobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpixeltransferoptions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSharedDataPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglshaderprogram.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltexture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltextureblitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix3x3 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix4x4 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltimerquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglvertexarrayobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDeviceWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevicewindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDevice \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QOpenGLContext \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QImage \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagedpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagelayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagesize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainterpath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpdfwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpicture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpictureformatplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmapcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrasterwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSize \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSizeF \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QTransform \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsessionmanager.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstandarditemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstatictext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstylehints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsyntaxhighlighter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentfragment.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtexttable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaccessiblewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qactiongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qboxlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qbuttongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcalendarwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcheckbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolordialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolumnview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommandlinkbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qpushbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommonstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcompleter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatawidgetmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatetimeedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdial.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialogbuttonbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdirmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfileiconprovider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdockwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdrawutil.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qerrormessage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfiledialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfilesystemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfocusframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qformlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesturerecognizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsanchorlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicseffect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitemanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslinearlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsproxywidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicswidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsscene.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicssceneevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicstransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QVector3D \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgroupbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qheaderview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qinputdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlineedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemeditorfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeyeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeysequenceedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlabel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlcdnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmainwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdiarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdisubwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenu.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenubar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmessagebox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmouseeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qopenglwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qplaintextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qproxystyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCommonStyle \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qradiobutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscroller.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QScrollerProperties \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollerproperties.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMetaType \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVariant \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qshortcut.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizegrip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplashscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstatusbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleditemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylefactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylepainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsystemtrayicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextbrowser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtooltip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreeview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidgetitemiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundogroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundostack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundoview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwhatsthis.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidgetaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwizard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QString \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMap \
		release\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/release/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CLumoMap.h -o release\moc_CLumoMap.cpp

release\moc_CComm.cpp: CComm.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFuture \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFutureWatcher \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMutex \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrent \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrentDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentcompilertest.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrent_global.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilterkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentiteratekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmedian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentthreadengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmapkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentreducekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfunctionwrappers.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrunbase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentstoredfunctioncall.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QByteArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QHostAddress \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qhostaddress.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetworkglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetwork-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qabstractsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QTcpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtcpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QUdpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qudpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPort \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialport.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPortInfo \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		release\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/release/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CComm.h -o release\moc_CComm.cpp

release\moc_CMainWin.cpp: CMainWin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QMainWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmainwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QMenuBar \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenubar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenu.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qactiongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QStatusBar \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstatusbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMutex \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCombobox \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QDockWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdockwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QVBoxLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qboxlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QPushButton \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qpushbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QHeaderView \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qheaderview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCheckBox \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcheckbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QJsonDocument \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QJsonObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QJsonArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFile \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDir \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDebug \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QMessageBox \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmessagebox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLabel \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlabel.h \
		CLumoMap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgets \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgetsDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGui \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGuiDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qabstracttextdocumentlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qglyphrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrawfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontdatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessible.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessiblebridge.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbackingstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMargins \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QRect \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurfaceformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbitmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolorspace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolortransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdesktopservices.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdrag.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericpluginfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengineplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimageiohandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagereader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagewriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix4x4.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector3d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector4d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qquaternion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmovie.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qoffscreensurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qt_windows.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\KHR\khrplatform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengles2ext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglcontext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QScopedPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QSurfaceFormat \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglversionfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengldebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglextrafunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglframebufferobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpixeltransferoptions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSharedDataPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglshaderprogram.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltexture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltextureblitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix3x3 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix4x4 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltimerquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglvertexarrayobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDeviceWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevicewindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDevice \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QOpenGLContext \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QImage \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagedpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagelayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagesize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainterpath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpdfwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpicture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpictureformatplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmapcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrasterwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSize \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSizeF \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QTransform \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsessionmanager.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstandarditemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstatictext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstylehints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsyntaxhighlighter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentfragment.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtexttable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaccessiblewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qbuttongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcalendarwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolordialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolumnview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommandlinkbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommonstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcompleter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatawidgetmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatetimeedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdial.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialogbuttonbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdirmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfileiconprovider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdrawutil.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qerrormessage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfiledialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfilesystemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfocusframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qformlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesturerecognizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsanchorlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicseffect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitemanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslinearlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsproxywidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicswidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsscene.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicssceneevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicstransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QVector3D \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgroupbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qinputdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlineedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemeditorfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeyeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeysequenceedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlcdnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdiarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdisubwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmouseeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qopenglwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qplaintextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qproxystyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCommonStyle \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qradiobutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscroller.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QScrollerProperties \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollerproperties.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMetaType \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVariant \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qshortcut.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizegrip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplashscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleditemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylefactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylepainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsystemtrayicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextbrowser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtooltip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreeview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidgetitemiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundogroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundostack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundoview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwhatsthis.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidgetaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwizard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QString \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMap \
		CCloudPoints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVector \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtMath \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDataStream \
		CComm.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFuture \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFutureWatcher \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrent \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrentDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentcompilertest.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrent_global.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilterkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentiteratekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmedian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentthreadengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmapkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentreducekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfunctionwrappers.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrunbase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentstoredfunctioncall.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QByteArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QHostAddress \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qhostaddress.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetworkglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetwork-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qabstractsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QTcpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtcpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QUdpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qudpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPort \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialport.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPortInfo \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportinfo.h \
		CProtocol.h \
		crc16.h \
		CCopyTableWidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QTableWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QKeyEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QApplication \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QClipboard \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QItemSelectionRange \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QStyleFactory \
		release\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/release/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CMainWin.h -o release\moc_CMainWin.cpp

release\moc_CCopyTableWidget.cpp: CCopyTableWidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QTableWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QKeyEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QApplication \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QClipboard \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QItemSelectionRange \
		release\moc_predefs.h \
		D:\Qt\5.15.0\msvc2019_64\bin\moc.exe
	D:\Qt\5.15.0\msvc2019_64\bin\moc.exe $(DEFINES) --compiler-flavor=msvc --include D:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch/release/moc_predefs.h -ID:/Qt/5.15.0/msvc2019_64/mkspecs/win32-msvc -ID:/DeveSW/_TI-DSP/_Study_Develope/___zsc_Libraries/UI_Project/LiDAR/LiDAR_CLumoMap_4Ch -ID:/Qt/5.15.0/msvc2019_64/include -ID:/Qt/5.15.0/msvc2019_64/include/QtWidgets -ID:/Qt/5.15.0/msvc2019_64/include/QtGui -ID:/Qt/5.15.0/msvc2019_64/include/QtANGLE -ID:/Qt/5.15.0/msvc2019_64/include/QtNetwork -ID:/Qt/5.15.0/msvc2019_64/include/QtConcurrent -ID:/Qt/5.15.0/msvc2019_64/include/QtSerialPort -ID:/Qt/5.15.0/msvc2019_64/include/QtCore -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.40.33807\ATLMFC\include" -I"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\VS\include" -I"C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\ucrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\um" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\shared" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\winrt" -I"C:\Program Files (x86)\Windows Kits\10\\include\10.0.26100.0\\cppwinrt" -I"C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um" CCopyTableWidget.h -o release\moc_CCopyTableWidget.cpp

compiler_moc_objc_header_make_all:
compiler_moc_objc_header_clean:
compiler_moc_source_make_all:
compiler_moc_source_clean:
compiler_uic_make_all:
compiler_uic_clean:
compiler_yacc_decl_make_all:
compiler_yacc_decl_clean:
compiler_yacc_impl_make_all:
compiler_yacc_impl_clean:
compiler_lex_make_all:
compiler_lex_clean:
compiler_clean: compiler_moc_predefs_clean compiler_moc_header_clean 



####### Compile

release\CLumoMap.obj: CLumoMap.cpp CLumoMap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgets \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QtWidgetsDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGui \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QtGuiDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtgui-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qabstracttextdocumentlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgb.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrgba64.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindowdefs_win.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qregion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qkeysequence.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector2d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtouchdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbrush.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpolygon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixelformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qglyphrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrawfont.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontdatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpalette.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessible.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessiblebridge.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qaccessibleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbackingstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QEvent \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMargins \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QRect \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsurfaceformat.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcursor.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qbitmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qclipboard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolorspace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qcolortransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdesktopservices.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qdrag.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qfontmetrics.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericmatrix.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qgenericpluginfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qguiapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qinputmethod.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qiconengineplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimageiohandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagereader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qimagewriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmatrix4x4.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector3d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvector4d.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qquaternion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qmovie.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qoffscreensurface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qt_windows.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES3\gl3platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\KHR\khrplatform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtANGLE\GLES2\gl2platform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengles2ext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglcontext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QScopedPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QSurfaceFormat \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglversionfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengldebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglextrafunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglframebufferobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglpixeltransferoptions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSharedDataPointer \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglshaderprogram.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltexture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltextureblitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix3x3 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QMatrix4x4 \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopengltimerquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglvertexarrayobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qopenglwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDeviceWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintdevicewindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QWindow \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QPaintDevice \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QOpenGLContext \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QImage \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagedpaintdevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagelayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpagesize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpaintengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpainterpath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpdfwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpicture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpictureformatplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qpixmapcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qrasterwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSize \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QSizeF \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QTransform \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsessionmanager.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstandarditemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstatictext.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qstylehints.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qsyntaxhighlighter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentfragment.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextdocumentwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtextlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtexttable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qvalidator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\qtguiversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgets-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizepolicy.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractslider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtabwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qrubberband.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractitemview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qabstractscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaccessiblewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qactiongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdesktopwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qboxlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qbuttongroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcalendarwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcheckbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolordialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcolumnview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommandlinkbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qpushbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcommonstyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qcompleter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatawidgetmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdatetimeedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdial.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdialogbuttonbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdirmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfileiconprovider.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdockwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qdrawutil.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qerrormessage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfiledialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfilesystemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfocusframe.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontcombobox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qfontdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qformlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QLayout \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgesturerecognizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsanchorlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslayoutitem.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicseffect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsgridlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsitemanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicslinearlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsproxywidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicswidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsscene.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicssceneevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicstransform.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtGui\QVector3D \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgraphicsview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qgroupbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qheaderview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qinputdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlineedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qitemeditorfactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeyeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qkeysequenceedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlabel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlcdnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qlistwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmainwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdiarea.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmdisubwindow.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenu.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmenubar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmessagebox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qmouseeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qopenglwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QWidget \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qplaintextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextedit.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qprogressdialog.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qproxystyle.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QCommonStyle \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qradiobutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscroller.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QPointF \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\QScrollerProperties \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qscrollerproperties.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMetaType \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QVariant \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qshortcut.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsizegrip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qspinbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplashscreen.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsplitter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedlayout.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstackedwidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstatusbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleditemdelegate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylefactory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstylepainter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qstyleplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qsystemtrayicon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtableview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtablewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtextbrowser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbox.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtoolbutton.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtooltip.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreeview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidget.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtreewidgetitemiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundogroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundostack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qundoview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwhatsthis.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwidgetaction.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qwizard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtWidgets\qtwidgetsversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QString \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMap

release\CComm.obj: CComm.cpp CComm.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFuture \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuture.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig-bootstrapped.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconfig.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcore-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocessordetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcompilerdetection.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypeinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsysinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlogging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qflags.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasicatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_bootstrap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qgenericatomic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_cxx11.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qatomic_msvc.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qglobalstatic.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmutex.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnumeric.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversiontagging.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfutureinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrunnable.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qshareddata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qchar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrefcount.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhashfunctions.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstring.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qnamespace.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringliteral.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringalgorithms.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringview.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringbuilder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpair.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainertools_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpoint.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraylist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregexp.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringmatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresultstore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdebug.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qiodevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectdefs_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreevent.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetatype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvarlengtharray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontainerfwd.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobject_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlocale.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariant.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qset.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcontiguouscache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedpointer_impl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QFutureWatcher \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturewatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbasictimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QObject \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QMutex \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrent \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\QtConcurrentDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCore \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QtCoreDepends \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracteventdispatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventloop.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractitemmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractnativeeventfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstractstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qabstracttransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydataops.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qarraydatapointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbitarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbuffer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qbytearraymatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcache.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcalendar.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatetime.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborcommon.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qregularexpression.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurl.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qurlquery.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\quuid.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcbormap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamreader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfloat16.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcborstreamwriter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcollator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineoption.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcommandlineparser.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcoreapplication.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qconcatenatetablesproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qcryptographichash.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdatastream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdeadlinetimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qelapsedtimer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfiledevice.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qdiriterator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeasingcurve.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qendian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qeventtransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfactoryinterface.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfileselector.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QStringList \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfilesystemwatcher.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfinalstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qfuturesynchronizer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qhistorystate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qidentityproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qisenum.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qitemselectionmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonarray.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonvalue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsondocument.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qjsonobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibrary.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlibraryinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qversionnumber.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlinkedlist.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qlockfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qloggingcategory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmargins.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmath.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmessageauthenticationcode.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmetaobject.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedata.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimedatabase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qmimetype.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qobjectcleanuphandler.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qoperatingsystemversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qparallelanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpauseanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qplugin.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpointer.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpluginloader.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qprocess.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qpropertyanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qvariantanimation.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qqueue.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrandom.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qreadwritelock.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qrect.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsize.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qresource.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsavefile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopedvaluerollback.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qscopeguard.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsequentialanimationgroup.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsettings.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsharedmemory.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignalmapper.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsignaltransition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsocketnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsortfilterproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstack.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstandardpaths.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstate.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstatemachine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstorageinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qstringlistmodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qsystemsemaphore.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporarydir.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtemporaryfile.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextboundaryfinder.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtextcodec.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthread.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadpool.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qthreadstorage.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimeline.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtimezone.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtranslator.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtransposeproxymodel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtypetraits.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwaitcondition.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QDeadlineTimer \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qwineventnotifier.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qxmlstream.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\qtcoreversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentcompilertest.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrent_global.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentexception.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilter.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfilterkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentiteratekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmedian.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentthreadengine.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmapkernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentreducekernel.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentfunctionwrappers.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentmap.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrun.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentrunbase.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentstoredfunctioncall.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtConcurrent\qtconcurrentversion.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QByteArray \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QHostAddress \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qhostaddress.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetworkglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtnetwork-config.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qabstractsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QTcpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qtcpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\QUdpSocket \
		D:\Qt\5.15.0\msvc2019_64\include\QtNetwork\qudpsocket.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPort \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialport.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportglobal.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\QSerialPortInfo \
		D:\Qt\5.15.0\msvc2019_64\include\QtSerialPort\qserialportinfo.h \
		D:\Qt\5.15.0\msvc2019_64\include\QtCore\QList

release\CMainWin.obj: CMainWin.cpp 

release\main.obj: main.cpp 

release\moc_CCloudPoints.obj: release\moc_CCloudPoints.cpp 

release\moc_CLumoMap.obj: release\moc_CLumoMap.cpp 

release\moc_CComm.obj: release\moc_CComm.cpp 

release\moc_CMainWin.obj: release\moc_CMainWin.cpp 

release\moc_CCopyTableWidget.obj: release\moc_CCopyTableWidget.cpp 

####### Install

install:  FORCE

uninstall:  FORCE

FORCE:


#ifndef JINOID_STD_PROTOCOL_H
#define JINOID_STD_PROTOCOL_H

#include "crc16.h"

enum eCmd {
    typeHCS = 0xA1,
    getVersion,
    stop,
    start,
    sw1,
    sw2,
    sw3,
    sw4,
    typePA2 = 0xAA,
    getParam,
    setParam,
    getBulk,
    setBulk
};

#ifndef CRC16_H_

}
#else
static quint16 getCrc16(const char* data, uint16_t dataSize) {
    return MakeCRC16(&data[4], dataSize - 4);
}
#endif

static int pack(enum eCmd command, uint16_t dType, uint16_t sAddr, uint16_t wCnt,
    const char* data, int dataSize, const char* result, int resultSize,
    char* packet, int* packetSize) {
    int offset = 0;

    if (command < typePA2) {
        packet[offset++] = 0xA1;
    }
    else {
        packet[offset++] = 0xAA;
        packet[offset++] = 0x00;
    }

    switch (command) {
    case stop:
        packet[offset++] = 'Q';
        break;
    case start:
        packet[offset++] = 'S';
        break;
    case getVersion:
        packet[offset++] = 'V';
        break;
    case sw1:
        packet[offset++] = 'A';
        break;
    case sw2:
        packet[offset++] = 'B';
        break;
    case sw3:
        packet[offset++] = 'C';
        break;
    case sw4:
        packet[offset++] = 'D';
        break;
    case getParam:
        packet[offset++] = 'M';
        packet[offset++] = 'B';
        packet[offset++] = (7 >> 8) & 0xFF;
        packet[offset++] = 7 & 0xFF;
        packet[offset++] = (dType >> 8) & 0xFF;
        packet[offset++] = dType & 0xFF;
        packet[offset++] = (sAddr >> 8) & 0xFF;
        packet[offset++] = sAddr & 0xFF;
        packet[offset++] = (wCnt >> 8) & 0xFF;
        packet[offset++] = wCnt & 0xFF;
        // Add CRC16
        uint16_t crc = getCrc16(packet);
        packet[offset++] = (crc >> 8) & 0xFF;
        packet[offset++] = crc & 0xFF;
        break;
    case setParam:
        packet[offset++] = 'A';
        packet[offset++] = 'E';
        packet[offset++] = ((7 + dataSize) >> 8) & 0xFF;
        packet[offset++] = (7 + dataSize) & 0xFF;
        packet[offset++] = (dType >> 8) & 0xFF;
        packet[offset++] = dType & 0xFF;
        packet[offset++] = (sAddr >> 8) & 0xFF;
        packet[offset++] = sAddr & 0xFF;
        packet[offset++] = ((dataSize / 2) >> 8) & 0xFF;
        packet[offset++] = (dataSize / 2) & 0xFF;
        memcpy(packet + offset, data, dataSize);
        offset += dataSize;
        // Add CRC16
        crc = getCrc16(packet);
        packet[offset++] = (crc >> 8) & 0xFF;
        packet[offset++] = crc & 0xFF;
        break;
    case getBulk:
        packet[offset++] = 'G';
        packet[offset++] = 'A';
        packet[offset++] = (6 >> 8) & 0xFF;
        packet[offset++] = 6 & 0xFF;
        packet[offset++] = (dType >> 8) & 0xFF;
        packet[offset++] = dType & 0xFF;
        packet[offset++] = (sAddr >> 8) & 0xFF;
        packet[offset++] = sAddr & 0xFF;
        packet[offset++] = (wCnt >> 8) & 0xFF;
        packet[offset++] = wCnt & 0xFF;
        break;
    case setBulk:
        packet[offset++] = 'G';
        packet[offset++] = 'B';
        packet[offset++] = ((5 + dataSize + resultSize) >> 8) & 0xFF;
        packet[offset++] = (5 + dataSize + resultSize) & 0xFF;
        packet[offset++] = (sAddr >> 8) & 0xFF;
        packet[offset++] = sAddr & 0xFF;
        packet[offset++] = (wCnt >> 8) & 0xFF;
        packet[offset++] = wCnt & 0xFF;
        memcpy(packet + offset, data, dataSize);
        offset += dataSize;
        if (result && resultSize > 0) {
            memcpy(packet + offset, result, resultSize);
            offset += resultSize;
        }
        break;
    default:
        *packetSize = 0;
        return 0; // Error: unknown command
    }

    packet[offset++] = '\r';  // Add 'CR' at the end
    *packetSize = offset;
    return offset;
}

static int unpack(const char* data, int dataSize, enum eCmd* command,
    uint16_t* dataType, uint16_t* startAddr, uint16_t* reqWordCnt,
    char* targetData, int* targetDataSize,
    char* resultData, int* resultDataSize) {
    if (dataSize < 3) {
        return 0; // 패킷이 너무 작음
    }

    // 헤더를 확인하여 명령 유형 파악
    if (data[0] == (char)0xA1) {
        *command = typeHCS;
    }
    else if (data[0] == (char)0xAA) {
        *command = typePA2;
    }
    else {
        return 0; // 잘못된 헤더
    }

    // 헤더와 데이터 크기에 따라 명령 추출
    if (*command == typeHCS) {
        switch (data[1]) {
        case 'V':
            *command = getVersion;
            break;
        case 'Q':
            *command = stop;
            break;
        case 'S':
            *command = start;
            break;
        case 'A':
            *command = sw1;
            break;
        case 'B':
            *command = sw2;
            break;
        case 'C':
            *command = sw3;
            break;
        case 'D':
            *command = sw4;
            break;
        default:
            return 0; // HCS에 대한 잘못된 명령
        }
    }
    else { // PA2 명령
        if (dataSize < 12) {
            return 0; // PA2 명령 데이터에 대해 패킷이 너무 작음
        }
        uint16_t readSize = (data[4] << 8) | data[5];
        if (dataSize < readSize + 6) {
            return 0; // 패킷이 선언된 크기보다 작음
        }
        *dataType = (data[6] << 8) | data[7];
        *startAddr = (data[8] << 8) | data[9];
        *reqWordCnt = (data[10] << 8) | data[11];
        switch (data[2]) {
        case 'M': // GetParam
            *command = getParam;
            break;
        case 'A': // SetParam
            *command = setParam;
            if (targetData && targetDataSize) {
                int size = *reqWordCnt * 2; // 2 bytes per word
                if (size > *targetDataSize) {
                    size = *targetDataSize; // 버퍼 오버플로우 방지
                }
                memcpy(targetData, data + 12, size);
                *targetDataSize = size;
            }
            break;
        case 'G': // GetBulk or SetBulk
            switch (data[3]) {
            case 'A':
                *command = getBulk;
                break;
            case 'B':
                *command = setBulk;
                *startAddr = (data[6] << 8) | data[7];
                *reqWordCnt = (data[8] << 8) | data[9];
                if (targetData && targetDataSize) {
                    int size = *reqWordCnt * 2; // 2 bytes per word
                    if (size > *targetDataSize) {
                        size = *targetDataSize; // 버퍼 오버플로우 방지
                    }
                    memcpy(targetData, data + 10, size);
                    *targetDataSize = size;
                }
                if (resultData && resultDataSize) {
                    int size = 20; // 고정 크기 20 바이트
                    if (size > *resultDataSize) {
                        size = *resultDataSize; // 버퍼 오버플로우 방지
                    }
                    memcpy(resultData, data + 10 + (*reqWordCnt * 2), size);
                    *resultDataSize = size;
                }
                break;
            default:
                return 0; // 잘못된 GetBulk/SetBulk 하위 명령
            }
            break;
        default:
            return 0; // PA2에 대한 잘못된 명령
        }
    }

    return (int)*command;
}

#endif // JINOID_STD_PROTOCOL_H

@echo off
REM This script sets up the Visual Studio environment for command-line builds.
REM Please verify the path to vcvarsall.bat matches your installation.

set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VS_PATH%" (
    set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvarsall.bat"
)
if not exist "%VS_PATH%" (
    set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
)

if exist "%VS_PATH%" (
    call "%VS_PATH%" x64
) else (
    echo Error: Could not find vcvarsall.bat. Please edit setup_env.bat with the correct path.
    exit /b 1
)

# Environment Setup & Verification Log

- [x] Analyze project files to identify build requirements <!-- id: 0 -->
    - [x] Read `CLumoMap.pro` <!-- id: 1 -->
    - [x] Read `setup_env.bat` <!-- id: 2 -->
    - [x] Read `.vscode/tasks.json` and `.vscode/launch.json` <!-- id: 3 -->
- [x] List required environment and tools for the user <!-- id: 4 -->
- [x] Verify Local Environment <!-- id: 5 -->
    - [x] Check existence of Qt installation path (`D:\Qt\5.15.0\msvc2019_64`) <!-- id: 6 -->
    - [x] Verify Visual Studio environment setup (`setup_env.bat`) <!-- id: 7 -->
- [x] Build Project <!-- id: 8 -->
    - [x] Run `qmake` task <!-- id: 9 -->
    - [x] Run `build` task <!-- id: 10 -->
- [x] Debug Project <!-- id: 11 -->
    - [x] Configure `launch.json` for CodeLLDB <!-- id: 12 -->
    - [x] Change build output to `antiGravity/debug` <!-- id: 13 -->
    - [x] Configure Release build task <!-- id: 14 -->

> **Status**: Environment setup complete. Build and Debug configurations verified.

QMAKE_CXX.QT_COMPILER_STDCXX = 199711L
QMAKE_CXX.QMAKE_MSC_VER = 1940
QMAKE_CXX.QMAKE_MSC_FULL_VER = 194033811
QMAKE_CXX.COMPILER_MACROS = \
    QT_COMPILER_STDCXX \
    QMAKE_MSC_VER \
    QMAKE_MSC_FULL_VER
QMAKE_CXX.INCDIRS = \
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.40.33807\\include" \
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.40.33807\\ATLMFC\\include" \
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\VS\\include" \
    "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.26100.0\\ucrt" \
    "C:\\Program Files (x86)\\Windows Kits\\10\\\\include\\10.0.26100.0\\\\um" \
    "C:\\Program Files (x86)\\Windows Kits\\10\\\\include\\10.0.26100.0\\\\shared" \
    "C:\\Program Files (x86)\\Windows Kits\\10\\\\include\\10.0.26100.0\\\\winrt" \
    "C:\\Program Files (x86)\\Windows Kits\\10\\\\include\\10.0.26100.0\\\\cppwinrt" \
    "C:\\Program Files (x86)\\Windows Kits\\NETFXSDK\\4.8\\include\\um"
QMAKE_CXX.LIBDIRS = \
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.40.33807\\ATLMFC\\lib\\x64" \
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.40.33807\\lib\\x64" \
    "C:\\Program Files (x86)\\Windows Kits\\NETFXSDK\\4.8\\lib\\um\\x64" \
    "C:\\Program Files (x86)\\Windows Kits\\10\\lib\\10.0.26100.0\\ucrt\\x64" \
    "C:\\Program Files (x86)\\Windows Kits\\10\\\\lib\\10.0.26100.0\\\\um\\x64"

@echo off
call setup_env.bat
if %errorlevel% neq 0 exit /b %errorlevel%
nmake %*

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

#ifndef CCLOUDPOINTS_H
#define CCLOUDPOINTS_H


#include <QObject>
#include <QTimer>
#include <QVector>
#include <QPointF>
#include <QtMath>
#include <QDataStream> 

class ICloudPointGetter {
public:
    virtual ~ICloudPointGetter() {}
    virtual void clearPoints() = 0;
    virtual void setPoint(quint16 angle, quint16 distance, int layerNo) = 0;
};
Q_DECLARE_INTERFACE(ICloudPointGetter, "com.LumosLiDAR.ICloudPointGetter/1.0")

class CCloudPoints : public QObject, public ICloudPointGetter {
    Q_OBJECT
    Q_INTERFACES(ICloudPointGetter)

public:
    CCloudPoints(QObject* parent, int pixelsPerMeter = 100, int maxPoints = 2400 + 10)
        : QObject(parent), m_maxPoints(maxPoints), m_pixelsPerMeter(pixelsPerMeter)
    {
        // 기본값 초기화
        m_distanceRate = 0.1f;
        m_unitToMeter = 0.01f; // cm
        updateScale();

        m_angleOffset = 0.0f;
        m_isClockwise = true;
        m_virtualShapeType = 0;
    }

    QVector<QPointF> getPoints() const {
        return m_points;
    }

    float getUnitToPixelScale() const {
        return m_unitToMeter * m_pixelsPerMeter;
    }

    void setPoints(QVector<QPointF> points) {
        m_points.append(points);
        // (MaxPoints 로직 유지)
        int excess = m_points.size() - m_maxPoints;
        if (excess > 0) {
            for (int i = 0; i < excess; i++) {
                m_points.removeFirst();
            }
        }
    }

    void setOrientation(float angleOffsetDeg, bool isCW) {
        m_angleOffset = angleOffsetDeg;
        m_isClockwise = isCW;
    }

    // 거리 설정 함수
    void setDistanceSettings(float rate, float unitToMeter) {
        m_distanceRate = rate;
        m_unitToMeter = unitToMeter;
        updateScale();
    }

    // ICloudPointGetter 구현
    void clearPoints() override {
        m_points.clear();
    }

    void setPoint(quint16 angle, quint16 distance, int layerNo) override {
        Q_UNUSED(layerNo);
        while (angle > 36000) {
            angle -= 36000;
        }

        float fAngle = (float)angle / 100;

        // Raw * Rate * U2M * PPM
        float fPixelDist = (float)distance * m_finalScale;

        // 각도 보정
        if (m_isClockwise) fAngle = -fAngle;
        
        fAngle += m_angleOffset;

        float radian = fAngle * M_PI / 180.0;

        float x = fPixelDist * std::cos(radian);
        float y = -fPixelDist * std::sin(radian); // Y축 반전

        m_points.append(QPointF(x, y));
    }

    //int getPointCount() const {
    //    return m_points.size();
    //}

    /**
     * @brief 가상 데이터 생성 (Raw 데이터 생성)
     */
    QByteArray generateVirtualPayload(int channels, float resolution, int mesuresPerScan) {

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setByteOrder(QDataStream::BigEndian);

        quint16 _resolution = quint16(resolution * 100), angle = 0, distUnit = 3000;

        for (int i = 0; i < mesuresPerScan; ++i) {
            switch (m_virtualShapeType) {
            case 0: distUnit = 30000 + ((i % 50) * 100); break;
            case 1: distUnit = 25000; break;
            case 2: distUnit = i * 50; break;
            case 3: distUnit = 0; break;
            }

            out << angle;
            for (int j = 0; j < channels; j++) {
                out << distUnit;
                distUnit += (distUnit * 0.02);
            }
            angle += _resolution;
        }

        m_virtualShapeType = (m_virtualShapeType + 1) % 4;
        return payload;
    }

private:
    QVector<QPointF> m_points;
    QTimer timer;
    int m_maxPoints = 2400;

    float m_pixelsPerMeter = 100;
    float m_scale = m_pixelsPerMeter / 1000;

    float m_angleOffset;
    bool m_isClockwise;
    int m_virtualShapeType;

    // [신규]
    float m_distanceRate;
    float m_unitToMeter;
    float m_finalScale;

    void updateScale() {
        // Raw -> [Rate] -> Unit -> [U2M] -> Meter -> [PPM] -> Pixel
        m_finalScale = m_distanceRate * m_unitToMeter * m_pixelsPerMeter;
    }
};

#endif // CCLOUDPOINTS_H
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

#include "CComm.h"

// #include <QtCore/QCoreApplication>
// #include <QtCore/QMetaObject>
