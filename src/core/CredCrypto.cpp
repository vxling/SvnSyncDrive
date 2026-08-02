#include "core/CredCrypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <QByteArray>

namespace svnsync {

namespace {
constexpr int kKeyBytes = 32;
constexpr int kIvBytes = 12;
constexpr int kTagBytes = 16;
}

QByteArray CredCrypto::generateKey()
{
    QByteArray key(kKeyBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(key.data()), key.size()) != 1)
        return QByteArray();
    return key;
}

QByteArray CredCrypto::encrypt(const QByteArray &key, const QString &plaintext)
{
    if (key.size() != kKeyBytes || plaintext.isEmpty())
        return QByteArray();

    const QByteArray input = plaintext.toUtf8();
    QByteArray iv(kIvBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(iv.data()), iv.size()) != 1)
        return QByteArray();

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return QByteArray();

    QByteArray out(input.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int len = 0;
    int total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvBytes, nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(iv.constData())) == 1
        && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                             reinterpret_cast<const unsigned char *>(input.constData()),
                             input.size()) == 1;
    if (ok) {
        total = len;
        ok = EVP_EncryptFinal_ex(ctx,
                                 reinterpret_cast<unsigned char *>(out.data() + total),
                                 &len) == 1;
        total += len;
    }
    QByteArray tag(kTagBytes, Qt::Uninitialized);
    if (ok)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes,
                                 tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return QByteArray();

    out.resize(total);
    return (iv + tag + out).toBase64();
}

QString CredCrypto::decrypt(const QByteArray &key, const QByteArray &blob)
{
    if (key.size() != kKeyBytes)
        return QString();
    const QByteArray raw = QByteArray::fromBase64(blob);
    if (raw.size() < kIvBytes + kTagBytes)
        return QString();

    const QByteArray iv = raw.left(kIvBytes);
    const QByteArray tag = raw.mid(kIvBytes, kTagBytes);
    const QByteArray cipher = raw.mid(kIvBytes + kTagBytes);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return QString();

    QByteArray out(cipher.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int len = 0;
    int total = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvBytes, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(iv.constData())) == 1
        && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                             reinterpret_cast<const unsigned char *>(cipher.constData()),
                             cipher.size()) == 1;
    if (ok) {
        total = len;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes,
                                 const_cast<char *>(tag.constData())) == 1
            && EVP_DecryptFinal_ex(ctx,
                                   reinterpret_cast<unsigned char *>(out.data() + total),
                                   &len) == 1;
        total += len;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return QString();

    out.resize(total);
    return QString::fromUtf8(out);
}

} // namespace svnsync
