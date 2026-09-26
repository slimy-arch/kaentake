#pragma once
#include "ztl/ztl.h"


class CInPacket {
protected:
    int m_bLoopback;
    int m_nState;
    ZArray<unsigned char> m_aRecvBuff;
    unsigned short m_uLength;
    unsigned short m_uRawSeq;
    unsigned short m_uDataLen;
    size_t m_uOffset;

public:
    // Inline readers over the members. Bounds follow CInPacket::Decode1 (0x004065F3), which checks
    // m_uLength - m_uOffset, not the receive buffer's capacity.
    size_t GetOffset() const {
        return m_uOffset;
    }
    void SetOffset(size_t uOffset) {
        m_uOffset = uOffset;
    }
    bool CanRead(size_t n) const {
        return m_aRecvBuff.GetCount() != 0 && m_uOffset + n <= m_uLength;
    }
    const unsigned char* Current() const {
        return &m_aRecvBuff[m_uOffset];
    }
    // Alias of Current() for code ported from the Coloring Prism bundle (weapontint.cpp).
    // UNCHECKED: call CanRead first.
    const unsigned char* CurrentPublic() const {
        return Current();
    }
    unsigned short Peek2() const {
        return CanRead(2) ? *reinterpret_cast<const unsigned short*>(Current()) : 0;
    }
    template <typename T>
    T Decode() {
        if (!CanRead(sizeof(T))) {
            return T{};
        }
        T value = *reinterpret_cast<const T*>(Current());
        m_uOffset += sizeof(T);
        return value;
    }
};

static_assert(sizeof(CInPacket) == 0x18);


class COutPacket {
protected:
    int m_bLoopback;
    ZArray<unsigned char> m_aSendBuff;
    unsigned int m_uOffset;
    int m_bIsEncryptedByShanda;

public:
    explicit COutPacket(int nType) : m_aSendBuff(0x100) {
        Init(nType, 0, 0);
    }
    void Encode1(unsigned char n) {
        EncodeBuffer(&n, 1);
    }
    void Encode2(unsigned short n) {
        EncodeBuffer(&n, 2);
    }
    void Encode4(unsigned int n) {
        EncodeBuffer(&n, 4);
    }
    void EncodeStr(ZXString<char> s) {
        int n = s.GetLength();
        Encode2(n);
        EncodeBuffer(s, n);
    }
    void EncodeBuffer(const void* p, size_t uSize) {
        EnlargeBuffer(uSize);
        memcpy(&m_aSendBuff[m_uOffset], p, uSize);
        m_uOffset += uSize;
    }
    void Init(int nType, int bLoopback, int bTypeHeader1Byte) {
        m_bLoopback = bLoopback;
        m_uOffset = 0;
        if (nType != 0x7FFFFFFF) {
            if (bTypeHeader1Byte) {
                Encode1(nType);
            } else {
                Encode2(nType);
            }
        }
        m_bIsEncryptedByShanda = 0;
    }

protected:
    void EnlargeBuffer(size_t uSize) {
        size_t uCur = m_aSendBuff.GetCount();
        size_t uReq = m_uOffset + uSize;
        if (uCur < uReq) {
            do {
                uCur *= 2;
            } while (uCur < uReq);
            m_aSendBuff.Realloc(uCur, 0);
        }
    }
};

static_assert(sizeof(COutPacket) == 0x10);