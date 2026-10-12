#define WINVER 0x06010000
#define _WIN32_WINNT 0x06010000
#include <windows.h>
#include <souistd.h>
#include <commgr2.h>
#include <core/SHostWnd.h>
#include <control/SListView.h>
#include <core/SItemPanel.h>
#include <core/SWindowMgr.h>   // SWindowMgr::GetWindow：SWND 句柄 -> SWindow*
#include <control/SCmnCtrl.h>
#include <helper/SAdapterBase.h>
#include <res.mgr/SResProviderMgr.h>
#include <control/STabCtrl.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfidl.h>
#include <propvarutil.h>
#include <cwchar>
#include <cstdarg>
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

// 拖拽诊断日志：OutputDebugString，调试时在 VS「输出」窗口用 [drag] 过滤即可看到
static void DragLog(const wchar_t *fmt, ...) {
    wchar_t buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    wcscat_s(buf, L"\r\n");
    OutputDebugStringW(buf);
}

struct FileItem {
    std::wstring path, name, info, duration;
    ULONGLONG bytes;
    bool selected;
    FileItem() : bytes(0), selected(false) {}
};

class CMainWnd;

// 任务状态
enum TaskState { TS_Waiting = 0, TS_Running, TS_Done, TS_Cancelled };

struct TaskItem {
    int id;                 // 自增唯一标识：拖拽/置顶会改变下标，不能靠下标追踪
    std::wstring path;      // 源文件路径，用于避免同一文件重复排队
    std::wstring name;
    std::wstring detail;
    TaskState state;
    int progress;
    bool pinned;
    TaskItem() : id(0), state(TS_Waiting), progress(0), pinned(false) {}
};

// 任务分组：0=置顶(进行中) 1=普通(进行中) 2=已结束
// 排序时按 rank 稳定排序；拖拽只允许在同组内换位，避免把已完成任务拖到进行中里去。
static int TaskRank(const TaskItem &t) {
    if (t.state == TS_Done || t.state == TS_Cancelled) return 2;
    return t.pinned ? 0 : 1;
}

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

class CTaskAdapter : public SAdapterBase {
    CMainWnd *m_owner;
public:
    explicit CTaskAdapter(CMainWnd *owner) : m_owner(owner) {}
    virtual int getCount();
    virtual ULONG_PTR getItemData(int position) { return (ULONG_PTR)position; }
    virtual void getView(int position, SWindow *pItem, pugi::xml_node xmlTemplate);
    bool OnPin(EventCmd *evt);
    bool OnCancel(EventCmd *evt);
};

class CMainWnd : public SHostWnd {
    friend class CFileAdapter;
    friend class CTaskAdapter;
    std::vector<FileItem> m_files;
    std::vector<TaskItem> m_tasks;
    int m_nextTaskId;
    int m_activeTaskId;      // 当前正在处理的任务 id，0 表示没有
    std::mutex m_mutex;
    std::thread m_worker;
    bool m_closing;
    bool m_processing;
    int m_progress;
    CFileAdapter *m_adapter;
    CTaskAdapter *m_taskAdapter;
    SListView *m_taskList;
    bool m_dragging;         // 是否正在拖拽任务行
    bool m_dragMoved;        // 本次拖拽是否真的改变过顺序
    int m_dragId;            // 被拖拽任务的 id
public:
    CMainWnd() : SHostWnd(_T("LAYOUT:app")), m_nextTaskId(1), m_activeTaskId(0),
                 m_closing(false), m_processing(false), m_progress(0),
                 m_adapter(NULL), m_taskAdapter(NULL), m_taskList(NULL),
                 m_dragging(false), m_dragMoved(false), m_dragId(0) {}
    ~CMainWnd() { StopWorker(); }
    void AddPaths(const std::vector<std::wstring> &paths);
    void RemoveAt(int index);
    void ClearFiles();
    void RefreshList();
    void UpdateSummary();
    void UpdateEmptyState();
    void UpdateTasks();
    void StopWorker();
    void ShowTasksPage();
    bool OnTabSelChanged(EventTabSelChanged *evt);
    bool OnMetadata(FileItem *item);
    const FileItem &Item(int i) const { return m_files[i]; }
    int Count() const { return (int)m_files.size(); }
    // 任务相关
    bool IsDraggingTask(int id) const { return m_dragging && m_dragId == id; }
    const TaskItem &TaskAt(int i) const { return m_tasks[i]; }
    int TaskCount() const { return (int)m_tasks.size(); }
    int IndexOfTask(int id) const;
    void SortTasks();
    void TogglePinTask(int id);
    void CancelTask(int id);
    void BeginTaskDrag(int index);
    void DragTaskTo(int index);
    void EndTaskDrag();
protected:
    void OnClose() {
        if (m_closing) return;
        m_closing = true;
        if (m_processing) { KillTimer(2); m_processing = false; }
        DestroyWindow();
    }
    LRESULT OnInitDialog(HWND, LPARAM);
    LRESULT OnMetadataMessage(UINT, WPARAM, LPARAM);
    void OnStart();
    bool StartNextTask();    // 取出下一个等待中的任务开始处理；没有了返回 false
    void OnTimer(UINT_PTR id);
    void OnAddFiles();
    void OnClearFiles() { ClearFiles(); }
    // 宿主窗口级别的鼠标消息：拖拽过程中鼠标可能移出某一行，
    // 靠 SWindow 事件拿不到连续移动，所以在宿主这一层统一处理。
    void OnHostLButtonDown(UINT nFlags, CPoint pt);
    void OnHostMouseMove(UINT nFlags, CPoint pt);
    void OnHostLButtonUp(UINT nFlags, CPoint pt);
    EVENT_MAP_BEGIN()
        EVENT_ID_COMMAND(1, OnClose)
        EVENT_NAME_COMMAND(L"btn_add_files", OnAddFiles)
        EVENT_NAME_COMMAND(L"btn_add_files_empty", OnAddFiles)
        EVENT_NAME_COMMAND(L"btn_clear_files", OnClearFiles)
        EVENT_NAME_COMMAND(L"btn_start", OnStart)
    EVENT_MAP_END()
    BEGIN_MSG_MAP_EX(CMainWnd)
        MSG_WM_CLOSE(OnClose)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_TIMER(OnTimer)
        MSG_WM_LBUTTONDOWN(OnHostLButtonDown)
        MSG_WM_MOUSEMOVE(OnHostMouseMove)
        MSG_WM_LBUTTONUP(OnHostLButtonUp)
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

int CTaskAdapter::getCount() { return m_owner->TaskCount(); }

void CTaskAdapter::getView(int position, SWindow *pItem, pugi::xml_node xmlTemplate) {
    // 面板是从回收池里复用的，只有第一次才建子控件 + 订阅事件，否则会重复订阅。
    if (pItem->GetChildrenCount() == 0) {
        pItem->InitFromXml(xmlTemplate);
        SWindow *pin = pItem->FindChildByName(_T("btn_pin"));
        if (pin) pin->GetEventSet()->subscribeEvent(EVT_CMD, Subscriber(&CTaskAdapter::OnPin, this));
        SWindow *cancel = pItem->FindChildByName(_T("btn_cancel"));
        if (cancel) cancel->GetEventSet()->subscribeEvent(EVT_CMD, Subscriber(&CTaskAdapter::OnCancel, this));
    }
    // 每次都要刷新：这个面板可能是刚从回收池取出来的，带着上一个位置的数据
    pItem->SetUserData(position);

    const TaskItem &t = m_owner->TaskAt(position);
    bool finished = (t.state == TS_Done || t.state == TS_Cancelled);

    SWindow *name = pItem->FindChildByName(_T("txt_task_name"));
    if (name) name->SetWindowText(t.name.c_str());

    SWindow *state = pItem->FindChildByName(_T("txt_task_state"));
    if (state) {
        SStringT text;
        switch (t.state) {
        case TS_Running:   text.Format(_T("处理中 %d%%"), t.progress); break;
        case TS_Done:      text = t.detail.empty() ? _T("已完成") : t.detail.c_str(); break;
        case TS_Cancelled: text = _T("已取消"); break;
        default:           text = t.pinned ? _T("等待中 · 已置顶") : _T("等待中"); break;
        }
        state->SetWindowText(text);
    }

    SProgress *prog = pItem->FindChildByName2<SProgress>(_T("prog_task"));
    if (prog) { prog->SetVisible(t.state == TS_Running); prog->SetValue(t.progress); }

    SWindow *card = pItem->FindChildByName(_T("task_card"));
    if (card) {
        LPCWSTR skin = L"skin_task_card";
        if (m_owner->IsDraggingTask(t.id)) skin = L"skin_task_dragging";
        else if (t.pinned)                skin = L"skin_task_pinned";
        card->SetAttribute(L"skin", skin);
    }

    SWindow *pin = pItem->FindChildByName(_T("btn_pin"));
    if (pin) { pin->SetVisible(!finished); pin->SetWindowText(t.pinned ? _T("取消置顶") : _T("置顶")); }

    SWindow *cancel = pItem->FindChildByName(_T("btn_cancel"));
    if (cancel) cancel->SetWindowText(finished ? _T("移除") : _T("取消"));
}

static int TaskIdAt(CMainWnd *owner, SWindow *root) {
    if (!root) return 0;
    int pos = (int)root->GetUserData();
    if (pos < 0 || pos >= owner->TaskCount()) return 0;
    return owner->TaskAt(pos).id;
}

bool CTaskAdapter::OnPin(EventCmd *evt) {
    SWindow *s = sobj_cast<SWindow>(evt->sender);
    if (!s) return true;
    int id = TaskIdAt(m_owner, s->GetRoot());
    if (id) m_owner->TogglePinTask(id);
    return true;
}
bool CTaskAdapter::OnCancel(EventCmd *evt) {
    SWindow *s = sobj_cast<SWindow>(evt->sender);
    if (!s) return true;
    int id = TaskIdAt(m_owner, s->GetRoot());
    if (id) m_owner->CancelTask(id);
    return true;
}
void CMainWnd::RefreshList() { if (m_adapter) m_adapter->notifyDataSetChanged(); UpdateSummary(); UpdateEmptyState(); }
void CMainWnd::UpdateTasks() {
    int running=0, waiting=0, done=0, cancelled=0;
    for (size_t i=0;i<m_tasks.size();++i) {
        switch (m_tasks[i].state) {
        case TS_Running:   running++;   break;
        case TS_Done:      done++;      break;
        case TS_Cancelled: cancelled++; break;
        default:           waiting++;   break;
        }
    }
    SWindow *summary = FindChildByName(_T("txt_task_summary"));
    if (summary) summary->SetWindowText(SStringT().Format(
        _T("任务列表  %d 个（进行中 %d · 等待 %d · 完成 %d · 已取消 %d）"),
        (int)m_tasks.size(), running, waiting, done, cancelled));

    bool empty = m_tasks.empty();
    SWindow *emptyTip = FindChildByName(_T("txt_tasks_empty"));
    if (emptyTip) { emptyTip->SetVisible(empty); emptyTip->SetAttribute(L"display", empty ? L"1" : L"0"); }
    SListView *list = m_taskList ? m_taskList : FindChildByName2<SListView>(_T("task_list"));
    if (list) { list->SetVisible(!empty); list->SetAttribute(L"display", empty ? L"0" : L"1"); }

    // 列表不可见时 notifyDataSetChanged 只会打标记，等切到该页再刷（SOUI 的既有行为）
    if (m_taskAdapter) m_taskAdapter->notifyDataSetChanged();
}

int CMainWnd::IndexOfTask(int id) const {
    for (int i=0;i<(int)m_tasks.size();++i) if (m_tasks[i].id == id) return i;
    return -1;
}

void CMainWnd::SortTasks() {
    // 稳定排序：同组内保持原有相对顺序，只把不同 rank 的组分开
    std::stable_sort(m_tasks.begin(), m_tasks.end(),
        [](const TaskItem &a, const TaskItem &b) { return TaskRank(a) < TaskRank(b); });
}

void CMainWnd::TogglePinTask(int id) {
    int idx = IndexOfTask(id);
    if (idx < 0) return;
    m_tasks[idx].pinned = !m_tasks[idx].pinned;
    SortTasks();
    UpdateTasks();
}

void CMainWnd::CancelTask(int id) {
    int idx = IndexOfTask(id);
    if (idx < 0) return;
    TaskState st = m_tasks[idx].state;
    if (st == TS_Running || st == TS_Waiting) {
        m_tasks[idx].state = TS_Cancelled;
        m_tasks[idx].progress = 0;
        if (id == m_activeTaskId) {   // 取消的是正在处理的任务，复位处理流程并自动开始下一个
            m_processing = false;
            m_activeTaskId = 0;
            KillTimer(2);
            SWindow *button = FindChildByName(_T("btn_start"));
            if (button) button->SetWindowText(_T("开始处理"));
            SProgress *progress = FindChildByName2<SProgress>(_T("progress_task"));
            if (progress) progress->SetValue(0);
            SWindow *percent = FindChildByName(_T("txt_progress_percent"));
            if (percent) percent->SetWindowText(_T("0%"));
            StartNextTask();          // 队列里还有等待的就接着处理；没有则保持空闲
        }
    } else {
        m_tasks.erase(m_tasks.begin() + idx);   // 已完成/已取消的按钮是「移除」
    }
    SortTasks();
    UpdateTasks();
}

void CMainWnd::BeginTaskDrag(int index) {
    if (index < 0 || index >= (int)m_tasks.size()) return;
    // 允许拖动所有状态的任务（含已完成/已取消）；
    // 只能同组内换位的约束由 DragTaskTo 里的 TaskRank 检查保证
    m_dragging = true;
    m_dragId = m_tasks[index].id;
    DragLog(L"[drag] BeginTaskDrag idx=%d id=%d", index, m_dragId);
    // 立刻刷一次：让被拖的那一行换成高亮 skin，用户能看到"抓住了"
    if (m_taskAdapter) m_taskAdapter->notifyDataSetChanged();
}

void CMainWnd::DragTaskTo(int index) {
    if (!m_dragging) return;
    int cur = IndexOfTask(m_dragId);
    if (cur < 0 || index < 0 || index >= (int)m_tasks.size() || cur == index) return;
    if (TaskRank(m_tasks[cur]) != TaskRank(m_tasks[index])) return;  // 不允许跨组
    TaskItem t = m_tasks[cur];
    m_tasks.erase(m_tasks.begin() + cur);
    m_tasks.insert(m_tasks.begin() + index, t);
    m_dragMoved = true;
    if (m_taskAdapter) m_taskAdapter->notifyDataSetChanged();
}

void CMainWnd::EndTaskDrag() {
    if (!m_dragging) return;
    m_dragging = false;
    m_dragMoved = false;
    m_dragId = 0;
    DragLog(L"[drag] EndTaskDrag");
    UpdateTasks();   // 无论如何都要刷，否则被拖行会一直保持高亮 skin
}

// 拖拽起手：不能订阅 item panel 的 EventLButtonDown —— 子控件的事件会被
// SItemPanel::OnFireEvent(SItemPanel.cpp:116) 包装成 EventOfPanel 直接转发给 listview，
// 根本不经过 panel 自己的 evtSet，订阅永远不会触发。所以在宿主这一层自己命中测试。
//
// 坐标系结论（源码证据）：
//  - LayoutChildren(SouiLayout.cpp:811) 给子窗口赋 rc 时 OffsetRect(rcParent.TopLeft())，
//    rcParent 是父窗口的 GetChildrenLayoutRect()（宿主坐标系）→ **每个窗口的 m_rcWindow
//    都是宿主客户区坐标系下的绝对坐标**，不是相对父窗口；
//  - SWindow::SwndFromPoint 递归时传同一个 pt（Swnd.cpp:890）、IsContainPoint 直接拿
//    m_rcWindow.PtInRect(pt)（Swnd.cpp:2783），也印证所有窗口共用一个坐标系；
//  - SListView::OnItemGetRect(SListView.cpp:461) 的 item rect = listview 的 GetClientRect()
//    + 滚动偏移 → 也是宿主坐标系。
// 所以 HitTest / SwndFromPoint 都直接吃宿主 WM_LBUTTONDOWN 的 pt，不要做任何转换！
// （之前加 HostToLocal 逐层累减是错的——把绝对坐标减成了负数，HitTest 永远 miss。）
void CMainWnd::OnHostLButtonDown(UINT, CPoint pt) {
    DragLog(L"[drag] LBD pt=(%d,%d)", pt.x, pt.y);
    if (m_taskList && m_taskList->IsVisible(TRUE)) {   // 注意 IsVisible 默认是 FALSE，必须传 TRUE 才检查父窗口
        CPoint ptHit = pt;                 // HitTest 会改写传入的 pt（减成面板内坐标），留原 pt 给 SwndFromPoint
        SItemPanel *panel = m_taskList->HitTest(ptHit);
        DragLog(L"[drag] HitTest -> %p", (void *)panel);
        if (panel) {
            // 点在两个行内按钮上时不抢，交给按钮自己的 EVT_CMD。
            // SwndFromPoint 与 HitTest 同坐标系（宿主客户区），吃原始 pt。
            // 返回值是 SWND 句柄（DWORD），要用 SWindowMgr::GetWindow 转回 SWindow*。
            SWindow *hit = SWindowMgr::GetWindow(panel->SwndFromPoint(pt, FALSE));
            DragLog(L"[drag] hit=%s", (hit && hit->GetName()) ? hit->GetName() : L"(null)");
            bool onButton = false;
            for (SWindow *s = hit; s && s != panel; s = s->GetParent()) {
                LPCWSTR nm = s->GetName();
                if (nm && (wcscmp(nm, L"btn_pin") == 0 || wcscmp(nm, L"btn_cancel") == 0)) { onButton = true; break; }
            }
            DragLog(L"[drag] onButton=%d", onButton ? 1 : 0);
            if (!onButton) BeginTaskDrag((int)panel->GetUserData());
        }
    } else {
        DragLog(L"[drag] skip: list=%p", (void *)m_taskList);
    }
    SetMsgHandled(FALSE);   // 继续交给 SHostWnd 正常分发
}

void CMainWnd::OnHostMouseMove(UINT, CPoint pt) {
    if (m_dragging) {
        // 鼠标在窗口外松开时收不到 WM_LBUTTONUP，用按键状态兜底结束拖拽
        if (!(GetKeyState(VK_LBUTTON) & 0x8000)) { EndTaskDrag(); }
        else if (m_taskList) {
            CPoint ptHit = pt;
            SItemPanel *panel = m_taskList->HitTest(ptHit);
            if (panel) {
                DragLog(L"[drag] move pt=(%d,%d) -> row %d", pt.x, pt.y, (int)panel->GetUserData());
                DragTaskTo((int)panel->GetUserData());
            }
        }
    }
    SetMsgHandled(FALSE);   // 继续交给 SHostWnd 正常分发
}

void CMainWnd::OnHostLButtonUp(UINT, CPoint) {
    if (m_dragging) EndTaskDrag();
    SetMsgHandled(FALSE);
}
void CMainWnd::UpdateSummary() { ULONGLONG total = 0; for (size_t i=0;i<m_files.size();++i) total += m_files[i].bytes; SWindow *s=FindChildByName(_T("txt_file_summary")); if(s) s->SetWindowText(SStringT().Format(_T("%d 个 · %s"),(int)m_files.size(),FormatSize(total).c_str())); SWindow *preview=FindChildByName(_T("source_preview_info")); SWindow *output=FindChildByName(_T("output_preview_info")); if(preview) preview->SetWindowText(m_files.empty() ? _T("等待加载视频") : SStringT().Format(_T("%s\n%s"),m_files[0].name.c_str(),m_files[0].info.c_str())); if(output) output->SetWindowText(m_files.empty() ? _T("处理后的视频将在这里显示") : _T("等待模拟处理")); }
void CMainWnd::UpdateEmptyState() {
    // visible 控制显示，display 控制隐藏时是否占位（OnAttrDisplay 运行时会触发重排）。
    // 两者互斥切换，避免隐藏的列表把空态挤到半格。
    SWindow *empty=FindChildByName(_T("empty_state"));
    SWindow *list=FindChildByName(_T("file_list"));
    bool isEmpty=m_files.empty();
    if(empty){ empty->SetVisible(isEmpty); empty->SetAttribute(L"display", isEmpty?L"1":L"0"); }
    if(list){ list->SetVisible(!isEmpty); list->SetAttribute(L"display", isEmpty?L"0":L"1"); }
}
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
LRESULT CMainWnd::OnInitDialog(HWND, LPARAM) { SListView *list=FindChildByName2<SListView>(_T("file_list")); if(list){m_adapter=new CFileAdapter(this); list->SetAdapter(m_adapter); m_adapter->Release();} SListView *tasks=FindChildByName2<SListView>(_T("task_list")); if(tasks){m_taskList=tasks; m_taskAdapter=new CTaskAdapter(this); tasks->SetAdapter(m_taskAdapter); m_taskAdapter->Release();} SProgress *progress=FindChildByName2<SProgress>(_T("progress_task")); if(progress){progress->SetRange(0,100);progress->SetValue(0);} STabCtrl *tab=FindChildByName2<STabCtrl>(_T("nav_tab")); if(tab) tab->GetEventSet()->subscribeEvent(EventTabSelChanged::EventID, Subscriber(&CMainWnd::OnTabSelChanged, this)); UpdateSummary(); UpdateEmptyState(); UpdateTasks(); return 0; }
// 把所有还没有排队/在处理的文件都生成任务（按路径去重），然后依次处理
void CMainWnd::OnStart() {
    if(m_processing) return;
    int created = 0;
    for (int i = 0; i < Count(); ++i) {
        bool queued = false;
        for (size_t j = 0; j < m_tasks.size(); ++j) {
            if (m_tasks[j].path == m_files[i].path &&
                (m_tasks[j].state == TS_Waiting || m_tasks[j].state == TS_Running)) { queued = true; break; }
        }
        if (queued) continue;
        TaskItem task;
        task.id = m_nextTaskId++;
        task.path = m_files[i].path;
        task.name = m_files[i].name;
        task.state = TS_Waiting;
        task.progress = 0;
        m_tasks.push_back(task);
        created++;
    }
    SWindow *status = FindChildByName(_T("txt_status"));
    if (!created) {
        if (status) status->SetWindowText(Count() ? _T("所选文件都已在任务列表中") : _T("请先添加视频文件"));
        return;
    }
    SortTasks();
    UpdateTasks();
    StartNextTask();
    ShowTasksPage();   // 建完任务顺手切到任务列表页，不用手动点页签
}

// 从任务队列里取下一个等待中的任务开始处理；全部处理完返回 false
bool CMainWnd::StartNextTask() {
    int next = -1;
    for (size_t i = 0; i < m_tasks.size(); ++i)
        if (m_tasks[i].state == TS_Waiting) { next = (int)i; break; }
    if (next < 0) return false;
    m_tasks[next].state = TS_Running;
    m_tasks[next].progress = 0;
    m_activeTaskId = m_tasks[next].id;
    m_processing = true;
    m_progress = 0;
    SWindow *button = FindChildByName(_T("btn_start"));
    if (button) button->SetWindowText(_T("处理中…"));
    SWindow *status = FindChildByName(_T("txt_status"));
    if (status) status->SetWindowText(SStringT().Format(_T("正在处理：%s"), m_tasks[next].name.c_str()));
    SProgress *progress = FindChildByName2<SProgress>(_T("progress_task"));
    if (progress) progress->SetValue(0);
    SWindow *percent = FindChildByName(_T("txt_progress_percent"));
    if (percent) percent->SetWindowText(_T("0%"));
    SetTimer(2, 100);
    UpdateTasks();
    return true;
}
void CMainWnd::OnTimer(UINT_PTR id) { if(id!=2) return; if(!m_processing) { KillTimer(2); return; } m_progress+=5; SProgress *progress=FindChildByName2<SProgress>(_T("progress_task")); if(progress) progress->SetValue(m_progress); SWindow *percent=FindChildByName(_T("txt_progress_percent")); if(percent) percent->SetWindowText(SStringT().Format(_T("%d%%"),m_progress)); SWindow *status=FindChildByName(_T("txt_status")); if(status) status->SetWindowText(SStringT().Format(_T("模拟 FFmpeg 处理中 %d%%"),m_progress));
 { int idx=IndexOfTask(m_activeTaskId); if(idx>=0) { m_tasks[idx].progress=m_progress; if(m_taskAdapter) m_taskAdapter->notifyItemDataChanged(idx); } }
 if(m_progress>=100) { int idx=IndexOfTask(m_activeTaskId); if(idx>=0) { m_tasks[idx].state=TS_Done; m_tasks[idx].progress=100; m_tasks[idx].detail=L"已完成（模拟）"; } m_activeTaskId=0; SortTasks(); UpdateTasks(); if(!StartNextTask()) { m_processing=false; KillTimer(2); SWindow *button=FindChildByName(_T("btn_start")); if(button) button->SetWindowText(_T("开始处理")); if(status) status->SetWindowText(_T("全部处理完成（模拟）")); SWindow *output=FindChildByName(_T("output_preview_info")); if(output) output->SetWindowText(_T("输出视频：模拟转换完成")); } } }
void CMainWnd::OnAddFiles() { IFileOpenDialog *dialog=NULL; if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return; DWORD options=0; dialog->GetOptions(&options); dialog->SetOptions(options|FOS_ALLOWMULTISELECT|FOS_FILEMUSTEXIST); COMDLG_FILTERSPEC filters[]={{L"视频文件",L"*.mp4;*.mkv;*.webm;*.avi;*.mov;*.wmv"},{L"所有文件",L"*.*"}}; dialog->SetFileTypes(2,filters); if(SUCCEEDED(dialog->Show(m_hWnd))){ IShellItemArray *items=NULL; if(SUCCEEDED(dialog->GetResults(&items))){ DWORD count=0; items->GetCount(&count); std::vector<std::wstring> paths; for(DWORD i=0;i<count;++i){IShellItem *item=NULL; PWSTR p=NULL; if(SUCCEEDED(items->GetItemAt(i,&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p))){paths.push_back(p);CoTaskMemFree(p);} if(item)item->Release();} AddPaths(paths); items->Release(); }} dialog->Release(); }

// 切到任务列表页（tab 的第 2 页，索引 1）
void CMainWnd::ShowTasksPage() { STabCtrl *tab=FindChildByName2<STabCtrl>(_T("nav_tab")); if(tab) tab->SetCurSel(1); UpdateTasks(); }

// 页签切换时刷新任务列表（返回 false 表示事件继续派发）
bool CMainWnd::OnTabSelChanged(EventTabSelChanged *evt) { UpdateTasks(); return false; }
static SStringT GetExeDirectory(){TCHAR path[MAX_PATH]={0};GetModuleFileName(NULL,path,MAX_PATH);SStringT d(path);return d.Left(d.ReverseFind(_T('\\'))+1);}
static int RunDemo(HINSTANCE instance, int showCommand)
{
    SComMgr2 components(_T("imgdecoder-png"));
    CAutoRefPtr<IRenderFactory> renderer; CAutoRefPtr<IImgDecoderFactory> decoder;
    if (!components.CreateRender_GDI((IObjRef**)&renderer) || !renderer || !components.CreateImgDecoder((IObjRef**)&decoder) || !decoder) return 1;
    renderer->SetImgDecoderFactory(decoder);
    SStringT resourceDirectory = GetExeDirectory() + _T("..\\..\\souiDemo\\res\\");
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





