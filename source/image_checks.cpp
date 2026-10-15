#include "image_checks.h"
#include <cmath>

double imageContrast(const QImage &image)
{
    if (image.isNull()) return -1;
    const auto size = image.size().scaled(256, 144, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    const auto gray = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    double sum = 0, square = 0;
    for (int y = 0; y < gray.height(); ++y) {
        const auto *line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {const double value = line[x]; sum += value; square += value * value;}
    }
    const double count = double(gray.width()) * gray.height(), mean = sum / count;
    return std::sqrt(qMax(0.0, square / count - mean * mean));
}
double imageDifference(const QImage &first, const QImage &second)
{
    if (first.isNull() || second.isNull() || first.size() != second.size()) return -1;
    const auto size = first.size().scaled(256, 144, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    const auto a = first.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    const auto b = second.scaled(a.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *left = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *right = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            sum += std::abs(qRed(left[x]) - qRed(right[x])) + std::abs(qGreen(left[x]) - qGreen(right[x])) + std::abs(qBlue(left[x]) - qBlue(right[x]));
    }
    return sum / (double(a.width()) * a.height() * 3);
}
bool imageChecksSelfTest()
{
    QImage black(128, 72, QImage::Format_RGB32); black.fill(Qt::black);
    QImage white = black; white.fill(Qt::white);
    QImage bars = black;
    for (int y = 0; y < bars.height(); ++y) for (int x = 64; x < bars.width(); ++x) bars.setPixel(x, y, qRgb(255, 255, 255));
    return imageContrast(black) == 0 && imageContrast(white) == 0 && imageContrast(bars) > 100
        && imageDifference(bars, bars) == 0 && imageDifference(black, white) > 250;
}
