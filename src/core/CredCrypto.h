#pragma once

#include <QByteArray>
#include <QString>

namespace svnsync {

/** Symmetric credential encryption (AES-256-GCM via OpenSSL EVP).
 *
 *  The master key is a random 32-byte blob owned by the caller (ConfigStore
 *  keeps it in the credentials table). Encrypted output is a base64 encoding
 *  of IV(12) | tag(16) | ciphertext so a corrupted or wrong-keyed blob is
 *  detected on decrypt and reported as an empty string. */
class CredCrypto
{
public:
    static QByteArray generateKey();
    static QByteArray encrypt(const QByteArray &key, const QString &plaintext);
    static QString decrypt(const QByteArray &key, const QByteArray &blob);
};

} // namespace svnsync
