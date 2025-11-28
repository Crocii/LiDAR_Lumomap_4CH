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

            // 타입별 색상 적용 (설정된 색상 사용)
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
    bool    m_isHighlightActive; // (이제 개별 Data.active 사용)

    bool  m_fadeEnabled;
    float m_angleOffset;
    bool  m_isClockwise;
    QString m_distanceUnit;
};