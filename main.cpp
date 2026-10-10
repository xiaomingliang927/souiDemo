#define WINVER 0x06010000
#define _WIN32_WINNT 0x06010000
#include <windows.h>
#include <souistd.h>
#include <commgr2.h>
#include <core/SHostWnd.h>
#include <control/SListView.h>
#include <control/SCmnCtrl.h>
#include <helper/SAdapterBase.h>
#include <res.mgr/SResProviderMgr.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfidl.h>
#include <propvarutil.h>
#include <vector>
#include <string>
#include <set>
#include <mutex>
#include <thread>
#include <algorithm>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

using namespace SOUI;
static const UINT WM_FILE_METADATA = WM_APP + 41;

struct FileItem {
    std::wstring path, name, info, duration;
    ULONGLONG bytes;
    bool selected;
    FileItem() : bytes(0), selected(false) {}
};

class CMainWnd;

class CFileAdapter : public SAdapterBase {
    CMainWnd *m_owner;
public:
    explicit CFileAdapter(CMainWnd *owner) : m_owner(owner) {}
    virtual int getCount();
    virtual ULONG_PTR getItemData(int position);
    virtual void getView(int position, SWindow *pItem, pugi::xml_node xmlTemplate);
    bool OnRemove(EventCmd *evt);
    bool OnCheck(EventSwndStateChanged *evt);
    bool OnPanelClick(EventArgs *evt);
};

class CMainWnd : public SHostWnd {
    friend class CFileAdapter;
    std::vector<FileItem> m_files;
    std::vector<std::wstring> m_tasks;
    std::mutex m_mutex;
    std::thread m_worker;
    bool m_closing;
    bool m_processing;
    int m_progress;
    CFileAdapter *m_adapter;
public:
    CMainWnd() : SHostWnd(_T("LAYOUT:app")), m_closing(false), m_processing(false), m_progress(0), m_adapter(NULL) {}
    ~CMainWnd() { StopWorker(); }
    void AddPaths(const std::vector<std::wstring> &paths);
    void RemoveAt(int index);
    void ClearFiles();
    void RefreshList();
    void UpdateSummary();
    void UpdateEmptyState();
    void UpdateTasks();
    void StopWorker();
    bool OnMetadata(FileItem *item);
    const FileItem &Item(int i) const { return m_files[i]; }
    int Count() const { return (int)m_files.size(); }
protected:
    void OnClose() { if(m_closing) return; m_closing=true; if(m_processing) { KillTimer(2); m_processing=false; } DestroyWindow(); }
    LRESULT OnInitDialog(HWND, LPARAM);
    LRESULT OnMetadataMessage(UINT, WPARAM, LPARAM);
    void SelectNavigation(LPCTSTR selectedName);
    void OnConvert() { SelectNavigation(_T("btn_convert")); SWindow *main=FindChildByName(_T("main_content")); SWindow *tasks=FindChildByName(_T("task_content")); SWindow *settings=FindChildByName(_T("right_settings")); if(main) main->SetVisible(TRUE); if(tasks) tasks->SetVisible(FALSE); if(settings) settings->SetVisible(TRUE); }
    void OnTasks() { SelectNavigation(_T("btn_tasks")); SWindow *main=FindChildByName(_T("main_content")); SWindow *tasks=FindChildByName(_T("task_content")); SWindow *settings=FindChildByName(_T("right_settings")); if(main) main->SetVisible(FALSE); if(tasks) tasks->SetVisible(TRUE); if(settings) settings->SetVisible(FALSE); UpdateTasks(); }
    void OnSettings() { SelectNavigation(_T("btn_settings")); }
    void OnStart();
    void OnTimer(UINT_PTR id);
    void OnAddFiles();
    void OnClearFiles() { ClearFiles(); }
    EVENT_MAP_BEGIN()
        EVENT_ID_COMMAND(1, OnClose)
        EVENT_NAME_COMMAND(L"btn_convert", OnConvert)
        EVENT_NAME_COMMAND(L"btn_tasks", OnTasks)
        EVENT_NAME_COMMAND(L"btn_settings", OnSettings)
        EVENT_NAME_COMMAND(L"btn_add_files", OnAddFiles)
        EVENT_NAME_COMMAND(L"btn_add_files_empty", OnAddFiles)
        EVENT_NAME_COMMAND(L"btn_clear_files", OnClearFiles)
        EVENT_NAME_COMMAND(L"btn_start", OnStart)
    EVENT_MAP_END()
    BEGIN_MSG_MAP_EX(CMainWnd)
        MSG_WM_CLOSE(OnClose)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_TIMER(OnTimer)
        MESSAGE_HANDLER_EX(WM_FILE_METADATA, OnMetadataMessage)
        CHAIN_MSG_MAP(SHostWnd)
    END_MSG_MAP()
};

static std::wstring FileNameOf(const std::wstring &path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}
static std::wstring FormatSize(ULONGLONG n) {
    wchar_t b[64];
    if (n >= 1024ULL * 1024ULL * 1024ULL) swprintf_s(b, L"%.1f GB", n / 1073741824.0);
    else if (n >= 1024ULL * 1024ULL) swprintf_s(b, L"%.1f MB", n / 1048576.0);
    else swprintf_s(b, L"%.1f KB", n / 1024.0);
    return b;
}
static std::wstring FormatDuration(ULONGLONG hns) {
    ULONGLONG s = hns / 10000000ULL;
    wchar_t b[32]; swprintf_s(b, L"%02llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    return b;
}
static FileItem ProbeFile(const std::wstring &path) {
    FileItem item; item.path = path; item.name = FileNameOf(path); item.info = L"正在读取媒体信息…"; item.duration = L"--:--";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        item.bytes = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (SUCCEEDED(hr)) {
        IMFSourceReader *reader = NULL;
        hr = MFCreateSourceReaderFromURL(path.c_str(), NULL, &reader);
        if (SUCCEEDED(hr)) {
            PROPVARIANT duration; PropVariantInit(&duration);
            if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)))
                item.duration = FormatDuration(duration.uhVal.QuadPart);
            PropVariantClear(&duration);
            IMFMediaType *type = NULL;
            if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &type))) {
                UINT32 w = 0, h = 0; MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
                GUID subtype = GUID_NULL; type->GetGUID(MF_MT_SUBTYPE, &subtype);
                wchar_t codec[16] = L"video"; StringFromGUID2(subtype, codec, 16);
                wchar_t inf[160]; swprintf_s(inf, L"%s · %u×%u · %s", codec, w, h, FormatSize(item.bytes).c_str());
                item.info = inf; type->Release();
            } else item.info = L"视频 · 分辨率未知 · " + FormatSize(item.bytes);
            reader->Release();
        } else item.info = L"无法读取媒体 · " + FormatSize(item.bytes);
        MFShutdown();
    } else item.info = L"Media Foundation 不可用 · " + FormatSize(item.bytes);
    return item;
}

int CFileAdapter::getCount() { return m_owner->Count(); }
ULONG_PTR CFileAdapter::getItemData(int position) { return (ULONG_PTR)position; }
void CFileAdapter::getView(int position, SWindow *pItem, pugi::xml_node xmlTemplate) {
    if (pItem->GetChildrenCount() == 0) {
        pItem->InitFromXml(xmlTemplate);
        SWindow *remove = pItem->FindChildByName(_T("btn_remove"));
        if (remove) remove->GetEventSet()->subscribeEvent(EVT_CMD, Subscriber(&CFileAdapter::OnRemove, this));
        SWindow *check = pItem->FindChildByName(_T("chk_select"));
        if (check) check->GetEventSet()->subscribeEvent(EventSwndStateChanged::EventID, Subscriber(&CFileAdapter::OnCheck, this));
        pItem->GetEventSet()->subscribeEvent(EventItemPanelClick::EventID, Subscriber(&CFileAdapter::OnPanelClick, this));
    }
    pItem->SetUserData(position);
    const FileItem &f = m_owner->Item(position);
    SWindow *name = pItem->FindChildByName(_T("txt_file_name"));
    SWindow *info = pItem->FindChildByName(_T("txt_file_info"));
    SWindow *duration = pItem->FindChildByName(_T("txt_duration"));
    SWindow *check = pItem->FindChildByName(_T("chk_select"));
    if (name) name->SetWindowText(f.name.c_str());
    if (info) info->SetWindowText(f.info.c_str());
    if (duration) duration->SetWindowText(f.duration.c_str());
    if (check) check->SetCheck(f.selected ? TRUE : FALSE);
}
bool CFileAdapter::OnRemove(EventCmd *evt) { SWindow *s = sobj_cast<SWindow>(evt->sender); if (s) m_owner->RemoveAt((int)s->GetRoot()->GetUserData()); return true; }
bool CFileAdapter::OnCheck(EventSwndStateChanged *evt) { SWindow *s = sobj_cast<SWindow>(evt->sender); if (s) { SItemPanel *root = sobj_cast<SItemPanel>(s->GetRoot()); if (root && root->GetUserData() < (ULONG_PTR)m_owner->Count()) m_owner->m_files[(int)root->GetUserData()].selected = s->IsChecked() != FALSE; } return true; }
bool CFileAdapter::OnPanelClick(EventArgs *evt) { EventItemPanelClick *click = sobj_cast<EventItemPanelClick>(evt); if (click && click->sender) { SItemPanel *panel = sobj_cast<SItemPanel>(click->sender); if (panel && panel->GetItemIndex() >= 0 && panel->GetItemIndex() < m_owner->Count()) { for (int i=0; i<m_owner->Count(); ++i) m_owner->m_files[i].selected = false; m_owner->m_files[panel->GetItemIndex()].selected = true; m_owner->RefreshList(); } } return true; }

void CMainWnd::RefreshList() { if (m_adapter) m_adapter->notifyDataSetChanged(); UpdateSummary(); UpdateEmptyState(); }
void CMainWnd::UpdateTasks() { SWindow *tasks=FindChildByName(_T("txt_tasks")); if(!tasks) return; if(m_tasks.empty()) { tasks->SetWindowText(_T("暂无转换任务")); return; } std::wstring text; for(size_t i=0;i<m_tasks.size();++i) { if(i) text += L"\n"; text += m_tasks[i]; } tasks->SetWindowText(text.c_str()); }
void CMainWnd::UpdateSummary() { ULONGLONG total = 0; for (size_t i=0;i<m_files.size();++i) total += m_files[i].bytes; SWindow *s=FindChildByName(_T("txt_file_summary")); if(s) s->SetWindowText(SStringT().Format(_T("%d 个 · %s"),(int)m_files.size(),FormatSize(total).c_str())); SWindow *preview=FindChildByName(_T("source_preview_info")); SWindow *output=FindChildByName(_T("output_preview_info")); if(preview) preview->SetWindowText(m_files.empty() ? _T("等待加载视频") : SStringT().Format(_T("%s\n%s"),m_files[0].name.c_str(),m_files[0].info.c_str())); if(output) output->SetWindowText(m_files.empty() ? _T("处理后的视频将在这里显示") : _T("等待模拟处理")); }
void CMainWnd::UpdateEmptyState() { SWindow *empty=FindChildByName(_T("empty_state")); SWindow *list=FindChildByName(_T("file_list")); bool isEmpty=m_files.empty(); if(empty) empty->SetVisible(isEmpty); if(list) list->SetVisible(!isEmpty); }
void CMainWnd::RemoveAt(int index) { if(index>=0 && index<Count()) { m_files.erase(m_files.begin()+index); RefreshList(); } }
void CMainWnd::ClearFiles() { m_files.clear(); RefreshList(); }
void CMainWnd::AddPaths(const std::vector<std::wstring> &paths) {
    { std::lock_guard<std::mutex> lock(m_mutex); m_closing=false; }
    std::set<std::wstring> existing; for(size_t i=0;i<m_files.size();++i){ std::wstring k=m_files[i].path; std::transform(k.begin(),k.end(),k.begin(),towlower); existing.insert(k); }
    for(size_t i=0;i<paths.size();++i){ std::wstring k=paths[i]; std::transform(k.begin(),k.end(),k.begin(),towlower); if(existing.insert(k).second){ FileItem f; f.path=paths[i]; f.name=FileNameOf(paths[i]); f.info=L"正在读取媒体信息…"; f.duration=L"--:--"; m_files.push_back(f); } }
    RefreshList();
    StopWorker();
    { std::lock_guard<std::mutex> lock(m_mutex); m_closing=false; }
    m_worker=std::thread([this](){ for(size_t i=0;;++i){ std::wstring path; { std::lock_guard<std::mutex> lock(m_mutex); if(m_closing) return; if(i>=m_files.size()) return; path=m_files[i].path; } FileItem *result=new FileItem(ProbeFile(path)); if(!m_closing && ::IsWindow(m_hWnd)) ::PostMessage(m_hWnd,WM_FILE_METADATA,(WPARAM)i,(LPARAM)result); else delete result; } });
}
void CMainWnd::StopWorker() { { std::lock_guard<std::mutex> lock(m_mutex); m_closing=true; } if(m_worker.joinable()) m_worker.join(); }
bool CMainWnd::OnMetadata(FileItem *item) { int idx=-1; for(int i=0;i<Count();++i) if(m_files[i].path==item->path){idx=i;break;} if(idx>=0){bool selected=m_files[idx].selected; m_files[idx]=*item; m_files[idx].selected=selected; if(m_adapter) m_adapter->notifyItemDataChanged(idx); UpdateSummary();} return idx>=0; }
LRESULT CMainWnd::OnMetadataMessage(UINT, WPARAM, LPARAM lParam) { FileItem *p=(FileItem*)lParam; OnMetadata(p); delete p; return 0; }
LRESULT CMainWnd::OnInitDialog(HWND, LPARAM) { SListView *list=FindChildByName2<SListView>(_T("file_list")); if(list){m_adapter=new CFileAdapter(this); list->SetAdapter(m_adapter); m_adapter->Release();} SProgress *progress=FindChildByName2<SProgress>(_T("progress_task")); if(progress){progress->SetRange(0,100);progress->SetValue(0);} UpdateSummary(); UpdateEmptyState(); UpdateTasks(); return 0; }
void CMainWnd::OnStart() { if(m_processing) return; int selectedIndex=-1; for(int i=0;i<Count();++i) if(m_files[i].selected){selectedIndex=i;break;} if(selectedIndex<0) { SWindow *status=FindChildByName(_T("txt_status")); if(status) status->SetWindowText(_T("请先点击选择一个视频文件")); return; } m_tasks.push_back(std::wstring(L"等待处理：") + m_files[selectedIndex].name); UpdateTasks(); m_processing=true; m_progress=0; SWindow *button=FindChildByName(_T("btn_start")); if(button) button->SetWindowText(_T("处理中…")); SWindow *status=FindChildByName(_T("txt_status")); if(status) status->SetWindowText(_T("正在模拟 FFmpeg 处理…")); SProgress *progress=FindChildByName2<SProgress>(_T("progress_task")); if(progress) progress->SetValue(0); SWindow *percent=FindChildByName(_T("txt_progress_percent")); if(percent) percent->SetWindowText(_T("0%")); SetTimer(2,100); }
void CMainWnd::OnTimer(UINT_PTR id) { if(id!=2) return; if(!m_processing) { KillTimer(2); return; } m_progress+=5; SProgress *progress=FindChildByName2<SProgress>(_T("progress_task")); if(progress) progress->SetValue(m_progress); SWindow *percent=FindChildByName(_T("txt_progress_percent")); if(percent) percent->SetWindowText(SStringT().Format(_T("%d%%"),m_progress)); SWindow *status=FindChildByName(_T("txt_status")); if(status) status->SetWindowText(SStringT().Format(_T("模拟 FFmpeg 处理中 %d%%"),m_progress)); if(m_progress>=100) { m_processing=false; KillTimer(2); SWindow *button=FindChildByName(_T("btn_start")); if(button) button->SetWindowText(_T("处理完成")); if(status) status->SetWindowText(_T("处理完成（模拟）")); if(!m_tasks.empty()) { int selectedIndex=-1; for(int i=0;i<Count();++i) if(m_files[i].selected){selectedIndex=i;break;} if(selectedIndex>=0) m_tasks.back()=std::wstring(L"已完成：") + m_files[selectedIndex].name + L"（模拟）"; } UpdateTasks(); SWindow *output=FindChildByName(_T("output_preview_info")); if(output) output->SetWindowText(_T("输出视频：模拟转换完成")); } }
void CMainWnd::OnAddFiles() { IFileOpenDialog *dialog=NULL; if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return; DWORD options=0; dialog->GetOptions(&options); dialog->SetOptions(options|FOS_ALLOWMULTISELECT|FOS_FILEMUSTEXIST); COMDLG_FILTERSPEC filters[]={{L"视频文件",L"*.mp4;*.mkv;*.webm;*.avi;*.mov;*.wmv"},{L"所有文件",L"*.*"}}; dialog->SetFileTypes(2,filters); if(SUCCEEDED(dialog->Show(m_hWnd))){ IShellItemArray *items=NULL; if(SUCCEEDED(dialog->GetResults(&items))){ DWORD count=0; items->GetCount(&count); std::vector<std::wstring> paths; for(DWORD i=0;i<count;++i){IShellItem *item=NULL; PWSTR p=NULL; if(SUCCEEDED(items->GetItemAt(i,&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p))){paths.push_back(p);CoTaskMemFree(p);} if(item)item->Release();} AddPaths(paths); items->Release(); }} dialog->Release(); }

void CMainWnd::SelectNavigation(LPCTSTR selectedName) { LPCTSTR buttons[]={_T("btn_convert"),_T("btn_tasks"),_T("btn_settings")}; for(int i=0;i<3;++i){SWindow *b=FindChildByName(buttons[i]);if(b){bool selected=_tcscmp(buttons[i],selectedName)==0;b->SetAttribute(L"skin",selected?L"skin_nav_selected":L"skin_nav_normal");b->Invalidate();}} }
static SStringT GetExeDirectory(){TCHAR path[MAX_PATH]={0};GetModuleFileName(NULL,path,MAX_PATH);SStringT d(path);return d.Left(d.ReverseFind(_T('\\'))+1);}
static int RunDemo(HINSTANCE instance, int showCommand)
{
    SComMgr2 components(_T("imgdecoder-png"));
    CAutoRefPtr<IRenderFactory> renderer; CAutoRefPtr<IImgDecoderFactory> decoder;
    if (!components.CreateRender_GDI((IObjRef**)&renderer) || !renderer || !components.CreateImgDecoder((IObjRef**)&decoder) || !decoder) return 1;
    renderer->SetImgDecoderFactory(decoder);
    SStringT resourceDirectory = GetExeDirectory() + _T("..\\..\\newDemo\\res\\");
#ifdef _DEBUG
    LPCTSTR systemResource = _T("soui-sys-resourced.dll");
#else
    LPCTSTR systemResource = _T("soui-sys-resource.dll");
#endif
    HMODULE systemModule = LoadLibrary(systemResource); if (!systemModule) return 1;
    int result = 1;
    { SApplication application(renderer, instance); application.SetAppDir(resourceDirectory);
      CAutoRefPtr<IResProvider> sp; CreateResProvider(RES_PE, (IObjRef**)&sp);
      CAutoRefPtr<IResProvider> fp; CreateResProvider(RES_FILE, (IObjRef**)&fp);
      if (sp && sp->Init((WPARAM)systemModule, 0) && fp && fp->Init((WPARAM)(LPCTSTR)resourceDirectory, 0)) {
        application.LoadSystemNamedResource(sp); application.AddResProvider(fp); CMainWnd window; window.Create(NULL);
        if (::IsWindow(window.m_hWnd)) { window.GetNative()->SendMessage(WM_INITDIALOG); window.CenterWindow(); window.ShowWindow(showCommand); result = application.Run(window.m_hWnd); }
      }
    }
    FreeLibrary(systemModule); return result;
}int WINAPI _tWinMain(HINSTANCE instance,HINSTANCE,LPTSTR,int showCommand){HRESULT h=OleInitialize(NULL);if(FAILED(h))return 1;int r=RunDemo(instance,showCommand);OleUninitialize();return r;}





