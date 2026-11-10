#pragma once
#include "flow_model.h"
#include <QImage>
#include <atomic>
#include <functional>
using CaptureImage = std::function<QImage(QString &)>;
using CaptureNotice = std::function<void(const QString &)>;
bool offlineImageStep(const FlowStep &step,const QString &prefix,const QString &replay,
    const CaptureImage &capture,const CaptureNotice &notice,const std::atomic_bool &stop,QString &detail,QString &error);
bool offlineCaptureSelfTest();
bool offlineVideoReplaySelfTest(const QString &path);
