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
