#include "ocr_reader.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QPainter>
static QString ocrDataDirectory(const QString &exe)
{
    const auto bundled = QCoreApplication::applicationDirPath() + "/ocr/tessdata";
    return QFileInfo(bundled + "/eng.traineddata").isFile() ? bundled : QFileInfo(exe).absolutePath() + "/tessdata";
}
QString detectOcrExecutable(const QString &configured)
{
    if (QFileInfo(configured).isFile()) return configured;
    const auto installed = QStandardPaths::findExecutable("tesseract");
    for (const auto &path : QStringList{QCoreApplication::applicationDirPath() + "/ocr/tesseract.exe", installed,
         "C:/Program Files/Tesseract-OCR/tesseract.exe", qEnvironmentVariable("LOCALAPPDATA") + "/Programs/Tesseract-OCR/tesseract.exe"})
        if (QFileInfo(path).isFile()) return path;
    return {};
}
QString defaultOcrLanguage(const QString &exe)
{
    return QFileInfo(ocrDataDirectory(exe) + "/chi_tra.traineddata").isFile() ? "chi_tra+eng" : "eng";
}
QStringList ocrArguments(const QString &exe, const QString &image, const QString &language)
{
    QStringList args{image, "stdout", "-l", language.isEmpty() ? defaultOcrLanguage(exe) : language, "--psm", "6"};
    const auto data = ocrDataDirectory(exe);
    if (QFileInfo(data).isDir()) args << "--tessdata-dir" << data;
    return args;
}
QImage prepareOcrImage(const QImage &image)
{
    if (image.isNull()) return {};
    auto gray = image.convertToFormat(QImage::Format_Grayscale8); double sum = 0;
    for (int y = 0; y < gray.height(); ++y) for (int x = 0; x < gray.width(); ++x) sum += gray.constScanLine(y)[x];
    if (sum / (double(gray.width()) * gray.height()) < 128) gray.invertPixels();
    const int scale = gray.height() < 32 ? 3 : gray.height() < 60 ? 2 : 1;
    gray = gray.scaled(gray.width() * scale, gray.height() * scale, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage output(gray.size() + QSize(24, 24), QImage::Format_RGB32); output.fill(Qt::white);
    QPainter painter(&output); painter.drawImage(12, 12, gray); return output;
}
