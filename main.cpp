#include <souistd.h>
#include <commgr2.h>
#include <core/SHostWnd.h>
#include <res.mgr/SResProviderMgr.h>

using namespace SOUI;

class CMainWnd : public SHostWnd
{
public:
    CMainWnd() : SHostWnd(_T("LAYOUT:app")) {}

protected:
    void OnClose()
    {
        DestroyWindow();
    }

    BEGIN_MSG_MAP_EX(CMainWnd)
        MSG_WM_CLOSE(OnClose)
        CHAIN_MSG_MAP(SHostWnd)
    END_MSG_MAP()
};

static SStringT GetExeDirectory()
{
    TCHAR path[MAX_PATH] = {0};
    GetModuleFileName(NULL, path, MAX_PATH);
    SStringT directory(path);
    int separator = directory.ReverseFind(_T('\\'));
    return directory.Left(separator + 1);
}

static int RunDemo(HINSTANCE instance, int showCommand)
{
    SComMgr2 components(_T("imgdecoder-png"));
    CAutoRefPtr<IRenderFactory> renderer;
    CAutoRefPtr<IImgDecoderFactory> decoder;
    if (!components.CreateRender_GDI((IObjRef**)&renderer) || !renderer ||
        !components.CreateImgDecoder((IObjRef**)&decoder) || !decoder)
    {
        MessageBox(NULL, _T("无法加载渲染器或图片解码器，请检查程序目录中的 SOUI 组件 DLL。"),
                   _T("newDemo"), MB_OK | MB_ICONERROR);
        return 1;
    }
    renderer->SetImgDecoderFactory(decoder);

    // EXE 放在 soui\bin 下，资源放在 newDemo\res 下。
    SStringT resourceDirectory = GetExeDirectory() + _T("..\\..\\newDemo\\res\\");

#ifdef _DEBUG
    LPCTSTR systemResource = _T("soui-sys-resourced.dll");
#else
    LPCTSTR systemResource = _T("soui-sys-resource.dll");
#endif
    HMODULE systemModule = LoadLibrary(systemResource);
    if (!systemModule)
    {
        MessageBox(NULL, systemResource, _T("无法加载 SOUI 系统资源"), MB_OK | MB_ICONERROR);
        return 1;
    }

    int result = 1;
    {
        SApplication application(renderer, instance);
        application.SetAppDir(resourceDirectory);

        CAutoRefPtr<IResProvider> systemProvider;
        CreateResProvider(RES_PE, (IObjRef**)&systemProvider);
        CAutoRefPtr<IResProvider> fileProvider;
        CreateResProvider(RES_FILE, (IObjRef**)&fileProvider);

        if (!systemProvider || !systemProvider->Init((WPARAM)systemModule, 0) ||
            !fileProvider || !fileProvider->Init((WPARAM)(LPCTSTR)resourceDirectory, 0))
        {
            MessageBox(NULL, _T("资源加载失败，请检查 newDemo\\res\\uires.idx 及其引用的 XML 文件。"),
                       _T("newDemo"), MB_OK | MB_ICONERROR);
        }
        else
        {
            application.LoadSystemNamedResource(systemProvider);
            application.AddResProvider(fileProvider);

            CMainWnd window;
            window.Create(NULL);
            if (IsWindow(window.m_hWnd))
            {
                window.GetNative()->SendMessage(WM_INITDIALOG);
                window.CenterWindow();
                window.ShowWindow(showCommand);
                result = application.Run(window.m_hWnd);
            }
            else
            {
                MessageBox(NULL, _T("创建窗口失败，请检查 LAYOUT:app 对应的 XML 布局。"),
                           _T("newDemo"), MB_OK | MB_ICONERROR);
            }
        }
    }
    FreeLibrary(systemModule);
    return result;
}

int WINAPI _tWinMain(HINSTANCE instance, HINSTANCE, LPTSTR, int showCommand)
{
    HRESULT initialized = OleInitialize(NULL);
    if (FAILED(initialized))
        return 1;

    int result = RunDemo(instance, showCommand);
    OleUninitialize();
    return result;
}
