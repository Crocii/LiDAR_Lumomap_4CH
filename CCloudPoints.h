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