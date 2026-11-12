#include "offline_capture.h"
#include "video_reference.h"
#include "image_checks.h"
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QSaveFile>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include <cmath>
namespace {
constexpr qint64 budget=256LL*1024*1024;
struct Sample {qint64 ms;QImage image;};
struct Clip {QVector<Sample> samples;int fps=30,duration=0;QSize source;};
// The clip owns the underlying pixels for the entire analysis.
QImage videoComparisonView(const QImage &image, int ignoredPercent)
{
    if (ignoredPercent == 0) return image;
    const int height = qMax(1, image.height() * (100 - ignoredPercent) / 100);
    return QImage(image.constBits(), image.width(), height, image.bytesPerLine(), image.format());
}
bool writeClip(const QString &path,const Clip &clip,const std::atomic_bool &stop,QString &error){
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)){error=file.errorString();return false;}
    QDataStream out(&file);out.setVersion(QDataStream::Qt_6_5);
    out<<quint32(0x47564331)<<quint32(1)<<quint32(clip.samples.size())<<qint32(clip.fps)<<qint32(clip.duration)<<clip.source;
    for(const auto &sample:clip.samples){if(stop){error="已停止保存擷取資料";return false;}
        out<<sample.ms<<sample.image.size();
        const int bytes=int(sample.image.sizeInBytes());out.writeRawData(reinterpret_cast<const char *>(sample.image.constBits()),bytes);
    }
    if(out.status()!=QDataStream::Ok || !file.commit()){error="無法保存擷取資料："+file.errorString();return false;}return true;
}
bool readClip(const QString &path,Clip &clip,const std::atomic_bool &stop,QString &error){
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    QDataStream in(&file);in.setVersion(QDataStream::Qt_6_5);quint32 magic=0,version=0,count=0;qint32 fps=0,duration=0;
    in>>magic>>version>>count>>fps>>duration>>clip.source;
    if(magic!=0x47564331 || version!=1 || count<1 || count>72000 || fps<1 || fps>120 || duration<100 || duration>600000){error="不是有效的擷取資料檔";return false;}
    clip.fps=fps;clip.duration=duration;qint64 used=0,previous=-1;
    for(quint32 i=0;i<count;++i){if(stop){error="已停止讀取擷取資料";return false;}qint64 ms;QSize size;in>>ms>>size;
        const qint64 bytes=qint64(size.width())*size.height()*4;
        if(in.status()!=QDataStream::Ok || ms<0 || ms<previous || ms>601500 || size.width()<1 || size.height()<1 || size.width()>320 || size.height()>180 || bytes<1 || used+bytes>budget || file.bytesAvailable()<bytes){error="擷取資料內容不完整或超出限制";return false;}
        QImage image(size,QImage::Format_RGB32);if(image.isNull() || in.readRawData(reinterpret_cast<char *>(image.bits()),int(bytes))!=bytes){error="無法讀取擷取影格";return false;}
        clip.samples.append({ms,image});used+=bytes;previous=ms;
    }
    return in.status()==QDataStream::Ok;
}
}
bool offlineImageStep(const FlowStep &s,const QString &prefix,const QString &replay,
    const CaptureImage &capture,const CaptureNotice &notice,const std::atomic_bool &stop,QString &detail,QString &error){
    Clip clip;clip.fps=s.sampleFps;clip.duration=s.captureDurationMs;QElapsedTimer clock;clock.start();
    if(replay.isEmpty()){
        notice(QString("擷取中：%1 秒／目標 %2 FPS；完成後才開始分析").arg(s.captureDurationMs/1000.0).arg(s.sampleFps));
        qint64 used=0,next=0,lastNotice=0;
        while(!stop && clock.elapsed()<s.captureDurationMs){
            if(clock.elapsed()<next){QThread::msleep(1);continue;}
            const auto full=capture(error);const qint64 time=clock.elapsed();
            if(full.isNull())return false;
            if(!full.rect().contains(s.region)){error="影像區域超出視窗，請重新框選";return false;}
            // This is only acquisition and a bounded comparison-size copy. No
            // reference search, colour checks, disk writes or per-frame UI events.
            auto image=full.copy(s.region).scaled(QSize(320,180),Qt::KeepAspectRatio,Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
            const qint64 bytes=image.sizeInBytes();if(used+bytes>budget){error="擷取暫存達到 256 MiB 上限；請縮短擷取秒數或降低 FPS";return false;}
            clip.source=s.region.size();clip.samples.append({time,image});used+=bytes;
            next=qMax(clip.samples.first().ms+qRound64(clip.samples.size()*1000.0/s.sampleFps),time+1);
            if(time-lastNotice>=1000){notice(QString("擷取中：%1 / %2 秒，已取得 %3 張").arg(time/1000.0,0,'f',1).arg(s.captureDurationMs/1000.0).arg(clip.samples.size()));lastNotice=time;}
        }
        if(stop){error="已停止擷取";return false;}
        if(clip.samples.size()<2){error="擷取影格不足";return false;}
        notice("擷取完成，正在保存影格與時間戳…");
        if(!writeClip(prefix+".gvfgcap",clip,stop,error))return false;
    }else{notice("正在讀取已擷取資料，不操作待測 APP…");if(!readClip(replay,clip,stop,error))return false;
        if(!writeClip(prefix+".gvfgcap",clip,stop,error))return false;}
    if(stop){error="已停止";return false;}
    const auto &samples=clip.samples;const qint64 span=samples.last().ms-samples.first().ms;
    const double actual=span>0?1000.0*(samples.size()-1)/span:0; qint64 maxGap=0;
    for(int i=1;i<samples.size();++i)maxGap=qMax(maxGap,samples[i].ms-samples[i-1].ms);
    const QString sampling=QString("擷取：目標 %1 FPS／實際 %2 FPS（%3 張），最大採樣間隔 %4 ms").arg(clip.fps).arg(actual,0,'f',1).arg(samples.size()).arg(maxGap);
    notice("擷取完成；"+sampling+"；開始離線分析");
    QVector<ReferenceFrame> reference;
    if(s.type=="checkVideo" && !loadReferenceVideo(s.referenceVideo,reference,error,stop))return false;
    QElapsedTimer analysis;analysis.start();
    notice("正在從整段擷取資料尋找起始位置與循環對齊…");
    VideoAlignment alignment;
    QVector<QImage> comparisonImages;
    const int ignoredBottom = s.type == "checkVideo" ? s.ignoreVideoBottomPercent : 0;
    if(s.type=="checkVideo"){
        QVector<qint64> times;
        for(const auto &sample:samples){comparisonImages.append(videoComparisonView(sample.image, ignoredBottom));times.append(sample.ms);}
        if (ignoredBottom > 0) {
            for (auto &frame : reference) {
                if (stop) { error = "已停止對齊"; return false; }
                const auto view = videoComparisonView(frame.image, ignoredBottom);
                const double aspect = frame.image.text("aspect").toDouble() * frame.image.height() / view.height();
                const QString source = frame.image.text("source");
                frame.image = view.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                frame.image.setText("aspect", QString::number(aspect));
                frame.image.setText("source", source);
                frame.fingerprint = frame.image.scaled(16, 9, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
        }
        alignment=alignReferenceSequence(comparisonImages,times,reference,stop);
        if(!alignment.valid){error=stop?"已停止對齊":"無法建立影片時間對齊";return false;}
    }
    QJsonArray observations;bool passed=false,allGood=true;int matches=0; qint64 firstTime=-1,firstRef=-1,lastRef=-1,lastEvidence=-1000;
    int badEvidence=0;QImage baseline;double maximum=0;QString lastVerdict;QString evidenceError;
    for(int i=0;i<samples.size() && !stop;++i){const auto &sample=samples[i];bool save=i==0 || i+1==samples.size() || sample.ms-lastEvidence>=1000;
        if(s.type=="checkVideo"){
            const qint64 expected=alignment.offsetMs+sample.ms-samples.first().ms;
            const auto &comparison = comparisonImages[i];
            const auto m=matchReferenceVideo(comparison,reference,expected%alignment.durationMs,alignment.durationMs);const qint64 ref=m.index>=0?reference[m.index].ms:-1;
            qint64 delta=ref-expected%alignment.durationMs;
            if(delta>alignment.durationMs/2)delta-=alignment.durationMs;
            if(delta<-alignment.durationMs/2)delta+=alignment.durationMs;
            const qint64 unwrapped=expected+delta;
            observations.append(QJsonObject{{"elapsedMs",double(sample.ms)},{"referenceMs",double(ref)},{"unwrappedReferenceMs",double(unwrapped)},{"expectedReferenceMs",double(expected%alignment.durationMs)},{"rgbError",m.rgb},{"pass",m.pass},{"verdict",m.verdict}});
            if(!m.pass){allGood=false;lastVerdict=m.verdict;save=save || badEvidence<3;if(save)++badEvidence;}
            if(m.pass){++matches;if(firstTime<0){firstTime=sample.ms;firstRef=unwrapped;}lastRef=unwrapped;
                const qint64 elapsed=sample.ms-firstTime,advance=lastRef-firstRef;
                if(elapsed>=3000 && advance>=1500 && std::abs(advance-elapsed)<=1000 && matches>=5)passed=true;}
            if(save && evidenceError.isEmpty()){if(!saveVideoEvidence(prefix+QString("_video_%1").arg(i+1),comparison,reference,m,evidenceError))allGood=false;lastEvidence=sample.ms;}
        }else{
            const double contrast=imageContrast(sample.image),difference=baseline.isNull()?-1:imageDifference(baseline,sample.image);
            const double measure=s.condition=="change"?difference:qMax(contrast,difference);maximum=qMax(maximum,measure);passed=passed || measure>=s.expected.toDouble();
            observations.append(QJsonObject{{"elapsedMs",double(sample.ms)},{"contrast",contrast},{"difference",difference}});
            if(save && !sample.image.save(prefix+QString("_image_%1.png").arg(i+1)))evidenceError="無法保存影像分析證據";
            baseline=sample.image;
        }
        if(i%30==0)notice(QString("離線分析中：%1 / %2 張；%3").arg(i+1).arg(samples.size()).arg(sampling));
    }
    if(stop){error="已停止離線分析";return false;}
    if(s.type=="checkVideo")passed=passed && alignment.motion>=0.5 && firstTime>=0 && std::abs((lastRef-firstRef)-(samples.last().ms-firstTime))<=1000;
    bool ok=passed && allGood && evidenceError.isEmpty();
    QString verdict=s.type=="checkVideo"?(ok?QString("影片畫面與播放進度符合"):alignment.motion<0.5?QString("擷取影格沒有足夠變化，無法確認播放（可能停格或背景停止更新；必要時可手動啟用前景相容）"):!allGood?lastVerdict:QString("畫面相符但無法確認播放進度（重複／靜止片段），不是顏色超標"))
        :QString("%1；影像量測最大值 %2／門檻 %3").arg(passed?"影像條件符合":"影像條件未符合").arg(maximum,0,'f',2).arg(s.expected);
    const QString alignmentDetail=s.type=="checkVideo"?QString("\n起始對齊：%1 秒／影片 %2 秒；跨循環 %3 次；畫面變動量 %4")
        .arg(alignment.offsetMs/1000.0,0,'f',3).arg(alignment.durationMs/1000.0,0,'f',3)
        .arg(qMax(qint64(0),(lastRef/alignment.durationMs)-(firstRef/alignment.durationMs))).arg(alignment.motion,0,'f',2):QString();
    detail=verdict+alignmentDetail+"\n"+sampling+QString("\n離線分析耗時 %1 秒；擷取檔：%2").arg(analysis.elapsed()/1000.0,0,'f',2).arg(prefix+".gvfgcap");
    if (ignoredBottom > 0) detail += QString("\n擷取與參考畫面均忽略底部 %1%。").arg(ignoredBottom);
    QJsonObject metrics{{"targetFps",clip.fps},{"actualFps",actual},{"samples",samples.size()},{"maxGapMs",double(maxGap)},{"captureDurationMs",clip.duration},{"analysisMs",double(analysis.elapsed())},{"alignmentOffsetMs",double(alignment.offsetMs)},{"referenceDurationMs",double(alignment.durationMs)},{"observedMotion",alignment.motion},{"captureMode",s.captureMode},{"activateBeforeCapture",s.activateBeforeCapture},{"offline",true},{"replaySource",replay},{"observations",observations},{"pass",ok}};
    metrics.insert("ignoreVideoBottomPercent", ignoredBottom);
    QSaveFile report(prefix+"_sampling.json");const auto data=QJsonDocument(metrics).toJson();
    if(!report.open(QIODevice::WriteOnly)||report.write(data)!=data.size()||!report.commit()){error="無法保存離線分析統計";return false;}
    if(!ok)error=evidenceError.isEmpty()?verdict:evidenceError;return ok;
}
bool offlineCaptureSelfTest(){
    QTemporaryDir dir;if(!dir.isValid())return false;std::atomic_bool stop=false;QString error;Clip clip;clip.fps=60;clip.duration=100;clip.source=QSize(32,18);
    QImage black(32,18,QImage::Format_RGB32);black.fill(Qt::black);auto white=black;white.fill(Qt::white);clip.samples={{0,black},{17,white},{34,black}};
    if(!writeClip(dir.path()+"/sample.gvfgcap",clip,stop,error))return false;Clip loaded;
    if(!readClip(dir.path()+"/sample.gvfgcap",loaded,stop,error)||loaded.samples.size()!=3||loaded.samples[1].ms!=17||loaded.samples[1].image!=white)return false;
    FlowStep step;step.type="checkImage";step.condition="change";step.expected="8";QString detail;
    if(!offlineImageStep(step,dir.path()+"/result",dir.path()+"/sample.gvfgcap",[](QString &){return QImage();},[](const QString &){},stop,detail,error))return false;
    // Exercise acquisition separately: the provider must finish collecting all
    // samples before any analysis progress notification is emitted.
    step.region=QRect(0,0,32,18);step.captureDurationMs=120;step.sampleFps=60;int captured=0;bool analysisStarted=false,wrongOrder=false;
    const bool acquired=offlineImageStep(step,dir.path()+"/acquired",{},[&](QString &){if(analysisStarted)wrongOrder=true;return ++captured%2?black:white;},
        [&](const QString &message){if(message.startsWith("擷取完成；"))analysisStarted=true;},stop,detail,error);
    if(!acquired || captured<2 || wrongOrder || !analysisStarted)return false;
    Clip recorded;if(!readClip(dir.path()+"/acquired.gvfgcap",recorded,stop,error)||recorded.samples.size()!=captured)return false;
    // Truncated datasets must fail rather than reading uninitialized image data.
    QFile corrupt(dir.path()+"/broken.gvfgcap");if(!corrupt.open(QIODevice::WriteOnly))return false;corrupt.write("GVC1",4);corrupt.close();Clip invalid;
    if(readClip(dir.path()+"/broken.gvfgcap",invalid,stop,error))return false;
    stop=true;Clip canceled;return !readClip(dir.path()+"/sample.gvfgcap",canceled,stop,error);
}

bool offlineVideoReplaySelfTest(const QString &path){
    QTemporaryDir dir;if(!dir.isValid())return false;std::atomic_bool stop=false;QString error,detail;QVector<ReferenceFrame> reference;
    if(!loadReferenceVideo(path,reference,error,stop))return false;
    Clip clip;clip.fps=10;clip.duration=4500;clip.source=reference.first().image.size();
    for(const auto &frame:reference)if(frame.ms<=4500)clip.samples.append({frame.ms,frame.image});
    if(clip.samples.size()<30 || !writeClip(dir.path()+"/moving.gvfgcap",clip,stop,error))return false;
    FlowStep step;step.type="checkVideo";step.referenceVideo=path;
    auto capture=[](QString &){return QImage();};auto notify=[](const QString &){};
    if(!offlineImageStep(step,dir.path()+"/moving",dir.path()+"/moving.gvfgcap",capture,notify,stop,detail,error))return false;
    auto overlay = clip;
    for (auto &sample : overlay.samples) {
        for (int y = sample.image.height() * 4 / 5; y < sample.image.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(sample.image.scanLine(y));
            for (int x = 0; x < sample.image.width(); ++x)
                row[x] = qGray(row[x]) < 128 ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
        }
    }
    const QString overlayPath = dir.path() + "/overlay.gvfgcap";
    if (!writeClip(overlayPath, overlay, stop, error)
        || offlineImageStep(step, dir.path()+"/overlay_full", overlayPath, capture, notify, stop, detail, error)) return false;
    step.ignoreVideoBottomPercent = 20;
    if (!offlineImageStep(step, dir.path()+"/overlay_masked", overlayPath, capture, notify, stop, detail, error)) return false;
    step.ignoreVideoBottomPercent = 0;
    const qint64 duration=referenceDurationMs(reference);
    for(qint64 offset:{duration/2,qMax(qint64(0),duration-700)}){
        clip.samples.clear();clip.duration=int(offset==duration/2?4500:duration*2+1500);
        for(qint64 elapsed=0;elapsed<=clip.duration;elapsed+=100){
            const qint64 target=(offset+elapsed)%duration;int nearest=0;qint64 distance=duration;
            for(int i=0;i<reference.size();++i){const auto diff=std::abs(reference[i].ms-target);if(diff<distance){distance=diff;nearest=i;}}
            clip.samples.append({elapsed,reference[nearest].image});
        }
        if(!writeClip(dir.path()+"/loop_input.gvfgcap",clip,stop,error)
            || !offlineImageStep(step,dir.path()+"/loop_result",dir.path()+"/loop_input.gvfgcap",capture,notify,stop,detail,error))return false;
    }
    for(auto &frame:clip.samples)frame.image=reference.first().image;
    if(!writeClip(dir.path()+"/frozen.gvfgcap",clip,stop,error))return false;
    return !offlineImageStep(step,dir.path()+"/frozen",dir.path()+"/frozen.gvfgcap",capture,notify,stop,detail,error);
}
