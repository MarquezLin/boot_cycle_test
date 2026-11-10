#include "window_capture.h"
#include <QElapsedTimer>
#include <QThread>
#include <windows.h>
#include <dwmapi.h>
#include <roapi.h>
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
struct WindowCapture::Impl {
    QString stage="初始化";
    HWND window=nullptr;
    winrt::Windows::Graphics::SizeInt32 initialSize{};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice directDevice{nullptr};
    QSize clientSize;int sizeAdjustments=0;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<ID3D11Texture2D> staging;
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    ~Impl(){try{if(session)session.Close();if(pool)pool.Close();}catch(...) {}}
    void open(HWND handle) {
        if(!GraphicsCaptureSession::IsSupported())throw hresult_error(E_NOTIMPL);
        check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
        stage="D3D 裝置轉換";auto dxgi=device.as<IDXGIDevice>();com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        stage="建立視窗擷取目標";auto interop=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(handle,guid_of<GraphicsCaptureItem>(),put_abi(item)));
        auto direct=inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();directDevice=direct;
        RECT client{};GetClientRect(handle,&client);clientSize=QSize(client.right,client.bottom);
        stage="建立影格池";initialSize=item.Size();
        pool=Direct3D11CaptureFramePool::CreateFreeThreaded(direct,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,initialSize);
        stage="建立擷取工作階段";session=pool.CreateCaptureSession(item);
        try{session.IsCursorCaptureEnabled(false);}catch(const hresult_error &){}
        stage="啟動擷取";session.StartCapture();window=handle;stage="讀取影格";
    }
};
WindowCapture::WindowCapture(){HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);if(hr==RPC_E_CHANGED_MODE)hr=RoInitialize(RO_INIT_SINGLETHREADED);initialized=SUCCEEDED(hr);}
WindowCapture::~WindowCapture(){d.reset();if(initialized)RoUninitialize();}
QImage WindowCapture::next(quintptr handle,QString &error,const std::atomic_bool *stop) {
    error.clear();if(!initialized){error="無法初始化 Windows 背景擷取環境";return {};}const auto window=reinterpret_cast<HWND>(handle);
    if(!IsWindow(window)){error="找不到背景擷取視窗";return {};}
    if(IsIconic(window)){error="待測視窗已最小化，請還原；背景擷取不需切到前景";return {};}
    try {
        if(!d || d->window!=window){d.reset();d=std::make_unique<Impl>();d->open(window);}
        QElapsedTimer timer;timer.start();
        Direct3D11CaptureFrame frame{nullptr};
        while(!(stop&&*stop) && timer.elapsed()<1500) {
            frame=d->pool.TryGetNextFrame();
            if(frame)break;
            QThread::msleep(2);
        }
        if(!frame){error=stop&&*stop?"已停止背景擷取":"背景擷取未收到新影格（請確認視窗仍在繪圖）";return {};}
        // Keep the newest available frame; never count an old cached image as a new sample.
        while(auto newer=d->pool.TryGetNextFrame()){frame.Close();frame=std::move(newer);}
        const auto size=frame.ContentSize();
        RECT currentClient{};GetClientRect(window,&currentClient);
        if(d->clientSize!=QSize(currentClient.right,currentClient.bottom)){error="視窗大小已變更，請重新框選後再試跑";return {};}
        if(size.Width!=d->initialSize.Width || size.Height!=d->initialSize.Height){
            if(size.Width<=0 || size.Height<=0 || ++d->sizeAdjustments>2){error="背景擷取影格尺寸不穩定，請重試";return {};}
            frame.Close();d->staging=nullptr;d->initialSize=size;
            d->pool.Recreate(d->directDevice,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
            return next(handle,error,stop);
        }
        d->sizeAdjustments=0;
        auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(!d->staging){desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
            check_hresult(d->device->CreateTexture2D(&desc,nullptr,d->staging.put()));}
        d->context->CopyResource(d->staging.get(),texture.get());D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(d->context->Map(d->staging.get(),0,D3D11_MAP_READ,0,&mapped));
        QImage image(static_cast<uchar *>(mapped.pData),size.Width,size.Height,int(mapped.RowPitch),QImage::Format_RGB32);
        QImage copy=image.copy();d->context->Unmap(d->staging.get(),0);frame.Close();
        RECT bounds{},client{};POINT origin{};
        if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)))
            || !GetClientRect(window,&client) || !ClientToScreen(window,&origin)){error="無法取得背景視窗內容座標";return {};}
        const QRect content(origin.x-bounds.left,origin.y-bounds.top,client.right,client.bottom);
        if(!copy.rect().contains(content)){error="背景影像尺寸與視窗內容座標不符，請重新框選";return {};}
        return copy.copy(content);
    }catch(const hresult_error &e){error=QString("背景視窗擷取失敗（0x%1）；可改用前景桌面擷取")
        .arg(quint32(e.code().value),8,16,QChar('0'));d.reset();return {};}
    catch(const std::exception &){error="背景擷取資源不足或無法建立（"+(d?d->stage:QString("初始化"))+"），請改用前景桌面擷取";d.reset();return {};}
}
