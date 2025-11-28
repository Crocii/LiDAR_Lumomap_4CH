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
