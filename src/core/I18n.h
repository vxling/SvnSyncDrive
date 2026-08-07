#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

/**
 * Lightweight built-in translations (Chinese source text + English table).
 *
 * Every user-facing string is written in Chinese and passed through
 * I18n::translate(); when the current language is English and a translation exists
 * the English text is returned, otherwise the Chinese source is kept. This
 * avoids a Qt .ts/.qm toolchain while still giving instant runtime language
 * switching.
 *
 * Language codes: "zh_CN" (default) and "en".
 */
class I18n : public QObject
{
    Q_OBJECT

public:
    static I18n *instance();

    /** Translated text for the current language; sourceText for Chinese.
     *  Named translate() to avoid clashing with QObject's tr(). */
    static QString translate(const char *sourceText);

    static QString language();
    static void setLanguage(const QString &code);

    static QStringList supportedLanguages();

signals:
    /** Emitted after a switch so persistent widgets can re-translate. */
    void languageChanged(const QString &language);

private:
    I18n();

    QString m_language = QStringLiteral("zh_CN");
};
