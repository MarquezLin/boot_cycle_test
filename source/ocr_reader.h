#pragma once
#include <QImage>
#include <QStringList>
QString detectOcrExecutable(const QString &configured = {});
QString defaultOcrLanguage(const QString &executable);
QStringList ocrArguments(const QString &executable, const QString &image, const QString &language);
QImage prepareOcrImage(const QImage &image);
