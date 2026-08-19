// CStringRange 最小实现（仅测试用，定义在 SampleIMEBaseStructure.cpp 中的方法）
#include "SampleIMEBaseStructure.h"
#include <cwchar>
#include <string>

CStringRange::CStringRange() : _stringBufLen(0), _pStringBuf(nullptr) {}
CStringRange::~CStringRange() {}

const WCHAR* CStringRange::Get() const { return _pStringBuf; }
const DWORD_PTR CStringRange::GetLength() const { return _stringBufLen; }

void CStringRange::Clear() { _stringBufLen = 0; _pStringBuf = nullptr; }

void CStringRange::Set(const WCHAR* pwch, DWORD_PTR dwLength)
{
    _pStringBuf = pwch;
    _stringBufLen = dwLength;
}

void CStringRange::Set(CStringRange& sr)
{
    _pStringBuf = sr._pStringBuf;
    _stringBufLen = sr._stringBufLen;
}

CStringRange& CStringRange::operator=(const CStringRange& sr)
{
    _pStringBuf = sr._pStringBuf;
    _stringBufLen = sr._stringBufLen;
    return *this;
}

void CStringRange::CharNext(_Inout_ CStringRange* pCharNext)
{
    if (pCharNext && _stringBufLen)
    {
        pCharNext->_pStringBuf = _pStringBuf + 1;
        pCharNext->_stringBufLen = _stringBufLen - 1;
    }
}
