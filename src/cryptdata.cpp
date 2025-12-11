/*
 * Copyright (C) 2014 Red Hat
 *
 * This file is part of avpn.
 *
 * avpn is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cryptdata.h"

#if defined(_WIN32)
#include <QDateTime>
#include <QSysInfo>
#include <windows.h>
#include <winbase.h>

typedef WINBOOL(WINAPI* CryptProtectDataFunc)(DATA_BLOB* pDataIn,
    LPCWSTR szDataDescr,
    DATA_BLOB* pOptionalEntropy,
    PVOID pvReserved,
    CRYPTPROTECT_PROMPTSTRUCT*
        pPromptStruct,
    DWORD dwFlags,
    DATA_BLOB* pDataOut);
typedef WINBOOL(WINAPI* CryptUnprotectDataFunc)(DATA_BLOB* pDataIn,
    LPWSTR* ppszDataDescr,
    DATA_BLOB* pOptionalEntropy,
    PVOID pvReserved,
    CRYPTPROTECT_PROMPTSTRUCT*
        pPromptStruct,
    DWORD dwFlags,
    DATA_BLOB* pDataOut);

static CryptProtectDataFunc pCryptProtectData;
static CryptUnprotectDataFunc pCryptUnprotectData;
static int lib_init = 0;

static void __attribute__((constructor)) init(void)
{
    static HMODULE lib;
    lib = LoadLibraryA("crypt32.dll");
    if (lib == NULL)
        return;

    pCryptProtectData = (CryptProtectDataFunc)GetProcAddress(lib, "CryptProtectData");
    pCryptUnprotectData = (CryptUnprotectDataFunc)GetProcAddress(lib, "CryptUnprotectData");
    if (pCryptProtectData == NULL || pCryptUnprotectData == NULL) {
        FreeLibrary(lib);
        return;
    }
    lib_init = 1;
}

QByteArray CryptData::encode(QString& txt, QString password)
{

    if (lib_init == 0) {
        return password.toUtf8();
    }

    DATA_BLOB DataIn;
    QByteArray passwordArray{ password.toUtf8() };
    DataIn.pbData = (BYTE*)passwordArray.data();
    DataIn.cbData = passwordArray.size() + 1;

    DATA_BLOB Entropy;
    QString enhancedEntropy = txt + QSysInfo::machineUniqueId() + QString::number(QDateTime::currentSecsSinceEpoch());
    QByteArray entropyArray{ enhancedEntropy.toUtf8() };
    Entropy.pbData = (BYTE*)entropyArray.data();
    Entropy.cbData = entropyArray.size() + 1;

    DATA_BLOB DataOut;
    QByteArray res;
    BOOL r = pCryptProtectData(&DataIn, NULL, &Entropy, NULL, NULL, 0, &DataOut);
    if (r == false) {
        return res;
    }

    QByteArray data;
    data.setRawData((const char*)DataOut.pbData, DataOut.cbData);

    res.clear();
    res.append("xxxx");
    res.append(data.toBase64());

    LocalFree(DataOut.pbData);
    return res;
}

bool CryptData::decode(QString& txt, QByteArray _enc, QString& res)
{
    res.clear();

    if (lib_init == 0 || _enc.startsWith("xxxx") == false) {
        res = QString::fromUtf8(_enc);
        return true;
    }

    DATA_BLOB DataIn;
    QByteArray enc{ QByteArray::fromBase64(_enc.mid(4)) };
    DataIn.pbData = (BYTE*)enc.data();
    DataIn.cbData = enc.size() + 1;

    DATA_BLOB Entropy;
    QString enhancedEntropy = txt + QSysInfo::machineUniqueId() + QString::number(QDateTime::currentSecsSinceEpoch());
    QByteArray entropyArray{ enhancedEntropy.toUtf8() };
    Entropy.pbData = (BYTE*)entropyArray.data();
    Entropy.cbData = entropyArray.size() + 1;

    DATA_BLOB DataOut;

    BOOL r = pCryptUnprotectData(&DataIn, NULL, &Entropy, NULL, NULL, 0, &DataOut);
    if (r == false) {
        return false;
    }

    res = QString::fromUtf8((const char*)DataOut.pbData, DataOut.cbData - 1);
    LocalFree(DataOut.pbData);
    return true;
}

#else

#include <QCryptographicHash>
#include <QRandomGenerator>

static QByteArray generateKey(const QString& txt, const QByteArray& salt)
{
    QByteArray key = txt.toUtf8() + salt;
    for (int i = 0; i < 10000; i++) {
        key = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    }
    return key;
}

static QByteArray xorEncryptDecrypt(const QByteArray& data, const QByteArray& key)
{
    QByteArray result = data;
    for (int i = 0; i < result.size(); i++) {
        result[i] = result[i] ^ key[i % key.size()];
    }
    return result;
}

QByteArray CryptData::encode(QString& txt, QString password)
{
    QByteArray salt = QByteArray::number(QRandomGenerator::global()->generate64(), 16);
    QByteArray key = generateKey(txt, salt);
    
    QByteArray passwordData = password.toUtf8();
    QByteArray encrypted = xorEncryptDecrypt(passwordData, key);
    
    QByteArray result;
    result.append("yyyy");
    result.append(salt.toBase64());
    result.append(":");
    result.append(encrypted.toBase64());
    
    return result;
}

bool CryptData::decode(QString& txt, QByteArray _enc, QString& res)
{
    res.clear();

    if (_enc.startsWith("yyyy")) {
        _enc = _enc.mid(4);
        int separatorPos = _enc.indexOf(':');
        if (separatorPos == -1) {
            return false;
        }
        
        QByteArray salt = QByteArray::fromBase64(_enc.left(separatorPos));
        QByteArray encrypted = QByteArray::fromBase64(_enc.mid(separatorPos + 1));
        
        QByteArray key = generateKey(txt, salt);
        QByteArray decrypted = xorEncryptDecrypt(encrypted, key);
        
        res = QString::fromUtf8(decrypted);
        return true;
    } else {
        res = QString::fromUtf8(_enc);
        return true;
    }
}

#endif