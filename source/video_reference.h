#pragma once
#include <QImage>
#include <QVector>
#include <QString>
#include <atomic>
struct ReferenceFrame {qint64 ms; QImage image; QImage fingerprint;};
struct VideoMatch {int index=-1; double structure=255, rgb=255, swapped=255, badFraction=1; QString verdict; bool pass=false;};
bool loadReferenceVideo(const QString &path,QVector<ReferenceFrame> &frames,QString &error,const std::atomic_bool &stop);
struct VideoAlignment {qint64 offsetMs=0,durationMs=0;double score=255,motion=0;bool valid=false;};
qint64 referenceDurationMs(const QVector<ReferenceFrame> &frames);
VideoAlignment alignReferenceSequence(const QVector<QImage> &images,const QVector<qint64> &times,const QVector<ReferenceFrame> &frames,const std::atomic_bool &stop);
VideoMatch matchReferenceVideo(const QImage &actual,const QVector<ReferenceFrame> &frames,qint64 expectedMs=-1,qint64 durationMs=0);
bool saveVideoEvidence(const QString &prefix,const QImage &actual,const QVector<ReferenceFrame> &frames,const VideoMatch &match,QString &error);
bool videoReferenceSelfTest();
