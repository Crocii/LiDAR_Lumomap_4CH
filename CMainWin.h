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