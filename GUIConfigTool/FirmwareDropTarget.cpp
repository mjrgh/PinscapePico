// Pinscape Pico - Config Tool Boot Loader Drive window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <regex>
#include <Windows.h>
#include <windowsx.h>
#include "FirmwareDropTarget.h"


ULONG STDMETHODCALLTYPE FirmwareDropTarget::AddRef()
{
    return InterlockedIncrement(&refCnt);
}

ULONG STDMETHODCALLTYPE FirmwareDropTarget::Release()
{
    ULONG ret = InterlockedDecrement(&refCnt);
    if (ret == 0) delete this;
    return ret;
}

HRESULT STDMETHODCALLTYPE FirmwareDropTarget::QueryInterface(REFIID riid, void **ppvObj)
{
    if (riid == IID_IDropTarget)
        return *ppvObj = static_cast<IDropTarget*>(this), S_OK;
    else if (riid == IID_IUnknown)
        return *ppvObj = static_cast<IUnknown*>(this), S_OK;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE FirmwareDropTarget::DragEnter(IDataObject *pDataObj, DWORD grfKeyState, POINTL ptl, DWORD *pdwEffect)
{
    // Try getting a simple file drop handle (HDROP).  This is used for Windows
    // desktop shell drag-drop operations.
    STGMEDIUM stg;
    stg.tymed = TYMED_HGLOBAL;
    FORMATETC fmt_hdrop ={ CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    if (SUCCEEDED(pDataObj->GetData(&fmt_hdrop, &stg)))
    {
        // get the file drop handle and drop file count
        HDROP hDrop = reinterpret_cast<HDROP>(stg.hGlobal);
        UINT nFiles = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);

        // if there's one file, get its name
        if (nFiles == 1)
        {
            // get the drag file
            if (UINT len = DragQueryFile(hDrop, 0, NULL, 0); len != 0)
            {
                // allocate space and retrieve the filename
                dragDropFile.resize(++len);
                DragQueryFile(hDrop, 0, dragDropFile.data(), len);

                // make sure it ends in .uf2 - if not, forget the file
                std::basic_regex<TCHAR> pat(_T("^.*\\.uf2$"), std::regex_constants::icase);
                if (!std::regex_match(dragDropFile.data(), pat))
                    dragDropFile.clear();
            }
        }
    }

    // test the location for a valid drop hot spot
    TestDrop(ptl, pdwEffect);
    isDragActive = true;
    return S_OK;
}

bool FirmwareDropTarget::TestDrop(POINTL ptl, DWORD *pdwEffect)
{
    // only proceed if our window is still extant, and we have a valid drag/drop file
    if (win != nullptr && dragDropFile.size() != 0 && win->IsFirmwareDropLocation(ptl))
    {
        // allow it - an UF2 file drop installs the file, which is conceptually a copy
        *pdwEffect = DROPEFFECT_COPY;
        isTargetHot = true;
        return true;
    }

    // can't drop here
    *pdwEffect = DROPEFFECT_NONE;
    isTargetHot = false;
    return false;
}

HRESULT STDMETHODCALLTYPE FirmwareDropTarget::DragLeave()
{
    dragDropFile.clear();
    isTargetHot = false;
    isDragActive = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FirmwareDropTarget::DragOver(DWORD grfKeyState, POINTL ptl, DWORD *pdwEffect)
{
    TestDrop(ptl, pdwEffect);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FirmwareDropTarget::Drop(IDataObject *pDataObj, DWORD grfKeyState, POINTL ptl, DWORD *pdwEffect)
{
    // if the drop is valid, send it to the window for processing
    if (TestDrop(ptl, pdwEffect))
    {
        // install it
        win->ExecFirmwareDrop(ptl, dragDropFile.data());

        // this consumes the drop file - clear the name
        dragDropFile.clear();
        isTargetHot = false;
        isDragActive = false;
    }

    return S_OK;
}

void FirmwareDropTarget::Detach()
{
    this->win = nullptr;
}
