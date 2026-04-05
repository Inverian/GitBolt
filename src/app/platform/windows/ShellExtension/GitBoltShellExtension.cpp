// Windows Shell Extension for GitBolt
// Adds "Open in GitBolt" to Windows Explorer context menu
// Build as a COM DLL, register with regsvr32

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <string>

// {GUID} for GitBolt shell extension
// {8A5B7E3C-4F2D-4A1B-9C6E-3D8F7A2B1C4E}
static const CLSID CLSID_GitBoltShellExt =
    {0x8A5B7E3C, 0x4F2D, 0x4A1B, {0x9C, 0x6E, 0x3D, 0x8F, 0x7A, 0x2B, 0x1C, 0x4E}};

class GitBoltContextMenu : public IShellExtInit, public IContextMenu {
    LONG refCount_ = 1;
    std::wstring selectedPath_;
public:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override;

    // IShellExtInit
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject* pdtobj, HKEY hkeyProgID) override;

    // IContextMenu
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags) override;
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* pici) override;
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR idCmd, UINT uType, UINT* pReserved, CHAR* pszName, UINT cchMax) override;
};

// Implement just enough to show "Open in GitBolt" in Explorer context menu
// Full implementation deferred to Windows build environment

#endif // _WIN32
