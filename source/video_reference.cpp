#include "video_reference.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <cmath>
#include <algorithm>
using Microsoft::WRL::ComPtr;
namespace {
QImage thumb(const QImage &im) {return im.scaled(160,90,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);}
double structureDistance(const QImage &a,const QImage &b) {
    // Unordered RGB mean keeps red/blue exchange visible to the colour check,
    // while allowing the temporal search to find the correct scene.
    double distance=0;
    for(int y=0;y<a.height();++y)for(int x=0;x<a.width();++x) {
        const auto p=a.pixel(x,y),q=b.pixel(x,y);
        distance+=std::abs((qRed(p)+qGreen(p)+qBlue(p))-(qRed(q)+qGreen(q)+qBlue(q)))/3.0;
    }
    return distance/(a.width()*a.height());
}
}
bool loadReferenceVideo(const QString &path,QVector<ReferenceFrame> &frames,QString &error,const std::atomic_bool &stop) {
    frames.clear();error.clear();
    HRESULT hr=MFStartup(MF_VERSION);if(FAILED(hr)){error="無法啟動 Windows 影片解碼器";return false;}
    struct Shutdown {~Shutdown(){MFShutdown();}} shutdown;
    ComPtr<IMFAttributes> attributes;ComPtr<IMFSourceReader> reader;ComPtr<IMFMediaType> type;
    MFCreateAttributes(&attributes,2);if(attributes)attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);
    hr=MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(path.utf16()),attributes.Get(),&reader);
    auto failed=[&](const QString &message){error=message+QString("（0x%1）").arg(static_cast<quint32>(hr),8,16,QChar('0'));frames.clear();return false;};
    if(FAILED(hr))return failed("無法開啟參考影片，請使用 Windows 可解碼的短片");
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE);reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),TRUE);
    MFCreateMediaType(&type);type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
    hr=reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get());if(FAILED(hr))return failed("影片無法轉為 RGB32");
    PROPVARIANT duration;PropVariantInit(&duration);hr=reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&duration);
    const qint64 durationMs=SUCCEEDED(hr)&&duration.vt==VT_UI8?qint64(duration.uhVal.QuadPart/10000):0;
    const bool tooLong=SUCCEEDED(hr)&&duration.vt==VT_UI8&&duration.uhVal.QuadPart>1200000000ULL;PropVariantClear(&duration);
    if(tooLong){error="參考影片請使用 120 秒以內的短片";return false;}
    QElapsedTimer timer;timer.start();qint64 next=0,decodedEndMs=0;LONGLONG previousDecoded=-1;
    while(!stop) {
        if(timer.elapsed()>45000){error="參考影片索引超過 45 秒，請改用更短或較低解析度影片";frames.clear();return false;}
        DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;
        hr=reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,&time,&sample);
        if(FAILED(hr))return failed("參考影片解碼失敗");
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
        if(!sample)continue;
        LONGLONG sampleDuration=0;sample->GetSampleDuration(&sampleDuration);
        if(sampleDuration<=0)sampleDuration=previousDecoded>=0?time-previousDecoded:333333;
        decodedEndMs=qMax(decodedEndMs,qint64((time+qMax(LONGLONG(1),sampleDuration))/10000));previousDecoded=time;
        if(time/10000<next)continue;
        if(time/10000>120000 || frames.size()>=3601){error="參考影片超過 120 秒限制";frames.clear();return false;}
        ComPtr<IMFMediaType> current;hr=reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&current);if(FAILED(hr))return failed("讀取影片格式失敗");
        UINT32 w=0,h=0;MFGetAttributeSize(current.Get(),MF_MT_FRAME_SIZE,&w,&h);
        if(!w||!h||w>8192||h>8192){error="參考影片尺寸無效或超過 8192";return false;}
        ComPtr<IMFMediaBuffer> buffer;hr=sample->ConvertToContiguousBuffer(&buffer);if(FAILED(hr))return failed("讀取影格失敗");
        ComPtr<IMF2DBuffer> twoD;BYTE *data=nullptr;LONG stride=0;QImage image;
        if(SUCCEEDED(buffer.As(&twoD))&&SUCCEEDED(twoD->Lock2D(&data,&stride))) {
            image=QImage(int(w),int(h),QImage::Format_RGB32);
            for(UINT32 y=0;y<h;++y)memcpy(image.scanLine(int(y)),data+ptrdiff_t(y)*stride,size_t(w)*4);
            twoD->Unlock2D();
        } else {
            DWORD length=0;hr=buffer->Lock(&data,nullptr,&length);if(FAILED(hr))return failed("鎖定影格失敗");
            UINT32 stored=0;current->GetUINT32(MF_MT_DEFAULT_STRIDE,&stored);stride=stored?static_cast<LONG>(stored):LONG(w*4);
            if(quint64(std::abs(stride))*h>length){buffer->Unlock();error="參考影片影格長度不符";return false;}
            image=QImage(int(w),int(h),QImage::Format_RGB32);
            BYTE *top=stride<0?data+size_t(h-1)*size_t(-stride):data;
            for(UINT32 y=0;y<h;++y)memcpy(image.scanLine(int(y)),top+ptrdiff_t(y)*stride,size_t(w)*4);
            buffer->Unlock();
        }
        image.setText("aspect",QString::number(double(w)/h));
        auto thumbnail=thumb(image);thumbnail.setText("aspect",image.text("aspect"));thumbnail.setText("source",path);thumbnail.setText("durationMs",QString::number(durationMs));
        frames.append({time/10000,thumbnail,thumbnail.scaled(16,9,Qt::IgnoreAspectRatio,Qt::SmoothTransformation)});next=time/10000+33;
    }
    if(decodedEndMs>0)for(auto &frame:frames)frame.image.setText("durationMs",QString::number(decodedEndMs));
    if(stop){error="已停止參考影片索引";return false;}
    if(frames.size()<3){error="參考影片影格不足，請提供至少 1 秒的影片";return false;}
    return true;
}
qint64 referenceDurationMs(const QVector<ReferenceFrame> &frames){
    if(frames.isEmpty())return 0;
    const auto stored=frames.first().image.text("durationMs").toLongLong();
    if(stored>frames.last().ms)return stored;
    const qint64 interval=frames.size()>1?qMax(qint64(1),frames.last().ms-frames[frames.size()-2].ms):33;
    return frames.last().ms+interval;
}
namespace {
qint64 circularDistance(qint64 a,qint64 b,qint64 duration){const auto d=std::abs(a-b);return qMin(d,duration-d);}
int nearestReference(const QVector<ReferenceFrame> &frames,qint64 time,qint64 duration){
    auto it=std::lower_bound(frames.begin(),frames.end(),time,[](const ReferenceFrame &frame,qint64 value){return frame.ms<value;});
    const int high=it==frames.end()?0:int(it-frames.begin());const int low=high==0?int(frames.size())-1:high-1;
    return circularDistance(frames[high].ms,time,duration)<circularDistance(frames[low].ms,time,duration)?high:low;
}
}
VideoAlignment alignReferenceSequence(const QVector<QImage> &images,const QVector<qint64> &times,const QVector<ReferenceFrame> &frames,const std::atomic_bool &stop){
    VideoAlignment result;result.durationMs=referenceDurationMs(frames);
    if(images.size()<2 || images.size()!=times.size() || frames.size()<2 || result.durationMs<=0)return result;
    QVector<QImage> coarseImages;coarseImages.reserve(images.size());
    for(const auto &image:images){if(stop)return result;coarseImages.append(image.scaled(16,9,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32));}
    // Independently require observed image changes; predicting timestamps alone
    // must never make a frozen capture look like forward video progression.
    for(int i=1;i<coarseImages.size();++i){double diff=0;for(int y=0;y<9;++y)for(int x=0;x<16;++x){auto a=coarseImages.first().pixel(x,y),b=coarseImages[i].pixel(x,y);diff+=(std::abs(qRed(a)-qRed(b))+std::abs(qGreen(a)-qGreen(b))+std::abs(qBlue(a)-qBlue(b)))/3.0;}result.motion=qMax(result.motion,diff/144);}
    QVector<QImage> fingerprints;for(const auto &frame:frames)fingerprints.append(frame.fingerprint.isNull()?frame.image.scaled(16,9,Qt::IgnoreAspectRatio,Qt::SmoothTransformation):frame.fingerprint);
    const int checks=qMin(24,int(images.size()));
    for(const auto &start:frames){
        if(stop)return result;double score=0;
        for(int n=0;n<checks;++n){const int i=n*(int(images.size())-1)/(checks-1);
            const qint64 expected=(start.ms+times[i]-times.first())%result.durationMs;
            score+=structureDistance(coarseImages[i],fingerprints[nearestReference(frames,expected,result.durationMs)]);
        }
        score/=checks;if(score<result.score){result.score=score;result.offsetMs=start.ms;}
    }
    result.valid=!stop;return result;
}
VideoMatch matchReferenceVideo(const QImage &actual,const QVector<ReferenceFrame> &frames,qint64 expectedMs,qint64 durationMs) {
    VideoMatch m;if(actual.isNull()||frames.isEmpty()){m.verdict="無法對齊：沒有可比較的影格";return m;}
    const double aspect=frames.first().image.text("aspect").toDouble();
    if(aspect>0&&std::abs(double(actual.width())/actual.height()/aspect-1)>0.04){m.verdict="無法對齊：框選區域長寬比不符，請排除邊框與黑邊";return m;}
    const auto a=thumb(actual);
    const auto coarse=a.scaled(16,9,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    QVector<std::pair<double,int>> candidates;candidates.reserve(frames.size());
    auto temporal=[&](int i){return expectedMs>=0 && durationMs>0?circularDistance(frames[i].ms,expectedMs%durationMs,durationMs):qint64(0);};
    for(int i=0;i<frames.size();++i){if(expectedMs>=0 && durationMs>0 && temporal(i)>200)continue;
        const auto &frame=frames[i];const auto fingerprint=frame.fingerprint.isNull()?frame.image.scaled(16,9,Qt::IgnoreAspectRatio,Qt::SmoothTransformation):frame.fingerprint;
        candidates.append({structureDistance(coarse,fingerprint)+temporal(i)*0.00001,i});}
    if(candidates.isEmpty()){m.verdict="無法對齊：對應時間沒有參考影格";return m;}
    const auto count=qMin(qsizetype(8),candidates.size());
    std::partial_sort(candidates.begin(),candidates.begin()+count,candidates.end());double best=1e9;
    for(int candidate=0;candidate<count;++candidate){const int i=candidates[candidate].second;const double distance=structureDistance(a,frames[i].image),score=distance+temporal(i)*0.00001;if(score<best){best=score;m.index=i;m.structure=distance;}}
    const auto &b=frames[m.index].image;double rgb=0,swapped=0;int bad=0;
    for(int y=0;y<90;++y)for(int x=0;x<160;++x){auto p=a.pixel(x,y),q=b.pixel(x,y);
        const int d=std::abs(qRed(p)-qRed(q))+std::abs(qGreen(p)-qGreen(q))+std::abs(qBlue(p)-qBlue(q));rgb+=d/3.0;
        swapped+=(std::abs(qRed(p)-qBlue(q))+std::abs(qGreen(p)-qGreen(q))+std::abs(qBlue(p)-qRed(q)))/3.0;
        if(d/3.0>40)++bad;
    }
    m.rgb=rgb/14400;m.swapped=swapped/14400;m.badFraction=double(bad)/14400;
    m.pass=m.rgb<=12 && m.badFraction<=0.08;
    if(m.pass)m.verdict="畫面相符";
    else if(m.swapped<12&&m.swapped<m.rgb*0.55)m.verdict="疑似紅藍顛倒";
    else if(m.structure>28)m.verdict="無法對齊：影片內容、播放位置或畫面差異過大";
    else if(m.badFraction<0.35&&m.badFraction>0.01)m.verdict="疑似局部破圖／畫面異常";
    else m.verdict="疑似偏色／亮度或色彩轉換異常";
    return m;
}
bool saveVideoEvidence(const QString &prefix,const QImage &actual,const QVector<ReferenceFrame> &frames,const VideoMatch &m,QString &error) {
    if(m.index<0)return true;
    const auto a=thumb(actual),b=frames[m.index].image;QImage diff(a.size(),QImage::Format_RGB32);
    for(int y=0;y<90;++y)for(int x=0;x<160;++x){auto p=a.pixel(x,y),q=b.pixel(x,y);diff.setPixel(x,y,qRgb(std::abs(qRed(p)-qRed(q)),std::abs(qGreen(p)-qGreen(q)),std::abs(qBlue(p)-qBlue(q))));}
    QFile file(prefix+".json");
    const auto json=QJsonDocument(QJsonObject{{"referenceFile",frames[m.index].image.text("source")},{"verdict",m.verdict},{"referenceMs",double(frames[m.index].ms)},{"rgbMeanError",m.rgb},{"structureError",m.structure},{"redBlueSwapError",m.swapped},{"badPixelFraction",m.badFraction},{"pass",m.pass}}).toJson();
    if(!actual.save(prefix+"_actual.png")||!b.save(prefix+"_reference.png")||!diff.save(prefix+"_difference.png")||!file.open(QIODevice::WriteOnly)||file.write(json)!=json.size()){error="無法保存影片比較證據";return false;}return true;
}
bool videoReferenceSelfTest(){
    QImage image(160,90,QImage::Format_RGB32);image.fill(QColor(190,80,25));QPainter p(&image);p.fillRect(20,20,60,40,QColor(25,160,220));p.end();
    QVector<ReferenceFrame> frames{{0,image}};if(!matchReferenceVideo(image,frames).pass)return false;
    auto swap=image.rgbSwapped();if(matchReferenceVideo(swap,frames).verdict!="疑似紅藍顛倒")return false;
    auto damage=image;QPainter d(&damage);d.fillRect(30,30,60,30,Qt::magenta);d.end();if(matchReferenceVideo(damage,frames).pass)return false;
    return !matchReferenceVideo(QImage(),frames).pass;
}
