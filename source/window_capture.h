#pragma once
#include <QImage>
#include <memory>
#include <atomic>
class WindowCapture {
public:
    WindowCapture();
    ~WindowCapture();
    QImage next(quintptr window,QString &error,const std::atomic_bool *stop=nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
    bool initialized=false;
};
