// Pinscape Pico - Config Editor Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY


#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <list>
#include <map>
#include <iterator>
#include <memory>
#include <algorithm>
#include <regex>
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <tchar.h>
#include "../Scintilla/include/Scintilla.h"
#include "../Scintilla/include/ILexer.h"
#include "../Firmware/JSON.h"
#include "PinscapePicoAPI.h"
#include "JSONLexer/LexJSON.h"
#include "Dialog.h"
#include "Application.h"
#include "WinUtil.h"
#include "Utilities.h"
#include "ConfigEditorWin.h"
#include "resource.h"

// this is all in the PinscapePico namespace
using namespace PinscapePico;

// --------------------------------------------------------------------------
//
// Build the compiled schema tree.  Wrap it in a namespace, since it
// contains a large number of public variable declarations that are only
// meaningful within the tree.
//
namespace PinscapePicoJSONSchema {
#include "Generated/ConfigSchema.h"
}

// We only need to directly access the root schema object.  All other
// references are from traversing the tree from the root.  Create an
// alias to the root object in our root namespace, purely for brevity.
static const auto *jsonSchemaRoot = PinscapePicoJSONSchema::jsonSchemaRoot;

// find a property in the schema tree
const JSONSchemaObj::Prop *JSONSchemaObj::FindProp(const char *name, const JSONParserExt::Value *val) const
{
    // strip quotes if present
    if (*name == '"')
        ++name;

    // search the property list
    for (const auto &prop : props)
    {
        if (strcmp(prop.name, name) == 0)
            return &prop;
    }

    // Not found.  If we have a subclass property, try to identify the subclass.
    if (subclassIdPropName != nullptr && val != nullptr)
    {
        auto const *subclassIdVal = val->Get(subclassIdPropName);
        if (!subclassIdVal->IsUndefined())
        {
            std::string subclassId = subclassIdVal->String();
            for (auto *scObj : subclassObjects)
            {
                if (scObj->subclassIdPropVal != nullptr && subclassId == scObj->subclassIdPropVal)
                {
                    // this is the one - search this subclass for a match
                    return scObj->FindProp(name, val);
                }
            }
        }
    }

    // not found
    return nullptr;
}

// iterate properties
void JSONSchemaObj::ForEachProp(std::function<void(const Prop*)> func, const JSONParserExt::Value *val) const
{
    // search the property list
    for (const auto &prop : props)
        func(&prop);

    // if we have a subclass property, try to identify the subclass
    if (subclassIdPropName != nullptr && val != nullptr)
    {
        auto const *subclassIdVal = val->Get(subclassIdPropName);
        if (!subclassIdVal->IsUndefined())
        {
            std::string subclassId = subclassIdVal->String();
            for (auto *scObj : subclassObjects)
            {
                if (scObj->subclassIdPropVal != nullptr && subclassId == scObj->subclassIdPropVal)
                {
                    // this is the one - iterate over its properties
                    scObj->ForEachProp(func, val);
                }
            }
        }
    }
}

const JSONSchemaObj *JSONSchemaObj::Prop::GetSubObj() const
{
    // use the direct sub-object reference if available
    if (subObj != nullptr)
        return subObj;

    // check for a cross-reference object
    if (xrefObj != nullptr)
        return xrefObj;

    // no sub-object found
    return nullptr;
}

bool JSONSchemaObj::Prop::IsBool() const
{
    return types.size() == 1 && types.front().name != nullptr && strcmp(types.front().name, "boolean") == 0;
}


// --------------------------------------------------------------------------
//
// Config editor window
//

// Scintilla indicator assignments
static const int INDICATOR_FIND = INDICATOR_CONTAINER + 0;

// Custom messages
static const UINT MSG_FINDREPLACEDLG = WM_USER + 601;  // Find/Replace button click; WPARAM = button ID
static const UINT MSG_OFFERTEMPLATE = WM_USER + 602;   // offer to load a template file

// statics
ConfigEditorWin::Options ConfigEditorWin::options;

// construction
ConfigEditorWin::ConfigEditorWin(HINSTANCE hInstance, 
    std::shared_ptr<VendorInterface::Shared> &device, uint8_t configType) :
	BaseDeviceWindow(hInstance, device), 
    configType(configType)
{
    // load bitmaps
    bmpCtlBarButtons = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_EDITTOOLS));
    BITMAP bmp;
    GetObject(bmpCtlBarButtons, sizeof(bmp), &bmp);
    szCtlBarButtons ={ bmp.bmWidth, bmp.bmHeight };

    // populate the control bar button list
    const static CtlBarButton buttons[]
    {
        { ID_FILE_PROGRAMPICO, 2, "Program Pico", "Save file to Pico flash and apply new settings" },
        { -1 }, // spacer
        { ID_EDIT_UNDO, 4, nullptr, "Undo" },
        { ID_EDIT_REDO, 5, nullptr, "Redo" },
        { -1 }, // spacer
        { -2, -2, nullptr, "Quick find" }, // Find box
        { ID_EDIT_FINDNEXT, 0, nullptr, "Find Next" },
        { -1 }, // spacer
        { ID_EDIT_FINDREPLACE, 3, nullptr, "Find-and-Replace text, and more search options" },
    };
    for (auto &b : buttons)
        ctlBarButtons.emplace_back(b);

    // add the nav drop button
    navDropButton = &ctlBarButtons.emplace_back(CtlBarButton{ ID_NAV_DROP_BUTTON, 6, nullptr, "Navigation", true });
}

// destruction
ConfigEditorWin::~ConfigEditorWin()
{
    // clean up resources
    DeleteFont(editorFont);
}

void ConfigEditorWin::OnCreateWindow()
{
    // do the base class work
    __super::OnCreateWindow();

    // Load menus and accelerators
    hMenuBar = LoadMenu(hInstance, MAKEINTRESOURCE(ID_MENU_CONFIGEDITWIN));
    hCtxMenu = LoadMenu(hInstance, MAKEINTRESOURCE(ID_CTXMENU_CONFIGEDITWIN));

    // load fonts
    const char *editorFontName = "Courier New";
    {
        // get the window DC
        HDCHelper hdc(hwnd);

        // create the fonts
        editorFont = CreateFontA(
            -MulDiv(10, GetDeviceCaps(hdc, LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
            editorFontName);
        toolWindowTitleFont = CreateFontA(
            cyErrorTitle, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_ROMAN,
            "Segoe UI");

        // get metrics
        HFONT oldFont = SelectFont(hdc, mainFont);
        GetTextMetrics(hdc, &tmEditorFont);
        SelectFont(hdc, toolWindowTitleFont);
        GetTextMetrics(hdc, &tmToolWindowTitleFont);
        SelectFont(hdc, oldFont);
    }

    // figure the height of the control bar and error panel
    cyControlBar = max(boldFontMetrics.tmHeight + 6, szCtlBarButtons.cy);
    cyErrorPanel = gApp.settingsJSON.Get("editor.errorPanel.cy")->Int(mainFontMetrics.tmHeight * 10);

    // create the find box: single-line edit control with a cue banner saying "Find..."
    findBox = CreateControl(ID_EDIT_FINDBOX, WC_EDITA, "", 
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        mainFontMetrics.tmAveCharWidth*20, mainFontMetrics.tmHeight + 2);
    SendMessage(findBox, WM_SETFONT, reinterpret_cast<WPARAM>(mainFont), MAKELPARAM(FALSE, 0));
    SendMessage(findBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find..."));

    // create the error box: read-only multi-line edit, initially not visible
    errorPanel = CreateControl(ID_EDIT_ERRORS, WC_EDITA, "",
        WS_CHILD | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL,
        100, 20);
    ShowWindow(errorPanel, SW_HIDE);

    // if we haven't loaded Scintilla yet, do so now
	static bool scintillaLoaded = false;
	if (!scintillaLoaded)
	{
        // only attempt this once (even if it fails, since it will presumably just 
        // fail again in the same way on repeats - the usual problem is that the
        // DLL file is missing)
        scintillaLoaded = true;
        if (LoadLibraryA("Scintilla.dll") == NULL)
		{
			MessageBoxFmt(hwnd, "An error occurred trying to load the Scintilla text editing component DLL. "
				"You might need to reinstall the Config Tool application files. "
				"Editing configuration files won't be possible during this session.");
		}
	}

    // create the scintilla window
    RECT crc;
    GetClientRect(hwnd, &crc);
    sciWin = CreateWindowExA(0, "Scintilla", "Config Editor",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
        crc.left, crc.top, crc.right - crc.left, crc.bottom - crc.top,
        hwnd, reinterpret_cast<HMENU>(ID_SCINTILLA), hInstance, NULL);

    // if that succeeded, configure the editor
    if (sciWin != NULL)
    {
        // get the direct-call functions
        sciFunc = reinterpret_cast<ScintillaFunc_t*>(SendMessage(sciWin, SCI_GETDIRECTFUNCTION, 0, 0));
        sciFuncCtx = reinterpret_cast<void*>(SendMessage(sciWin, SCI_GETDIRECTPOINTER, 0, 0));

        // turn on line numbering
        CallSci(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
        CallSci(SCI_SETMARGINWIDTHN, 0, CallSci(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<INT_PTR>("_99999")));

        // set the default font for all styles
        CallSci(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<INT_PTR>(editorFontName));
        CallSci(SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
        CallSci(SCI_STYLECLEARALL);

        // set the lexer
        auto *jsonLexer = new LexerJSON();
        CallSci(SCI_SETILEXER, 0, reinterpret_cast<INT_PTR>(jsonLexer));

        // mouse dwell
        CallSci(SCI_SETMOUSEDWELLTIME, 500);

        // folding 
        CallSci(SCI_SETPROPERTY, reinterpret_cast<INT_PTR>("fold"), reinterpret_cast<INT_PTR>("1"));
        CallSci(SCI_SETMARGINWIDTHN, 1, 16);
        CallSci(SCI_SETMARGINMASKN, 1, SC_MASK_FOLDERS);
        CallSci(SCI_SETMARGINSENSITIVEN, 1, TRUE);
        CallSci(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_MINUS);
        CallSci(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_PLUS);
        CallSci(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_EMPTY);
        CallSci(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_EMPTY);
        CallSci(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_EMPTY);
        CallSci(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_EMPTY);
        CallSci(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_EMPTY);
        CallSci(SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED);
        CallSci(SCI_SETELEMENTCOLOUR, SC_ELEMENT_FOLD_LINE, 0xFF000000 | HRGB(0xf0f0f0));
        CallSci(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);

        // set up coloring
        CallSci(SCI_STYLESETFORE, SCE_JSON_LINECOMMENT, HRGB(0x008800));
        CallSci(SCI_STYLESETFORE, SCE_JSON_BLOCKCOMMENT, HRGB(0x008800));
        CallSci(SCI_STYLESETFORE, SCE_JSON_KEYWORD, HRGB(0x000000));
        CallSci(SCI_STYLESETFORE, SCE_JSON_LDKEYWORD, HRGB(0x0000FF));
        CallSci(SCI_STYLESETFORE, SCE_JSON_NUMBER, HRGB(0xC000FF));
        CallSci(SCI_STYLESETFORE, SCE_JSON_HEXNUMBER, HRGB(0xC000FF));
        CallSci(SCI_STYLESETFORE, SCE_JSON_ERROR, HRGB(0xD0B000));
        CallSci(SCI_STYLESETFORE, SCE_JSON_STRING, HRGB(0xC00000));
        CallSci(SCI_STYLESETFORE, SCE_JSON_STRINGEOL, HRGB(0xFF0000));
        CallSci(SCI_STYLESETFORE, SCE_JSON_PROPERTYNAME, HRGB(0x0000D0));
        CallSci(SCI_STYLESETFORE, SCE_JSON_NAKEDPROPERTYNAME, HRGB(0x0000D0));
        CallSci(SCI_STYLESETFORE, SCE_JSON_URI, HRGB(0x0000FF));
        CallSci(SCI_STYLESETFORE, SCE_JSON_COMPACTIRI, HRGB(0x0000FF));
        CallSci(SCI_SETKEYWORDS, 0, reinterpret_cast<INT_PTR>("true false undefined null"));
        CallSci(SCI_STYLESETBACK, STYLE_BRACELIGHT, HRGB(0xF0D0FF));
        CallSci(SCI_STYLESETBACK, STYLE_BRACEBAD, HRGB(0xFF4040));
        CallSci(SCI_INDICSETSTYLE, INDICATOR_FIND, INDIC_ROUNDBOX);
        
        // auto-complete
        CallSci(SCI_AUTOCSETAUTOHIDE, FALSE);

        // Set all of the Scintilla mappings for control keys to null commands.
        // Scintilla normally maps the control keys to insert literal control
        // characters into the buffer, which isn't at all desirable with JSON
        // source material.  Disable this behavior by setting all of these
        // keys to SCI_NULL.  
        for (char ch = 'A' ; ch <= 'Z' ; ++ch)
        {
            CallSci(SCI_CLEARCMDKEY, (SCMOD_CTRL << 16) | ch);
            CallSci(SCI_ASSIGNCMDKEY, (SCMOD_CTRL << 16), SCI_NULL);
        }

        // load options from the JSON config, and apply the new settings
        options.Load();
        ApplyOptions();

        // disable the Scintilla native context menu, so that we can display our custom menu own instead
        CallSci(SCI_USEPOPUP, SC_POPUP_NEVER);

        // load the initial configuration text; this marks the start
        // of the undo log
        LoadFromDevice(true);
    }
}

void ConfigEditorWin::OnDestroy()
{
    // destroy the Find/Replace dialog if present
    if (findReplaceDlg != nullptr)
        DestroyWindow(findReplaceDlg->GetHWND());

    // inherit the default handling
    __super::OnDestroy();
}

// set Emacs or standard Windows mode for key bindings
void ConfigEditorWin::SetEmacsMode(bool emacsMode)
{
    // set the key bindings
    if (emacsMode)
    {
        // emacs key bindings
        static const KeyBinding emacsKeys[]{
            { "Ctrl+A", SCI_HOME },
            { "Ctrl+B", SCI_CHARLEFT },
            { "Ctrl+C", 0, ID_EDIT_COPY },
            { "Ctrl+D", SCI_CLEAR },
            { "Ctrl+E", SCI_LINEEND },
            { "Ctrl+F", SCI_CHARRIGHT },
            { "Ctrl+G", 0, ID_EDIT_CANCELMODE },
            { "Ctrl+K", SCI_DELLINERIGHT },
            { "Ctrl+L", SCI_VERTICALCENTRECARET },
            { "Ctrl+N", SCI_LINEDOWN },
            { "Ctrl+O", 0, ID_EDIT_OPENLINE },
            { "Ctrl+P", SCI_LINEUP },
            { "Ctrl+Q", 0, ID_EDIT_QUOTECHAR },
            { "Ctrl+R", 0, ID_EDIT_REVFIND },
            { "Ctrl+S", 0, ID_EDIT_FIND },
            { "Ctrl+T", 0, ID_EDIT_TRANSPOSECHAR },
            { "Ctrl+V", SCI_PAGEDOWN },
            { "Ctrl+W", 0, ID_EDIT_CUT },
            { "Ctrl+Y", 0, ID_EDIT_PASTE },
            { "Ctrl+Z", SCI_LINESCROLLDOWN },
            { "Ctrl+Space", 0, ID_EDIT_SETANCHOR },
            { "Ctrl+_", 0, ID_EDIT_GOTOLINENUMBER },
            { "Ctrl+[", 0, ID_FILE_CHECKERRORS },

            { "F1", 0, ID_HELP_HELP },
            { "F3", 0, ID_EDIT_FINDNEXT },
            { "F12", 0, ID_FILE_SAVETOFILE },
            { "Shift+F3", 0, ID_EDIT_FINDPREV },

            { "Ctrl+F9", 0, ID_EDIT_UNDO },
            { "Ctrl+F10", 0, ID_EDIT_REDO },

            { "Alt+V", SCI_PAGEUP },
            { "Alt+W", 0, ID_EDIT_COPY },
            { "Alt+Z", SCI_LINESCROLLUP },
            { "Alt+%", 0, ID_EDIT_FINDREPLACE },
            { "Alt+>", SCI_DOCUMENTEND },
            { "Alt+<", SCI_DOCUMENTSTART },

            { "Esc V", SCI_PAGEUP },
            { "Esc W", 0, ID_EDIT_COPY },
            { "Esc Z", SCI_LINESCROLLUP },
            { "Esc %", 0, ID_EDIT_FINDREPLACE },
            { "Esc >", SCI_DOCUMENTEND },
            { "Esc <", SCI_DOCUMENTSTART },

            { "Ctrl+X G", 0, ID_EDIT_GOTOLINENUMBER },
            { "Ctrl+X H", 0, ID_EDIT_SELECTALL },
            { "Ctrl+X R", 0, ID_EDIT_REDO },
            { "Ctrl+X U", 0, ID_EDIT_UNDO },
            { "Ctrl+X Ctrl+F", 0, ID_FILE_IMPORTFROMFILE },
            { "Ctrl+X Ctrl+R", 0, ID_FILE_RELOADFROMPICO },
            { "Ctrl+X Ctrl+S", 0, ID_FILE_PROGRAMPICO },
            { "Ctrl+X Ctrl+W", 0, ID_EDIT_SAVETOFILE },
            { "Ctrl+X Ctrl+X", 0, ID_EDIT_SWAPANCHOR },

            { "Ctrl+Shift+O", 0, ID_FILE_IMPORTFROMFILE },
        };
        InstallKeyBinding(emacsKeys, _countof(emacsKeys));
    }
    else
    {
        // standard Windows-style key bindings
        static const KeyBinding stdKeys[]{
            { "Ctrl+A", 0, ID_EDIT_SELECTALL },
            { "Ctrl+C", 0, ID_EDIT_COPY },
            { "Ctrl+F", 0, ID_EDIT_FIND },
            { "Ctrl+O", 0, ID_FILE_RELOADFROMPICO },
            { "Ctrl+Shift+O", 0, ID_FILE_IMPORTFROMFILE },
            { "Ctrl+R", 0, ID_EDIT_FINDREPLACE },
            { "Ctrl+S", 0, ID_FILE_PROGRAMPICO },
            { "Ctrl+V", 0, ID_EDIT_PASTE },
            { "Ctrl+X", 0, ID_EDIT_CUT },
            { "Ctrl+Y", 0, ID_EDIT_REDO },
            { "Ctrl+Z", 0, ID_EDIT_UNDO },
            { "Ctrl+_", 0, ID_EDIT_GOTOLINENUMBER },
            { "Ctrl+[", 0, ID_FILE_CHECKERRORS },
            { "F1", 0, ID_HELP_HELP },
            { "F3", 0, ID_EDIT_FINDNEXT },
            { "F12", 0, ID_FILE_SAVETOFILE },
        };
        InstallKeyBinding(stdKeys, _countof(stdKeys));
    }
}

static void OutputDebugStringFmtA(const char *fmt, ...)
{
    char buf[256];
    va_list va;
    va_start(va, fmt);
    sprintf_s(buf, fmt, va);
    va_end(va);
    OutputDebugStringA(buf);
}

const std::unordered_map<std::string_view, UINT> ConfigEditorWin::vkeyMap
{
    { "Backspace", VK_BACK },
    { "Tab", VK_TAB },
    { "Clear", VK_CLEAR },
    { "Enter", VK_RETURN },
    { "Shift", VK_SHIFT },
    { "Ctrl", VK_CONTROL },
    { "Pause", VK_PAUSE },
    { "Caps Lock", VK_CAPITAL },
    { "Esc", VK_ESCAPE },
    { "Space", VK_SPACE },
    { "PageUp", VK_PRIOR },
    { "PageDown", VK_NEXT },
    { "End", VK_END },
    { "Home", VK_HOME },
    { "Left", VK_LEFT },
    { "Up", VK_UP },
    { "Right", VK_RIGHT },
    { "Down", VK_DOWN },
    { "Select", VK_SELECT },
    { "Print", VK_PRINT },
    { "Execute", VK_EXECUTE },
    { "PrtScrn", VK_SNAPSHOT },
    { "Ins", VK_INSERT },
    { "Del", VK_DELETE },
    { "Help", VK_HELP },
    { "0", 0x30 },
    { "1", 0x31	},
    { "2", 0x32	},
    { "3", 0x33	},
    { "4", 0x34	},
    { "5", 0x35	},
    { "6", 0x36	},
    { "7", 0x37	},
    { "8", 0x38	},
    { "9", 0x39	},
    { ")", 0x30 | SHIFT },
    { "!", 0x31	| SHIFT },
    { "@", 0x32 | SHIFT	},
    { "#", 0x33	| SHIFT },
    { "$", 0x34	| SHIFT },
    { "%", 0x35	| SHIFT },
    { "^", 0x36	| SHIFT },
    { "&", 0x37	| SHIFT },
    { "*", 0x38	| SHIFT },
    { "(", 0x39	| SHIFT },
    { "A", 0x41	},
    { "B", 0x42	},
    { "C", 0x43	},
    { "D", 0x44	},
    { "E", 0x45	},
    { "F", 0x46	},
    { "G", 0x47	},
    { "H", 0x48	},
    { "I", 0x49	},
    { "J", 0x4A	},
    { "K", 0x4B	},
    { "L", 0x4C	},
    { "M", 0x4D	},
    { "N", 0x4E	},
    { "O", 0x4F	},
    { "P", 0x50	},
    { "Q", 0x51	},
    { "R", 0x52	},
    { "S", 0x53	},
    { "T", 0x54	},
    { "U", 0x55	},
    { "V", 0x56	},
    { "W", 0x57	},
    { "X", 0x58	},
    { "Y", 0x59	},
    { "Z", 0x5A	},
    { "Applications", VK_APPS },
    { "Sleep", VK_SLEEP },
    { "Keypad 0", VK_NUMPAD0 },
    { "Keypad 1", VK_NUMPAD1 },
    { "Keypad 2", VK_NUMPAD2 },
    { "Keypad 3", VK_NUMPAD3 },
    { "Keypad 4", VK_NUMPAD4 },
    { "Keypad 5", VK_NUMPAD5 },
    { "Keypad 6", VK_NUMPAD6 },
    { "Keypad 7", VK_NUMPAD7 },
    { "Keypad 8", VK_NUMPAD8 },
    { "Keypad 9", VK_NUMPAD9 },
    { "Multiply", VK_MULTIPLY },
    { "Add", VK_ADD },
    { "Separator", VK_SEPARATOR },
    { "Subtract", VK_SUBTRACT },
    { "Decimal", VK_DECIMAL },
    { "Divide", VK_DIVIDE },
    { "F1", VK_F1 },
    { "F2", VK_F2 },
    { "F3", VK_F3 },
    { "F4", VK_F4 },
    { "F5", VK_F5 },
    { "F6", VK_F6 },
    { "F7", VK_F7 },
    { "F8", VK_F8 },
    { "F9", VK_F9 },
    { "F10", VK_F10 },
    { "F11", VK_F11 },
    { "F12", VK_F12 },
    { "F13", VK_F13 },
    { "F14", VK_F14 },
    { "F15", VK_F15 },
    { "F16", VK_F16 },
    { "F17", VK_F17 },
    { "F18", VK_F18 },
    { "F19", VK_F19 },
    { "F20", VK_F20 },
    { "F21", VK_F21 },
    { "F22", VK_F22 },
    { "F23", VK_F23 },
    { "F24", VK_F24 },
    { "NumLock", VK_NUMLOCK },
    { "ScrollLock", VK_SCROLL },
    { "Browser Back", VK_BROWSER_BACK },
    { "Browser Forward", VK_BROWSER_FORWARD },
    { "Browser Refresh", VK_BROWSER_REFRESH },
    { "Browser Stop", VK_BROWSER_STOP },
    { "Browser Search", VK_BROWSER_SEARCH },
    { "Browser Favorites", VK_BROWSER_FAVORITES },
    { "Browser Start and Home", VK_BROWSER_HOME },
    { "Volume Mute", VK_VOLUME_MUTE },
    { "Volume Down", VK_VOLUME_DOWN },
    { "Volume Up", VK_VOLUME_UP },
    { "Next Track", VK_MEDIA_NEXT_TRACK },
    { "Previous Track", VK_MEDIA_PREV_TRACK },
    { "Stop Media", VK_MEDIA_STOP },
    { "Play/Pause Media", VK_MEDIA_PLAY_PAUSE },
    { "Start Mail", VK_LAUNCH_MAIL },
    { "Select Media", VK_LAUNCH_MEDIA_SELECT },
    { "Start Application 1", VK_LAUNCH_APP1 },
    { "Start Application 2", VK_LAUNCH_APP2 },
    { ";", VK_OEM_1 },
    { ":", VK_OEM_1 | SHIFT },
    { "=", VK_OEM_PLUS },
    { "+", VK_OEM_PLUS | SHIFT },
    { ",", VK_OEM_COMMA },
    { "<", VK_OEM_COMMA | SHIFT },
    { "-", VK_OEM_MINUS },
    { "_", VK_OEM_MINUS | SHIFT },
    { ".", VK_OEM_PERIOD },
    { ">", VK_OEM_PERIOD | SHIFT },
    { "/", VK_OEM_2 },
    { "?", VK_OEM_2 | SHIFT },
    { "`", VK_OEM_3 },
    { "~", VK_OEM_3 | SHIFT },
    { "[", VK_OEM_4 },
    { "{", VK_OEM_4 | SHIFT },
    { "\\", VK_OEM_5 },
    { "|", VK_OEM_5 | SHIFT },
    { "]", VK_OEM_6 },
    { "}", VK_OEM_6 | SHIFT },
    { "'", VK_OEM_7 },
    { "\"", VK_OEM_7 | SHIFT },
    { "Attn", VK_ATTN },
    { "CrSel", VK_CRSEL },
    { "ExSel", VK_EXSEL },
    { "Erase EOF", VK_EREOF },
    { "Play", VK_PLAY },
    { "Zoom", VK_ZOOM },
    { "PA1", VK_PA1 },
};

static bool iequals(const std::string_view &a, const std::string_view &b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

void ConfigEditorWin::InstallKeyBinding(const KeyBinding *keys, size_t numKeys)
{
    // clear all of the existing bindings
    for (auto &kb : keyBinding)
        kb.clear();

    // reset our counter of assigned context indices
    int nextContextIndex = 1;

    // command-to-key name map, for rebuilding the menus
    std::unordered_map<UINT, const char*> commandToKeyName;

    // traverse the key map
    const KeyBinding *k = keys;
    for (size_t i = 0 ; i < numKeys ; ++i, ++k)
    {
        // always start a new key at the initial context
        int curContextIndex = 0;
        
        // If it has a WM_COMMAND command, enter the command into our reverse 
        // mapping table.  This only applies if there's a WM_COMMAND command,
        // since those are the only ones that can appear on menus.
        if (k->command != 0)
            commandToKeyName.emplace(k->command, k->keys.c_str());

        // decode the key name
        int keycode = 0;
        for (const char *p = k->keys.c_str() ; *p != 0 ; )
        {
            // scan the next element
            const char *tokp = p, *curKeyName = p;
            for (; *p != 0 && *p != '+' && *p != ' ' ; ++p);
            std::string_view tok(tokp, p - tokp);

            // check for a modifier
            if (*p == '+')
            {
                // match the modifier
                if (iequals(tok, "shift"))
                    keycode |= SHIFT;
                else if (iequals(tok, "ctrl"))
                    keycode |= CTRL;
                else if (iequals(tok, "alt"))
                    keycode |= ALT;
                else
                    OutputDebugStringFmtA("Invalid modifier '%.*s' in key binding '%s'\n", 
                        static_cast<int>(tok.size()), tok.data(), k->keys.c_str());

                // skip the '+'
                ++p;
            }
            else
            {
                // last token of this key - look up the key code
                if (auto it = vkeyMap.find(tok); it != vkeyMap.end())
                {
                    // When a key name is used WITH a modifier, as in "Ctrl+%", use
                    // the base key, ignoring any modifiers associated with the key
                    // name, since that must be only for display purposes given that
                    // we have explicit modifiers.  When there are no explicit modifiers,
                    // keep the intrinsic modifiers.
                    //
                    // This applies to keys like "%", which is defined as (SHIFT | 0x35)
                    // (i.e., the shifted "5" key).  When a menu says something "Ctrl+%",
                    // remove the shift since the menu spelled out the modifiers it
                    // wants the user to press.  But "%" on its own needs the shift to
                    // activate it.
                    if (keycode != 0)
                        keycode |= (it->second) & ~(SHIFT | CTRL | ALT);
                    else
                        keycode = it->second;
                }
                else
                    OutputDebugStringFmtA("Invalid key name '%.*s' in key binding '%s'\n", 
                        static_cast<int>(tok.size()), tok.data(), k->keys.c_str());

                // check for another key following
                if (*p == ' ')
                {
                    // This is a prefix key in a sequence.  Look up the context selector
                    // for the key, or define a new one if we haven't encountered this
                    // prefix before.
                    if (auto it = keyBinding[curContextIndex].find(keycode); it != keyBinding->end())
                    {
                        // we've already enrolled this key - use its context selector
                        curContextIndex = it->second.contextSelector;
                    }
                    else if (nextContextIndex + 1 < static_cast<int>(_countof(keyBinding)))
                    {
                        // this is the first time we've seen this prefix - add a new context
                        keyBinding[curContextIndex].emplace(
                            std::piecewise_construct,
                            std::forward_as_tuple(keycode),
                            std::forward_as_tuple(std::string(k->keys.c_str(), p - k->keys.c_str()).c_str(), 0, 0, nextContextIndex));

                        // switch to the new context
                        curContextIndex = nextContextIndex++;
                    }
                    else
                    {
                        OutputDebugStringFmtA("Too many secondary key tables at key binding '%s'; limit is %d",
                            k->keys.c_str(), static_cast<int>(_countof(keyBinding)));
                    }

                    // start the next key
                    ++p;
                    keycode = 0;
                }
                else
                {
                    // This is the last key, so enter its command in the current context
                    keyBinding[curContextIndex].emplace(
                        std::piecewise_construct,
                        std::forward_as_tuple(keycode),
                        std::forward_as_tuple(k->keys.c_str(), k->sciCommand, k->command, 0));
                }
            }
        }
    }

    // Update the menu accelerator names
    UpdateMenuAccelLabels(hMenuBar, commandToKeyName);
    UpdateMenuAccelLabels(hCtxMenu, commandToKeyName);
}

void ConfigEditorWin::UpdateMenuAccelLabels(HMENU menu, 
    const std::unordered_map<UINT, const char*> &commandToKeyName)
{
    // traverse the submenus of the menu
    for (UINT i = 0, n = GetMenuItemCount(menu) ; i < n ; ++i)
    {
        // get the menu info
        MENUITEMINFOA mii{ sizeof(mii), MIIM_STRING | MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU };
        char label[256] = "";
        mii.dwTypeData = label;
        mii.cch = _countof(label);
        if (GetMenuItemInfoA(menu, i, TRUE, &mii))
        {
            // if it has a submenu, traverse it recursively
            if (mii.hSubMenu != NULL)
                UpdateMenuAccelLabels(mii.hSubMenu, commandToKeyName);

            // if it has a string, check for an accelerator match
            if (mii.cch != 0)
            {
                // find the command ID in the key map
                if (auto it = commandToKeyName.find(mii.wID); it != commandToKeyName.end())
                {
                    // find any existing accelerator label
                    char *accel = strchr(label, '\t');
                    if (accel == nullptr)
                        accel = label + strlen(label);

                    // add the new accelerator label
                    sprintf_s(accel, _countof(label) - (accel - label), "\t%s", it->second);

                    // update the menu label
                    mii.fMask = MIIM_STRING;
                    SetMenuItemInfoA(menu, i, TRUE, &mii);
                }
            }
        }
    }
}

// set my menu bar in the host application
bool ConfigEditorWin::InstallParentMenuBar(HWND hwndContainer)
{
    SetMenu(hwndContainer, hMenuBar);
    return true;
}


void ConfigEditorWin::ShowErrorPanel(bool show)
{
    errorPanelVisible = show;
    ShowWindow(errorPanel, show ? SW_SHOW : SW_HIDE);
    AdjustLayout();
}

void ConfigEditorWin::ResetParseDelay()
{
    if (parsePending)
        SetTimer(hwnd, TIMER_ID_PARSE, 100, NULL);
}

void ConfigEditorWin::ParseContents()
{
    if (parsePending)
    {
        // reset the JSON parser
        parsedJSON.reset(new JSONParserExt());

        // grab a copy of the text
        INT_PTR len = CallSci(SCI_GETLENGTH);
        parsedText.resize(len + 1);
        CallSci(SCI_GETTEXT, len, reinterpret_cast<INT_PTR>(parsedText.data()));

        // no more parse pending
        parsePending = false;

        // parse it
        parsedJSON->Parse(parsedText.data(), len);

        // find the new parse position
        FindParsePosition();

        // check to see if we should begin auto-completion
        CheckBeginAutoComplete();
    }
}

void ConfigEditorWin::FindParsePosition()
{
    // find the parse tree location of the current caret position
    FindParsePosition(CallSci(SCI_GETCURRENTPOS), curLocParsedPath, curLocSchemaObj, curLocSchemaSubclass, curLocSchemaProp, curLocSchemaVal);

    // get the help link for the current property
    curLocHelpLink = curLocSchemaProp != nullptr ? curLocSchemaProp->link : nullptr;
}

void ConfigEditorWin::FindParsePosition(INT_PTR pos, JSONParserExt::Path &path, 
    const JSONSchemaObj* &obj, const JSONParser::Value* &subclassSelector,
    const JSONSchemaObj::Prop* &prop, JSONParser::Value* &val)
{
    if (parsedJSON != nullptr)
    {
        // get the current position, as a pointer into the captured source text
        const char *txtPtr = parsedText.data() + pos;

        // find this position in the parse tree
        path = parsedJSON->FindTextPtr(txtPtr, &val);

        // clear old derived location data
        prop = nullptr;

        // Locate the path position in the schema tree
        const JSONSchemaObj *curObj = obj = jsonSchemaRoot;
        const JSONSchemaObj::Prop *curProp = nullptr;
        for (auto &ele : path.path)
        {
            // If there's a property name, look it up in the current
            // schema object.  The schema tree doesn't store anything
            // for array dereferences, so just skip those; it just
            // stores the next object down when the array elements are
            // objects.
            if (ele.prop.size() != 0 && curObj != nullptr)
            {
                // set the current schema object
                obj = curObj;

                // look up the property; stop if we don't find a match
                curProp = curObj->FindProp(ele.prop.c_str(), ele.value);
                if (curProp == nullptr)
                    break;

                // this is the most precise location so far
                prop = curProp;

                // traverse into the object
                curObj = (curProp->subObj != nullptr) ? curProp->subObj :
                    (curProp->xrefObj != nullptr) ? curProp->xrefObj :
                    nullptr;

                // check for a subclass type selector
                if (curObj != nullptr && curObj->subclassIdPropName != nullptr && ele.value->IsObject())
                {
                    // get the property element
                    subclassSelector = nullptr;
                    auto it = ele.value->object->find(curProp->name);
                    if (it != ele.value->object->end() && it->second.IsObject())
                    {
                        // search for the type selector property
                        auto it2 = it->second.object->find(curObj->subclassIdPropName);
                        if (it2 != it->second.object->end())
                            subclassSelector = &it2->second;
                    }
                }

                // set the last non-null object as the current object
                if (curObj != nullptr)
                    obj = curObj;
            }
        }
    }
}

void ConfigEditorWin::ShowNavDropMenu()
{
    // if a parse is pending, apply it now
    ParseContents();

    // build a sorted list of the top-level properties
    struct PropInfo
    {
        PropInfo(int cmd, const JSONParser::Value *val) : cmd(cmd), val(val) { }
        int cmd;                            // menu item ID, synthesized during menu construction
        const JSONParser::Value *val;       // value node
    };
    std::map<std::string, PropInfo> props;
    int cmd = 100;
    parsedJSON->rootValue.ForEach([&props, &cmd](const JSONParser::Value::StringWithLen &name, const JSONParser::Value *val) {
        props.emplace(std::piecewise_construct, std::forward_as_tuple(name.txt, name.len), std::forward_as_tuple(cmd++, val));
    });

    // construct a menu from the map
    HMENU menu = CreatePopupMenu();
    MENUITEMINFOA mii{ sizeof(mii) };
    mii.fMask = MIIM_ID | MIIM_STRING;
    const int ROOT_OBJECT_COMMAND = 50;
    int idx = 0;

    // add an item for the root object
    mii.dwTypeData = const_cast<LPSTR>("{ Root Object }");
    mii.wID = ROOT_OBJECT_COMMAND;
    InsertMenuItemA(menu, idx++, TRUE, &mii);

    // add the property items
    for (auto &pair : props)
    {
        mii.dwTypeData = const_cast<LPSTR>(pair.first.c_str());
        mii.wID = pair.second.cmd;
        InsertMenuItemA(menu, idx++, TRUE, &mii);
    }

    // get the nav button bottom right in screen coordinates
    POINT pt{ navDropButton->rc.right, navDropButton->rc.bottom };
    ClientToScreen(hwnd, &pt);

    // show the popup menu
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_TOPALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, hwnd, NULL);

    // find the command
    if (cmd == ROOT_OBJECT_COMMAND)
    {
        CallSci(SCI_GOTOLINE, 0);
        CallSci(SCI_VERTICALCENTRECARET);
    }
    else
    {
        for (auto &pair : props)
        {
            if (pair.second.cmd == cmd)
            {
                CallSci(SCI_GOTOLINE, pair.second.val->startTok.lineNum - 1);
                CallSci(SCI_VERTICALCENTRECARET);
                break;
            }
        }
    }

    // delete the menu
    DestroyMenu(menu);
}

// translate keyboard accelerators
bool ConfigEditorWin::TranslateAccelerators(HWND hwndMenu, MSG *msg)
{
    // check the message
    switch (msg->message)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Key-down event

        // reset the parsing delay timer
        ResetParseDelay();
        
        // Check what type of key we have.
        switch (msg->wParam)
        {
        case VK_SHIFT:
        case VK_MENU:
        case VK_CONTROL:
        case VK_LWIN:
        case VK_RWIN:
            // Modifier key.  These don't trigger commands directly, so
            // there's no translation necessary for this message, and it
            // doesn't affect the current prefix context.
            break;

        default:
            // Regular key
            if (quotedCharPending)
            {
                // Get the ASCII translation of the key.  Treat Ctrl+Space as NUL,
                // otherwise rely on ToAsciiEx to do the translation.
                BYTE kbState[256]{ 0 };
                if (GetKeyboardState(kbState))
                {
                    WORD charToInsert = 0;
                    int len = 0;
                    if (msg->wParam == VK_SPACE && (kbState[VK_CONTROL] & 0x80) != 0)
                    {
                        // Ctrl+Space = NUL
                        charToInsert = 0;
                        len = 1;
                    }
                    else
                    {
                        // use the Windows mapping
                        len = ToAsciiEx(msg->wParam, (msg->lParam >> 16) & 0xFF, kbState, &charToInsert, 0, NULL);
                    }
                    if (len != 0)
                    {
                        uint8_t buf[3]{ static_cast<uint8_t>(charToInsert & 0xFF), static_cast<uint8_t>((charToInsert >> 8) & 0xFF), 0 };
                        CallSci(SCI_ADDTEXT, len, reinterpret_cast<INT_PTR>(buf));
                    }
                }

                // consume the input and end quoted character mode
                quotedCharPending = false;
                return true;
            }
            else
            {
                // combine the VK_ code and the shift bits to get the map key
                int key = msg->wParam;
                if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) key |= SHIFT;
                if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) key |= CTRL;
                if ((GetKeyState(VK_MENU) & 0x8000) != 0) key |= ALT;

                // look up the key in the current context
                const auto &map = keyBinding[keyBindingContext.index];
                if (auto it = map.find(key); it != map.end())
                {
                    // Bound key found.  Invoke the Scintilla command, WM_COMMAND
                    // command, or key binding context change as appropriate.
                    auto &cmd = it->second;
                    if (cmd.sciCommand != 0)
                    {
                        // reset the prefix context
                        keyBindingContext.Clear();

                        // it's a direct Scintilla command
                        CallSci(cmd.sciCommand);
                    }
                    else if (cmd.command != 0)
                    {
                        // reset the prefix context
                        keyBindingContext.Clear();

                        // it's a WM_COMMAND - send it to the menu window, with the 1 in
                        // the high word of the WPARAM to indicate that it's from an 
                        // accelerator key press
                        SendMessage(hwndMenu, WM_COMMAND, MAKELPARAM(cmd.command, 1), 0);
                    }
                    else
                    {
                        // it's a context menu selection
                        keyBindingContext.index = cmd.contextSelector;
                        keyBindingContext.prefix = cmd.keys.c_str();
                    }

                    // message translated
                    return true;
                }
                else if (keyBindingContext.index != 0)
                {
                    // This is the second or nth key of a prefix sequence, and we
                    // didn't find a binding, so this forms an invalid sequence.
                    // Simply eat the key event, by saying that it was translated
                    // even though we didn't generate a command.
                    keyBindingContext.Clear();
                    return true;
                }
                else
                {
                    // Not found, no prefix in effect -> pass the key through
                    // to the window as a normal key.
                }
            }
            break;
        }
        break;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_ACTIVATE:
        // on mouse events or window activation events, reset the binding
        // prefix context and reset the parsing delay
        keyBindingContext.Clear();
        ResetParseDelay();
        break;
    }

    // not translated
    return false;
}

// Window UI activation
void ConfigEditorWin::OnActivateUI(bool isAppActivate)
{
    // set tab on the Scintilla control
    SetFocus(sciWin);
}

bool ConfigEditorWin::OnSetCursor(HWND hwndCursor, UINT hitTest, UINT msg)
{
    // get the mouse position in client coordinates
    POINT pt; 
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);

    // check for the error window grabber bar
    if (errorPanelVisible && PtInRect(&rcErrorSizer, pt))
    {
        // use the resize cursor
        SetCursor(LoadCursor(NULL, IDC_SIZENS));
        return true;
    }

    // use the default handling
    return __super::OnSetCursor(hwndCursor, hitTest, msg);
}


void ConfigEditorWin::SetConfigText(const std::vector<char> &text, bool resetUndo)
{
    // before we create an undo action, let's make sure the text has
    // actually changed
    auto docp = reinterpret_cast<const char*>(CallSci(SCI_GETDOCPOINTER));
    size_t len = CallSci(SCI_GETLENGTH);
    if (text.size() != len || memcmp(text.data(), docp, len) != 0)
    {
        // empty the current text and append the new text, as an atomic undo operation
        CallSci(SCI_BEGINUNDOACTION);
        CallSci(SCI_CLEARALL);
        CallSci(SCI_APPENDTEXT, text.size(), reinterpret_cast<INT_PTR>(text.data()));
        CallSci(SCI_ENDUNDOACTION);
    }

    // if desired, reset the undo history - this counts as the initial state for undo
    if (resetUndo)
        CallSci(SCI_EMPTYUNDOBUFFER);

    // save our internal copy for comparison on device reconnect
    lastDeviceText = text;
}

void ConfigEditorWin::LoadFromDevice(bool resetUndo)
{
    // retrieve the text from the device
    std::vector<char> text;
    int status = PinscapeResponse::ERR_FAILED;
    if (VendorInterface::Shared::Locker l(device); l.locked)
        status = device->device->GetConfig(text, configType);

    // on success, load into the editor window
    if (status == PinscapeResponse::OK)
    {
        // set the editor text
        SetConfigText(text, resetUndo);
        CallSci(SCI_SETSAVEPOINT);
    }
    else if (status == PinscapeResponse::ERR_NOT_FOUND)
    {
        // not found - set empty text
        text.clear();
        SetConfigText(text, resetUndo);
        CallSci(SCI_SETSAVEPOINT);

        // show the import template dialog, but after finishing the current message
        // (since we might still be creating the window)
        PostMessage(hwnd, MSG_OFFERTEMPLATE, 0, 0);
    }
    else
    {
        // announce the error, leave anything in the buffer unchanged
        MessageBoxFmt(hwnd, "Error loading configuration file from device (%s)", VendorInterface::ErrorText(status));
    }
}

void ConfigEditorWin::OfferTemplateFile()
{
    // Show a dialog offering to import a template file, depending on the config type
    static const std::map<uint8_t, int> dlgTypeMap{
        { PinscapeRequest::CONFIG_FILE_MAIN, IDD_BLANK_CONFIG_OPTIONS },
        { PinscapeRequest::CONFIG_FILE_SAFE_MODE, IDD_BLANK_SAFEMODECONFIG_OPTIONS },
    };
    if (auto dialogType = dlgTypeMap.find(configType); dialogType != dlgTypeMap.end())
    {
        // show the dialog
        class TplDlg : public Dialog
        {
        public:
            TplDlg(HINSTANCE hInstance) : Dialog(hInstance) { }
            virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override
            {
                // Select the SECOND radio button.  The first is always
                // "Blank", and the second is the default starter template.
                int n = 0;
                EnumChildWindows(hDlg, [](HWND child, LPARAM lparam)
                {
                    // check for a radio button
                    char c[128];
                    if (GetClassNameA(child, c, _countof(c))
                        && strcmp(c, WC_BUTTONA) == 0
                        && (GetWindowStyle(child) & BS_TYPEMASK) == BS_AUTORADIOBUTTON)
                    {
                        // It's a radio button - count it
                        auto &n = *reinterpret_cast<int*>(lparam);
                        if (++n == 2)
                        {
                            // it's the second radio button - check it and stop searching
                            Button_SetCheck(child, BST_CHECKED);
                            return FALSE;
                        }
                    }

                    // continue the enumeration
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&n));

                return __super::OnInitDialog(wparam, lparam);
            }
            virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom) override
            {
                // on OK, note the final radio button selection
                if (command == IDOK)
                {
                    static const struct
                    {
                        int buttonID;            // button command ID
                        const TCHAR *filename;    // corresponding filename
                    } idToFile[] =
                    {
                        // main config
                        { IDC_RB_STARTER, _T("Starter.json") },
                        { IDC_RB_PROEXPAN, _T("ProExpansionBoard.json") },
                        { IDC_RB_DIYEXPAN, _T("DIYExpansionBoard.json") },

                        // safe-mode config
                        { IDC_RB_STARTER_SAFEMODE, _T("SafeModeStarter.json") },
                    };
                    for (auto &m : idToFile)
                    {
                        if (IsDlgButtonChecked(hDlg, m.buttonID) == BST_CHECKED)
                        {
                            filename = m.filename;
                            break;
                        }
                    }
                }

                // continue to the default handling
                return __super::OnCommand(command, code, ctlFrom);
            }

            // the file selected in the dialog, set on clicking OK
            const TCHAR *filename = nullptr;
        };
        TplDlg dlg(hInstance);
        if (dlg.Show(dialogType->second, GetDialogOwner()) == IDOK && dlg.filename != nullptr)
        {
            // file successfully selected - import it, from the ConfigTemplates folder
            TCHAR path[MAX_PATH];
            GetConfigTemplatesFolder(path);
            PathAppend(path, dlg.filename);
            ImportFile(path);
        }
    }
}

void ConfigEditorWin::SaveToDevice()
{
    // get a pointer to the Scintilla text
    const char *text = reinterpret_cast<const char*>(CallSci(SCI_GETCHARACTERPOINTER));
    INT_PTR len = CallSci(SCI_GETLENGTH);

    // run a syntax check before saving to the Pico
    JSONParser json;
    json.Parse(text, len);
    if (json.errors.size() != 0)
    {
        // run the error check command to show the error panel
        PostMessage(hwnd, WM_COMMAND, ID_FILE_CHECKERRORS, 0);

        // explain why the save was refused
        MessageBoxFmt(hwnd, "FILE NOT SAVED\r\n"
            "\r\nThe JSON contains syntax errors.  Only a valid JSON file can "
            "be saved to the device.  Please review and correct the listed errors.");

        // abort
        return;
    }

    // If the Confirm Saves is enabled, assume that we'll show a
    // confirmation dialog before saving.  However, we'll skip that
    // if we also show a schema error dialog, since that asks what
    // is basically the same question ("do you really want to save
    // this file to the Pico?") in a slightly different way ("do
    // you really want to save the file *with these errors* to the
    // Pico?").  It would be awfully redundant and irritating to
    // to click through both dialogs.
    bool confirmSave = options.confirmSaves;

    // do a schema rules check if desired
    if (options.checkRulesBeforeSave)
    {
        // run a rules check
        CheckSchema(json);

        // if we found any errors, show the confirmation dialog
        if (schemaCheckErrors.size() != 0)
        {
            // show the dialog; abort the save on Cancel
            class SchemaErrorDialog : public Dialog
            {
            public:
                SchemaErrorDialog(HINSTANCE hInstance, ConfigEditorWin *win) : Dialog(hInstance), win(win) { }
                ConfigEditorWin *win;

                virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom) override
                {
                    // show help if needed
                    if (command == IDC_BTN_HELP)
                    {
                        win->ShowHelpFile("ConfigEditor.htm", "SchemaCheck");
                        return true;
                    }

                    // use the normal handling
                    return __super::OnCommand(command, code, ctlFrom);
                }
            };
            SchemaErrorDialog dlg(hInstance, this);
            if (dlg.Show(IDD_SAVE_TO_PICO_SCHEMA_ERRORS) != IDOK)
            {
                ShowErrorPanel(schemaCheckErrors);
                return;
            }

            // save confirmed - don't show another confirmation dialog
            confirmSave = false;
        }
    }

    // ask for confirmation if desired
    if (confirmSave)
    {
        class ConfirmationDialog : public Dialog
        {
        public:
            ConfirmationDialog(HINSTANCE hInstance) : Dialog(hInstance) { }

            virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override
            {
                // if the dialog is showing, the "confirm saves" option is enabled - sync the checkbox
                CheckDlgButton(hDlg, IDC_CK_CONFIRM_SAVES, BST_CHECKED);
                return __super::OnInitDialog(wparam, lparam);
            }

            virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom)
            {
                // on dismissal, update the "confirm saves" option to match the checkbox;
                // do this even if they canceled, since we'll take this as canceling the
                // save, not the checkbox update
                options.confirmSaves = (IsDlgButtonChecked(hDlg, IDC_CK_CONFIRM_SAVES) == BST_CHECKED);
                return __super::OnCommand(command, code, ctlFrom);
            }

        };
        ConfirmationDialog dlg(hInstance);
        if (dlg.Show(IDD_CONFIRM_SAVE_TO_PICO) != IDOK)
            return;
    }

    // if auto-backups are enabled, perform the auto-backup
    if (options.autoBackupEnabled)
    {
        // translate substitution variables in the auto-backup folder
        std::string folder = ExpandAutoCompleteDir(options.autoBackupFolder);

        // make sure the folder exists
        if (!PathFileExistsA(folder.c_str()))
            CreateDirectoryA(folder.c_str(), NULL);

        // get the device ID, to use in constructing the filename
        PinscapePico::DeviceID id;
        if (VendorInterface::Shared::Locker l(device); l.locked)
            device->device->QueryID(id);

        // put a name to the config file type
        const char *configTypeName =
            configType == PinscapeRequest::CONFIG_FILE_MAIN ? "Config" :
            configType == PinscapeRequest::CONFIG_FILE_SAFE_MODE ? "SafeMode" :
            "OtherConfig";

        // get the current local time
        SYSTEMTIME st;
        GetLocalTime(&st);
        
        // build a filename based on the file ID, device ID, and date
        char fname[256];
        sprintf_s(fname, "%s_%s_%04d%02d%02d_%02d%02d%02d.json",
            configTypeName, id.hwid.ToString().c_str(),
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        // build the full path
        char path[MAX_PATH];
        PathCombineA(path, folder.c_str(), fname);

        // write the file
        std::ofstream f(path);
        if (!f || !SaveToFile(f, text, len))
        {
            int btn = MessageBoxA(GetDialogOwner(),
                StrPrintf("Error writing auto-save file %s\r\n"
                    "\r\n"
                    "Do you still wish to save the file to the Pico?", path).c_str(),
                "Error Auto-Saving", MB_ICONERROR | MB_YESNO);

            if (btn != IDYES)
                return;
        }
    }

    // lock the device
    int status = PinscapeResponse::ERR_FAILED;
    bool rebooted = false;
    if (VendorInterface::Shared::Locker l(device); l.locked)
    {
        // send the configuration
        status = device->device->PutConfig(text, static_cast<int32_t>(len), configType);
        if (status == PinscapeResponse::OK)
        {
            // success - record this as the text on the device
            lastDeviceText.resize(len);
            memcpy(lastDeviceText.data(), text, len);

            // for the main config, reset the Pico
            if (configType == PinscapeRequest::CONFIG_FILE_MAIN)
            {
                device->device->ResetPico();
                rebooted = true;
            }
        }
    }
    
    // show the result
    if (status == PinscapeResponse::OK)
    {
        // success - mark the savepoint in the log
        CallSci(SCI_SETSAVEPOINT);

        // announce the successful result on the status line (so that there's no modal alert to dismiss)
        gApp.AddTimedStatusMessage(StrPrintf(
            "Configuration successfully saved to Pico%s", rebooted ? "; Pico rebooted" : "").c_str(),
            HRGB(0xffffff), HRGB(0x008000), 5000);
    }
    else
    {
        MessageBoxFmt(hwnd, "Error saving the configuration: %s (error code %d)",
            VendorInterface::ErrorText(status), status);
    }
}

std::string ConfigEditorWin::ExpandAutoCompleteDir(const std::string &dir)
{
    static const std::regex varPat("\\$\\(([a-z0-9._$]+)\\)", std::regex_constants::icase);
    return regex_replace(dir, varPat, [](const std::match_results<std::string::const_iterator> &m) -> std::string
    {
        // convert the name to lower-case for case-insensitive matching
        auto varName = m[1].str();
        std::transform(varName.begin(), varName.end(), varName.begin(), ::tolower);

        // look up the name
        if (varName == "installdir")
        {
            // install folder - same as the folder containing the program file
            char path[MAX_PATH];
            GetModuleFileNameA(gApp.hInstance, path, _countof(path));
            PathRemoveFileSpecA(path);
            return path;
        }
        else if (varName == "settingsdir")
        {
            // settings folder - the folder containing the current settings file
            char path[MAX_PATH];
            sprintf_s(path, "%ws", gApp.settingsFile.c_str());
            PathRemoveFileSpecA(path);
            return path;            
        }
        else
        {
            // no match - return the variable name unchanged, including the $()
            return "$(" + m[1].str() + ")";
        }
    });
}

bool ConfigEditorWin::ConfirmDiscard()
{
    // if there are no modifications since the last device load,
    // there's no need to ask the user - just tell the caller to
    // proceed
    if (!isModified)
        return true;

    // ask for confirmation
    return (MessageBoxA(GetDialogOwner(), "This configuration has been modified "
        "since loading it from the Pico. If you proceed, the changes will be lost.\r\n"
        "\r\n"
        "Are you sure you want to discard changes?", "Warning - Unsaved Changes",
        MB_ICONEXCLAMATION | MB_YESNOCANCEL) == IDYES);
}

void ConfigEditorWin::ReloadFromDevice()
{
    // check for unsaved changes and prompt for confirmation if needed
    if (!ConfirmDiscard())
        return;

    // reload; this is an undoable action, so that the user can recover
    // work done before the reload
    LoadFromDevice(false);
}

void ConfigEditorWin::ImportFile()
{
    // check for unsaved changes and prompt for confirmation if needed
    if (!ConfirmDiscard())
        return;

    // ask for a filename
    GetFileNameDlg dlg(_T("Import file"),
        OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT,
        _T("JSON files\0*.json\0Text files\0*.txt\0All Files\0*.*\0"), _T("json"));
    if (dlg.Open(GetDialogOwner()))
        ImportFile(dlg.GetFilename());
}

void ConfigEditorWin::GetConfigTemplatesFolder(TCHAR path[MAX_PATH])
{
    // get the program filename
    GetModuleFileName(hInstance, path, MAX_PATH);

    // remove the filename portion and add the ConfigTemplates folder
    PathRemoveFileSpec(path);
    PathAppend(path, _T("ConfigTemplates"));
}

void ConfigEditorWin::ImportTemplateFile()
{
    // check for unsaved changes and prompt for confirmation if needed
    if (!ConfirmDiscard())
        return;

    // get the <install>\ConfigTemplates folder
    TCHAR path[MAX_PATH];
    GetConfigTemplatesFolder(path);

    // ask for a filename, starting in the ConfigTemplates folder under the program folder
    GetFileNameDlg dlg(_T("Import a template configuration file"),
        OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT,
        _T("JSON files\0*.json\0Text files\0*.txt\0All Files\0*.*\0"), _T("json"), nullptr, path);
    if (dlg.Open(GetDialogOwner()))
        ImportFile(dlg.GetFilename());
}

void ConfigEditorWin::ImportFile(const TCHAR *filename)
{
        // load the files
        std::ifstream f(filename);
        if (!f)
        {
            MessageBoxFmt(hwnd, "Unable to read file \"%" _TSFMT "\"", filename);
            return;
        }

        // load it into a stringstream buffer
        std::stringstream s;
        s << f.rdbuf();

        // empty the current Scintilla text and append the new text; do this
        // as an atomic undo action
        CallSci(SCI_BEGINUNDOACTION);
        CallSci(SCI_CLEARALL);
        CallSci(SCI_APPENDTEXT, s.str().size(), reinterpret_cast<INT_PTR>(s.str().c_str()));
        CallSci(SCI_ENDUNDOACTION);
}

void ConfigEditorWin::SaveToFile()
{
    // ask for a filename
    GetFileNameDlg dlg(_T("Save configuration to file"),
        OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT,
        _T("JSON files\0*.json\0Text files\0*.txt\0All Files\0*.*\0"), _T("json"));
    if (dlg.Save(GetDialogOwner()))
    {
        // get a pointer to the Scintilla text
        const char *text = reinterpret_cast<const char*>(CallSci(SCI_GETCHARACTERPOINTER));
        INT_PTR len = CallSci(SCI_GETLENGTH);

        // save the text
        std::ofstream f(dlg.GetFilename());
        if (!f || !SaveToFile(f, text, len))
            MessageBoxFmt(hwnd, "Error writing file (%" _TSFMT ")", dlg.GetFilename());
    }
}

bool ConfigEditorWin::SaveToFile(std::ofstream &f, const char *text, size_t len)
{
    // Write the text, stripping CR ('\r') characters.  Scintilla uses
    // DOS-style CR-LF line endings natively, but it can be a little
    // inconsistent, so we get the best results by passing native C
    // '\n' endings to std::ofstream, and letting std::ofstream
    // translate to platform newlines as it writes.
    const char *p = text;
    const char *textEnd = text + len;
    while (p < textEnd)
    {
        // scan ahead to the next CR
        const char *start = p;
        for (; p < textEnd && *p != '\r' ; ++p) ;

        // write the part up to the CR
        if (p != start)
        {
            f.write(start, p - start);
            if (!f)
                return false;
        }

        // skip this and any consecutively following \r
        for (; p < textEnd && *p == '\r' ; ++p) ;
    }

    // close the file and return the ofstream status result
    f.close();
    return !!f;
}

void ConfigEditorWin::GoToLineNum()
{
    // dialog handler
    class GoToLineDialog : public Dialog
    {
    public:
        GoToLineDialog(HINSTANCE hInstance) : Dialog(hInstance) { }

        // command handler
        virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom)
        {
            // on IDOK, stash the line number text for retrieval after
            // the dialog exits
            if (command == IDOK)
                linenum = _ttoi(GetDlgItemText(IDC_EDIT_LINENUM).c_str());

            // use the base class handling
            return __super::OnCommand(command, code, ctlFrom);
        }

        // line number read from the text box
        int linenum = -1;
    };

    // show the dialog
    GoToLineDialog dlg(hInstance);
    int result = dlg.Show(IDD_GOTOLINENUM, GetDialogOwner());

    // on OK, go the requested line; note that Scintilla uses 0-based
    // numbering internally but 1-based numbering in the UI, so we have
    // to adjust the input accordingly
    if (result == IDOK && dlg.linenum >= 1)
        CallSci(SCI_GOTOLINE, dlg.linenum - 1);
}

void ConfigEditorWin::OnEraseDeviceConfig(bool factoryReset)
{
    // empty our Scintilla control
    CallSci(SCI_CLEARALL);
}

bool ConfigEditorWin::OnDeviceReconnect()
{
    // retrieve the new text from the device
    std::vector<char> text;
    int status = -1001;
    if (VendorInterface::Shared::Locker l(device); l.locked)
        status = device->device->GetConfig(text, configType);

    // consider a "Not Found" to be equivalent to an empty file
    if (status == PinscapeResponse::ERR_NOT_FOUND)
    {
        text.clear();
        status = PinscapeResponse::OK;
    }

    if (status == PinscapeResponse::OK)
    {
        // compare to the last copy
        if (text != lastDeviceText)
        {
            // The text on the device has changed.  If we've made any
            // modifications, warn the user, but don't do anything else:
            // we want to leave the user's modifications in place so that
            // they can decide what to do about the mismatch, such as
            // saving a backup copy of the current buffer.  If the
            // editor contents are unchanged, simply refresh the editor
            // with the new text.
            if (isModified)
            {
                MessageBoxFmt(hwnd, MB_ICONWARNING,
                    "The configuration file saved on the device has been modified. "
                    "The version that you're currently editing is out of date. "
                    "The new device version has NOT been automatically re-loaded, "
                    "since you've made changes to the working copy that haven't "
                    "been saved yet.\r\n"
                    "\r\n"
                    "If you save the working copy to the device, you'll overwrite "
                    "the newer version on the device, possibly losing work. "
                    "It's recommended that you save the current work to a local "
                    "file so that you can compare it to the new version on the "
                    "device before deciding which version to keep.");
            }
            else
            {
                // No modifications - load the new text.  Keep undo across
                // this action, since it's perfectly valid to save some changes
                // and then decide you want to undo them.
                SetConfigText(text, false);
                CallSci(SCI_SETSAVEPOINT);
            }
        }
    }
    else if (isModified)
    {
        MessageBoxFmt(hwnd, "Unable to retrieve the device configuration file (error code %d). "
            "The device has reconnected since the current editor copy of the file was loaded, "
            "so the editor contents might be out of date. If you save the current editor copy "
            "to the device, it might overwrite a new version. It's recommended that you save "
            "your current changes to a local file to compare against the copy currently on "
            "the device before deciding which version to keep.", status);
    }
    else
    {
        MessageBoxFmt(hwnd, "Unable to retrieve the device configuration file (error code %d). "
            "The device has reconnected since the current editor copy of the file was loaded, "
            "so the editor copy might be out of date. It's recommended that you manually "
            "reload the device copy into the editor before proceeding.", status);
    }

    // whatever happened, keep the window open
    return true;
}

LRESULT ConfigEditorWin::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case MSG_FINDREPLACEDLG:
        // Find/Replace dialog messages
        {
            // get the curent dialog information
            findWhat = findReplaceDlg->GetFindWhat();
            replaceWith = findReplaceDlg->GetReplaceWith();
            findReplaceFlags = findReplaceDlg->GetFlags();

            // check what happened - the button ID is in the WPARAM
            switch (wparam)
            {
            case IDC_BTN_FINDNEXT:
            case IDC_BTN_FINDPREV:
                // find next/prev
                findReplaceFlags.reverse = (wparam == IDC_BTN_FINDPREV);
                UpdateSearch(findWhat.c_str(), findReplaceFlags);
                CommitSearch(CommitSearchReason::Accept, false);
                break;

            case IDCLOSE:
                // dialog closing - release our references
                findReplaceDlg = nullptr;
                findReplaceDlgSharedRef.reset();
                break;

            case IDC_BTN_REPLACE:
                // Replace
                findReplaceFlags.reverse = false;
                ExecReplace(findWhat.c_str(), replaceWith.c_str(), findReplaceFlags);
                break;

            case IDC_BTN_REPLACEALL:
                // Replace All.  Execute replace operations as long as we keep finding
                // matches, stopping at end of buffer.  Bundle these as a single undo
                // operation.
                if (findWhat.size() != 0)
                {
                    CallSci(SCI_BEGINUNDOACTION);
                    findReplaceFlags.wrap = false;
                    findReplaceFlags.reverse = false;
                    while (ExecReplace(findWhat.c_str(), replaceWith.c_str(), findReplaceFlags)) { }
                    CallSci(SCI_ENDUNDOACTION);
                }
                break;
            }

            // handled
            return 0;
        }

    case MSG_OFFERTEMPLATE:
        OfferTemplateFile();
        return 0;
    }

    // use the default handling
    return __super::WndProc(msg, wparam, lparam);
}

bool ConfigEditorWin::ExecReplace(const char *findWhat, const char *replaceWith, SearchFlags flags)
{
    // Check to see if the current selection matches the search.
    // If so, replace it; if not, treat this like a Find Next.
    CallSci(SCI_TARGETFROMSELECTION);
    auto oldStart = CallSci(SCI_GETTARGETSTART);
    auto oldEnd = CallSci(SCI_GETTARGETEND);
    CallSci(SCI_SETSEARCHFLAGS, flags.SciFlags());
    auto newStart = CallSci(SCI_SEARCHINTARGET, strlen(findWhat), reinterpret_cast<INT_PTR>(findWhat));
    auto newEnd = CallSci(SCI_GETTARGETEND);
    if (newStart == oldStart && newEnd == oldEnd)
    {
        // it's a match - apply the current replacement

        // case sensing
        enum class CaseType { None, Lower, Upper, Mixed, MixedFirstUpper };
        static const auto SenseCase = [](const char *str) -> CaseType
        {
            bool foundFirstAlpha = false;
            bool anyUpper = false, allUpper = true, firstUpper = false;
            bool anyLower = false, allLower = true;
            for (const char *p = str ; *p != 0 ; ++p)
            {
                char ch = *p;
                if (std::isalpha(ch))
                {
                    if (std::isupper(ch))
                    {
                        if (!foundFirstAlpha) firstUpper = true;
                        anyUpper = true, allLower = false;
                    }
                    else if (std::islower(ch))
                    {
                        anyLower = true, allUpper = false;
                    }
                    foundFirstAlpha = true;
                }
            }

            return (anyUpper && allUpper) ? CaseType::Upper :
                (anyLower && allLower) ? CaseType::Lower :
                (anyUpper && anyLower && firstUpper) ? CaseType::MixedFirstUpper :
                (anyUpper && anyLower) ? CaseType::Mixed :
                CaseType::None;
        };

        // if we're in case-preserving mode, refigure the replacement text to
        // match the case of the original
        std::string newReplaceWith;
        if (flags.preserveCase)
        {
            // extract the current match text
            Sci_TextRange r{ 0 };
            r.chrg.cpMin = newStart;
            r.chrg.cpMax = newEnd;
            std::vector<char> buf;
            buf.resize(newEnd - newStart + 1);
            r.lpstrText = buf.data();
            CallSci(SCI_GETTEXTRANGE, 0, reinterpret_cast<INT_PTR>(&r));

            // sense the case style of the source material
            switch (SenseCase(buf.data()))
            {
            case CaseType::Lower:
                // matched text is all lower-case; use all lower-case replacement text
                newReplaceWith = replaceWith;
                replaceWith = newReplaceWith.c_str();
                std::transform(newReplaceWith.begin(), newReplaceWith.end(), newReplaceWith.begin(), ::tolower);
                break;

            case CaseType::Upper:
                // matched text is all upper-case; use all upper-case replacement text
                newReplaceWith = replaceWith;
                replaceWith = newReplaceWith.c_str();
                std::transform(newReplaceWith.begin(), newReplaceWith.end(), newReplaceWith.begin(), ::toupper);
                break;

            case CaseType::Mixed:
                // Matched text is mixed-case.  Use the same case as the original.
                break;

            case CaseType::MixedFirstUpper:
                // Matched text is mixed-case with the first alphabetic character
                // capitalized.  Capitalize the first alphabetic character of the 
                // replacement, and otherwise use its same mixed case.
                newReplaceWith = replaceWith;
                replaceWith = newReplaceWith.c_str();
                for (char *p = newReplaceWith.data() ; *p != 0 ; ++p)
                {
                    if (std::isalpha(*p))
                    {
                        *p = std::toupper(*p);
                        break;
                    }
                }
                break;
            }
        }

        // replace the selection
        CallSci(SCI_TARGETFROMSELECTION);
        CallSci(flags.regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET,
            strlen(replaceWith), reinterpret_cast<INT_PTR>(replaceWith));

        // advance the selection to the end of the replacement
        newEnd = CallSci(SCI_GETTARGETEND);
        CallSci(SCI_SETSELECTION, newEnd, newEnd);
    }

    // find next
    bool found = UpdateSearch(findWhat, flags);
    CommitSearch(CommitSearchReason::Accept, false);

    // return the search status
    return found;
}


void ConfigEditorWin::OnShowWindow(bool show, LPARAM source)
{
    // when hiding the window, close the Find/Replace dialog
    if (!show && findReplaceDlg != nullptr)
        PostMessage(findReplaceDlg->GetHWND(), WM_CLOSE, 0, 0);

    // do the normal work
    __super::OnShowWindow(show, source);
}

void ConfigEditorWin::OnSizeWindow(WPARAM type, WORD width, WORD height)
{
	// adjust the layout
	AdjustLayout();

    // do the base class work
    __super::OnSizeWindow(type, width, height);
}

void ConfigEditorWin::AdjustLayout()
{
    // get the client area
    RECT crc;
    GetClientRect(hwnd, &crc);

    // get the window DC
    HDCHelper hdc(hwnd);

    // start with the scintilla control getting the whole thing, minus the
    // control bar
    RECT src = crc;
    src.top += cyControlBar;

    // set up the buttons
    int cxBtn = szCtlBarButtons.cy;  // not cx - buttons are square, bitmap width is sum of all cells
    int cyBtn = szCtlBarButtons.cy;
    int x = crc.left + 4;
    int y = (crc.top + cyControlBar - cyBtn)/2;
    RECT rcBtn{ x, y, x + cxBtn, y + cyBtn };
    for (auto &b : ctlBarButtons)
    {
        // check special items
        switch (b.cmd)
        {
        case 0:
            // unpopulated
            break;

        case -1:
            // spacer - add padding on either side, and set the button rect to a
            // one-pixel vertical line where we draw a visual separator
            b.rc ={ rcBtn.left + 6, rcBtn.top, rcBtn.left + 7, rcBtn.bottom };
            OffsetRect(&rcBtn, 13, 0);
            break;

        case -2:
            // Find Next box
            {
                // vertically center the text box within the control bar, with a little margin to the left
                POINT ptFindBox{ rcBtn.left + 2, (y + cyControlBar - mainFontMetrics.tmHeight)/2 };
                SIZE szFindBox{ mainFontMetrics.tmAveCharWidth * 32, mainFontMetrics.tmHeight };
                SetWindowPos(findBox, NULL, ptFindBox.x, ptFindBox.y, szFindBox.cx, szFindBox.cy, SWP_NOZORDER);

                // set a tooltip for the edit box's whole client area
                RECT rc;
                GetClientRect(findBox, &rc);
                SetTooltip(rc, ID_EDIT_FIND, b.tooltip, findBox);

                // advance the next control rect
                OffsetRect(&rcBtn, ptFindBox.x + szFindBox.cx - rcBtn.left + 2, 0);
            }
            break;

        default:
            // Regular button - set the button rect
            b.rc = rcBtn;

            // if it has text, add the text size into the button rect
            if (b.label != nullptr)
                b.rc.right += hdc.MeasureText(mainFont, b.label).cx + 8;

            // set the tooltip
            SetTooltip(b.rc, b.cmd, b.tooltip);

            // advance the rect to the next button, if it's not specially positioned
            if (!b.specialPos)
                OffsetRect(&rcBtn, b.rc.right - rcBtn.left, 0);
            break;
        }
    }

    // if the error window is visible, position it at the bottom
    if (errorPanelVisible)
    {
        // deduct it from Scintilla's space
        src.bottom -= cyErrorPanel + cyErrorSizer + cyErrorTitle;

        // make sure the title bar doesn't go below the bottom of the client area
        src.bottom = min(src.bottom, crc.bottom - cyErrorSizer - cyErrorTitle);

        // make sure Scintilla has a minimum height
        src.bottom = max(src.bottom, src.top + 50);

        // position the error panel
        SetWindowPos(errorPanel, NULL, crc.left, src.bottom + cyErrorSizer + cyErrorTitle,
            crc.right - crc.left, cyErrorPanel, SWP_NOZORDER);
    }

    // position the Scintilla window
    SetWindowPos(sciWin, NULL, src.left, src.top, src.right - src.left, src.bottom - src.top, SWP_NOZORDER);
}

bool ConfigEditorWin::OnLButtonDown(WPARAM keys, int x, int y)
{
    // check for a click in the error list close box
    POINT pt{ x, y };
    if (errorPanelVisible && PtInRect(&rcErrorCloseBox, pt))
    {
        ShowErrorPanel(false);
        return true;
    }

    // check for the error list sizer
    if (errorPanelVisible && PtInRect(&rcErrorSizer, pt))
    {
        errorPanelResize.tracking = true;
        errorPanelResize.start = pt;
        errorPanelResize.cyInitial = cyErrorPanel;
        SetCapture(hwnd);
        return true;
    }

    // check for the current context help link
    if (PtInRect(&rcHelpLink, pt))
    {
        ShowHelpFile("JSONConfigRef.htm", curLocHelpLink);
        return true;
    }

    // use the default handling
    return __super::OnLButtonDown(keys, x, y);
}

bool ConfigEditorWin::OnMouseMove(WPARAM keys, int x, int y)
{
    if (errorPanelResize.tracking)
    {
        int dy = y - errorPanelResize.start.y;
        int dh = -dy;  // -Y is towards top of screen = up = increasing height
        RECT rc = GetChildControlRect(errorPanel);
        cyErrorPanel = errorPanelResize.cyInitial + dh;
        AdjustLayout();
        return true;
    }

    return __super::OnMouseMove(keys, x, y);
}

bool ConfigEditorWin::OnLButtonUp(WPARAM keys, int x, int y)
{
    if (errorPanelResize.tracking)
        ReleaseCapture();

    return __super::OnLButtonUp(keys, x, y);
}

bool ConfigEditorWin::OnCaptureChange(HWND hwnd)
{
    if (errorPanelResize.tracking)
    {
        gApp.settingsJSON.SetNum("editor.errorPanel.cy", cyErrorPanel);
        errorPanelResize.tracking = false;
    }

    return __super::OnCaptureChange(hwnd);
}

void ConfigEditorWin::OnInitMenuPopup(HMENU hMenu, WORD itemPos, bool isSysMenu)
{
    // apply the default handling, to enable/disable items dynamically
    __super::OnInitMenuPopup(hMenu, itemPos, isSysMenu);

    // update the "Help on <selection>" item to reflect the current selection, if any
    MENUITEMINFOA mii{ sizeof(mii) };
    mii.fMask = MIIM_ID;
    if (GetMenuItemInfoA(hMenu, ID_HELP_ON_SELECTION, FALSE, &mii))
    {
        std::string s = "&Help on " + (curLocHelpLink != nullptr ? curLocParsedPath.ToString() : "Selection");
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = s.data();
        SetMenuItemInfoA(hMenu, ID_HELP_ON_SELECTION, FALSE, &mii);
    }
}

void ConfigEditorWin::UpdateCommandStatus(std::function<void(int, bool)> apply)
{
    // enable some commands according to whether or not there's a selection
    bool hasSelection = (CallSci(SCI_GETSELECTIONSTART) != CallSci(SCI_GETSELECTIONEND));
    apply(ID_EDIT_COPY, hasSelection);
    apply(ID_EDIT_CUT, hasSelection);
    apply(ID_EDIT_DELETE, hasSelection);

    // enable undo and redo according to the Scintilla status
    apply(ID_EDIT_UNDO, CallSci(SCI_CANUNDO) != 0);
    apply(ID_EDIT_REDO, CallSci(SCI_CANREDO) != 0);

    // enable Find Next if we have a search time saved
    apply(ID_EDIT_FINDNEXT, lastSearch.term.size() != 0);

    // enable Help On <selection> when there's a selection
    apply(ID_HELP_ON_SELECTION, curLocHelpLink != nullptr);

    // inherit the base handling
    __super::UpdateCommandStatus(apply);
}

void ConfigEditorWin::OnTimer(WPARAM timerId)
{
    // piggyback on the refresh timer for command updates
    switch (timerId)
    {
    case TIMER_ID_PARSE:
        // parse the contents
        ParseContents();

        // this is a one-shot timer, so delete it
        KillTimer(hwnd, timerId);
        break;
    }

    // inherit the default handling
    __super::OnTimer(timerId);
}

bool ConfigEditorWin::OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult)
{
    switch (notifyCode)
    {
    case 0:
    case 1:
        // menu command, accelerator, or button click
        switch (ctlCmdId)
        {
        case ID_TOOLS_TEXTEDITORPREFS:
            ShowPrefsDialog();
            return true;

        case ID_FILE_PROGRAMPICO:
            SaveToDevice();
            return true;

        case ID_FILE_RELOADFROMPICO:
            ReloadFromDevice();
            return true;

        case ID_FILE_IMPORTFROMFILE:
            ImportFile();
            return true;

        case ID_FILE_IMPORTFROMTEMPLATEFILE:
            ImportTemplateFile();
            return true;

        case ID_FILE_SAVETOFILE:
            SaveToFile();
            return true;

        case ID_EDIT_COPY:
            CallSci(SCI_COPY);
            return true;

        case ID_EDIT_CUT:
            CallSci(SCI_CUT);
            return true;

        case ID_EDIT_PASTE:
            CallSci(SCI_PASTE);
            return true;

        case ID_EDIT_DELETE:
            CallSci(SCI_CLEAR);
            return true;

        case ID_EDIT_SELECTALL:
            CallSci(SCI_SELECTALL);
            return true;

        case ID_EDIT_UNDO:
            CallSci(SCI_UNDO);
            CallSci(SCI_AUTOCCANCEL);
            return true;

        case ID_EDIT_REDO:
            CallSci(SCI_REDO);
            CallSci(SCI_AUTOCCANCEL);
            return true;

        case ID_EDIT_FIND:
        case ID_EDIT_REVFIND:
            // if focus is already in the search box, treat this as a find next/prev
            if (GetFocus() == findBox)
            {
                FindNextPrev(ctlCmdId == ID_EDIT_REVFIND);
            }
            else
            {

                // new search - restore the previous search term to the find box
                SetWindowTextA(findBox, lastSearch.term.c_str());
                SendMessage(findBox, EM_SETSEL, 0, lastSearch.term.size());

                // set default flags + forward/reverse
                lastSearch.flags.Clear();
                lastSearch.flags.reverse = (ctlCmdId == ID_EDIT_REVFIND);

                // set focus on the find box
                SetFocus(findBox);
            }
            return true;

        case ID_EDIT_FINDNEXT:
        case ID_EDIT_FINDPREV:
            FindNextPrev(ctlCmdId == ID_EDIT_FINDPREV);
            return true;

        case ID_EDIT_FINDREPLACE:
            // show the find/replace dialog
            if (findReplaceDlg != nullptr)
            {
                // the dialog is already open - set focus
                SetFocus(findReplaceDlg->GetHWND());
            }
            else
            {
                // create the dialog
                findReplaceDlg = new FindReplaceDialog(hInstance, hwnd);

                // share a reference with the dialog
                findReplaceDlg->sharedSelfRef.reset(findReplaceDlg);
                findReplaceDlgSharedRef = findReplaceDlg->sharedSelfRef;

                // open it
                findReplaceDlg->OpenModeless(DLG_FINDREPLACE, hwnd);

                // restore the last search settings
                findReplaceDlg->SetFlags(findReplaceFlags);
                findReplaceDlg->SetFindWhat(findWhat.c_str());
                findReplaceDlg->SetReplaceWith(replaceWith.c_str());
            }
            return true;

        case ID_EDIT_GOTOLINENUMBER:
            // show the go-to-line-number dialog
            GoToLineNum();
            return true;

        case ID_FILE_EXIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return true;

        case ID_FILE_CHECKERRORS:
            // syntax check
            if (CheckSyntax())
            {
                gApp.AddTimedStatusMessage("Success - JSON checks passed with no syntax or schema errors detected",
                    HRGB(0xFFFFFF), HRGB(0x008000), 5000);
            }
            return true;

        case ID_HELP_HELP:
            // If a call tip is active, show the JSON reference for the tip item.
            // Otherwise show the config editor help.
            if (CallSci(SCI_CALLTIPACTIVE) && callTipHelpLink != nullptr)
                ShowHelpFile("JSONConfigRef.htm", callTipHelpLink);
            else
                ShowHelpFile("ConfigEditor.htm");
            return true;

        case ID_HELP_ON_SELECTION:
            if (curLocHelpLink != nullptr)
                ShowHelpFile("JSONConfigRef.htm", curLocHelpLink);
            return true;

        case ID_HELP_JSONSYNTAXGUIDE:
            ShowHelpFile("ConfigFileFormat.htm");
            return true;

        case ID_HELP_PROPERTYREFERENCE:
            ShowHelpFile("JSONConfigRef.htm");
            return true;

        case ID_EDIT_PAGEUP:
            CallSci(SCI_PAGEUP);
            return true;

        case ID_EDIT_PAGEDOWN:
            CallSci(SCI_PAGEDOWN);
            return true;

        case ID_EDIT_LINESCROLLUP:
            CallSci(SCI_LINESCROLLUP);
            return true;

        case ID_EDIT_LINESCROLLDOWN:
            CallSci(SCI_LINESCROLLDOWN);
            return true;

        case ID_EDIT_GOTOTOP:
            CallSci(SCI_DOCUMENTSTART);
            return true;

        case ID_EDIT_GOTOEND:
            CallSci(SCI_DOCUMENTEND);
            return true;

        case ID_EDIT_SWAPANCHOR:
            {
                INT_PTR anchor = CallSci(SCI_GETANCHOR);
                INT_PTR caret = CallSci(SCI_GETCURRENTPOS);
                CallSci(SCI_SETCURRENTPOS, anchor);
                CallSci(SCI_SETANCHOR, caret);
            }
            return true;

        case ID_EDIT_SETANCHOR:
            // set the anchor to current position, and set Move-Extends-Selection mode
            CallSci(SCI_SETANCHOR, CallSci(SCI_GETCURRENTPOS));
            CallSci(SCI_SETMOVEEXTENDSSELECTION, 1);
            return true;

        case ID_EDIT_CANCELMODE:
            // cancel Move-Extends-Selection mode if active, and move anchor to here
            if (CallSci(SCI_GETANCHOR) != CallSci(SCI_GETCURRENTPOS) && CallSci(SCI_GETMOVEEXTENDSSELECTION))
            {
                CallSci(SCI_SETANCHOR, CallSci(SCI_GETCURRENTPOS));
                CallSci(SCI_SETMOVEEXTENDSSELECTION, 0);
            }

            // also pass this along to Sci's internal CANCEL command, for any
            // other mode cancellation that applies
            CallSci(SCI_CANCEL);

            // if the Find box is active, cancel the search by sending an Escape
            // key to the control
            if (GetFocus() == findBox)
                SendMessage(findBox, WM_KEYDOWN, VK_ESCAPE, 0);
            break;

        case ID_EDIT_OPENLINE:
            // insert a newline right of the cursor
            CallSci(SCI_INSERTTEXT, -1, reinterpret_cast<INT_PTR>("\n"));
            break;

        case ID_EDIT_TRANSPOSECHAR:
            // transpose the characters on either side of the caret
            {
                INT_PTR pos = CallSci(SCI_GETCURRENTPOS);
                if (pos < CallSci(SCI_GETLENGTH))
                {
                    INT_PTR chNxt = CallSci(SCI_GETCHARAT, pos);
                    if (chNxt != '\r' && chNxt != '\n')
                    {
                        int chPrv = CallSci(SCI_GETCHARAT, pos - 1);
                        if (chPrv != 0 && chPrv != '\r' && chPrv != '\n')
                        {
                            char buf[2] ={ static_cast<char>(chPrv), 0 };
                            CallSci(SCI_BEGINUNDOACTION);
                            CallSci(SCI_DELETEBACK);
                            CallSci(SCI_CHARRIGHT);
                            CallSci(SCI_INSERTTEXT, pos, reinterpret_cast<INT_PTR>(buf));
                            CallSci(SCI_ENDUNDOACTION);
                        }
                    }
                }
            }
            return true;

        case ID_EDIT_QUOTECHAR:
            // set the quoted-char flag, to be processed in the accelerator translator
            quotedCharPending = true;
            break;

        case ID_NAV_DROP_BUTTON:
            ShowNavDropMenu();
            return true;
        }
        break;

    case EN_CHANGE:
        // edit box change
        if (hwndCtl == findBox)
        {
            // get the new search term
            char buf[256];
            GetWindowTextA(findBox, buf, _countof(buf));

            // update the search
            UpdateSearch(buf, lastSearch.flags);
        }
        break;

    case EN_SETFOCUS:
        // open the search
        searchOpen = true;
        break;

    case EN_KILLFOCUS:
        // if the search is still open, end it
        if (searchOpen)
            CommitSearch(CommitSearchReason::KillFocus, true);
        break;
    }

    // not handled
    return __super::OnCommand(notifyCode, ctlCmdId, hwndCtl, lresult);
}

bool ConfigEditorWin::OnNotify(WPARAM id, NMHDR *nmhdr, LRESULT &lresult)
{
    // handle Scintilla notifications
    if (nmhdr->hwndFrom == sciWin)
    {
        auto *nm = reinterpret_cast<SCNotification*>(nmhdr);
        switch (nmhdr->code)
        {
        case SCN_SAVEPOINTLEFT:
            // document is modified
            isModified = true;
            break;

        case SCN_SAVEPOINTREACHED:
            // document unmodified
            isModified = false;
            break;

        case SCN_CHARADDED:
            // character added - perform auto-indent and auto brace matching
            AutoIndent(nm->ch);

            // check if they're typing something that could trigger auto-complete
            if (isalpha(nm->ch) || isdigit(nm->ch) || nm->ch == '_' || nm->ch == '$' || nm->ch == '"')
                openAutoCompletePending = true;
            break;

        case SCN_UPDATEUI:
            // UI change - selection position, styling, or contents changed
            {
                // Highlight braces
                auto CheckBrace = [this](INT_PTR pos)
                {
                    // check for a brace at the given position
                    int ch = static_cast<int>(CallSci(SCI_GETCHARAT, pos));
                    if (pos >= 0 && ch != 0 && strchr("[{()}]", static_cast<char>(CallSci(SCI_GETCHARAT, pos))) != nullptr)
                    {
                        // found a brace - look for a match
                        INT_PTR matchPos = CallSci(SCI_BRACEMATCH, pos);
                        if (matchPos != -1)
                        {
                            // found a match - highlight both braces
                            CallSci(SCI_BRACEHIGHLIGHT, pos, matchPos);
                            return true;
                        }
                        else
                        {
                            // no match - set the mismatched highlight for the character
                            CallSci(SCI_BRACEBADLIGHT, pos);
                            return true;
                        }
                    }

                    // no highlighting set
                    return false;
                };

                // check the current and previous characters for brace matches;
                // if not found, simply turn off brace highlighting
                if (auto pos = CallSci(SCI_GETCURRENTPOS); !CheckBrace(pos) && !CheckBrace(pos - 1))
                    CallSci(SCI_BRACEHIGHLIGHT, -1, -1);

                // check if the document was modified
                if ((nm->updated & SC_UPDATE_CONTENT) != 0)
                {
                    // schedule a JSON reparse
                    parsePending = true;
                    SetTimer(hwnd, TIMER_ID_PARSE, 100, NULL);
                }
                else if (!parsePending)
                {
                    // no change in the contents, and no parse pending, so just update
                    // the position in the current parse tree
                    FindParsePosition();
                }
            }
            break;

        case SCN_DWELLSTART:
            // if we're not showing another popup, show a call tip
            if (!CallSci(SCI_AUTOCACTIVE) && !CallSci(SCI_CALLTIPACTIVE))
            {
                // find the parse position of the hover point
                JSONParserExt::Path path;
                const JSONSchemaObj *obj;
                const JSONParser::Value *sc;
                const JSONSchemaObj::Prop *prop;
                JSONParser::Value *val;
                INT_PTR pos = CallSci(SCI_CHARPOSITIONFROMPOINT, nm->x, nm->y);
                FindParsePosition(pos, path, obj, sc, prop, val);

                // if we found a property, show its summary as the call tip
                if (prop != nullptr && prop->summary != nullptr)
                {
                    CallSci(SCI_CALLTIPSHOW, pos, reinterpret_cast<INT_PTR>(prop->summary));
                    callTipHelpLink = prop->link;
                }
            }
            break;

        case SCN_DWELLEND:
            // cancel the tip
            if (CallSci(SCI_CALLTIPACTIVE))
                CallSci(SCI_CALLTIPCANCEL);
            break;

        case SCN_CALLTIPCLICK:
            // Go to the help link.  Note that we can't currently access this, since
            // moving the mouse (to position it over the tip window) will end the "dwell"
            // period and cancel the link.  We'd need to allow the mouse to move out of
            // the text and over the tip window to let the user click on the tip, but
            // I don't see a good way to do that, since Scintilla doesn't have an API
            // to get the location of the tip window.  This is just in case we find a
            // way to allow this in the future.  (For now, the user can press F1 to
            // jump to the current tip help link.)
            if (callTipHelpLink != nullptr)
                ShowHelpFile("JSONConfigRef.htm", curLocHelpLink);
            break;
        }
    }

    // inherit default handling
    return __super::OnNotify(id, nmhdr, lresult);
}


// --------------------------------------------------------------------------
//
// Auto-indent
//

void ConfigEditorWin::AutoIndent(int chAdded)
{
    // indentation increment, in columns
    int indentSize = 4;

    // skip a newline sequence backwards
    auto SkipNewlineBack = [this](int pos)
    {
        if (pos > 0 && CallSci(SCI_GETCHARAT, pos) == '\n' && CallSci(SCI_GETCHARAT, pos-1) == '\r')
            pos -= 1;
        return pos;
    };

    // check the character added
    switch (chAdded)
    {
    case '\n':
        // return character - indent the new line to match the old line,
        // or indent one more level if the last line ends with an open
        // brace or paren
        {
            // get the current line number; if on the first line, do nothing
            auto curPos = CallSci(SCI_GETCURRENTPOS);
            auto curLine = CallSci(SCI_LINEFROMPOSITION, curPos);
            if (curLine > 0)
            {
                // get previous line's indentation
                INT_PTR indent = CallSci(SCI_GETLINEINDENTATION, curLine - 1);

                // get the line-ending character
                auto lineEndPos = SkipNewlineBack(curPos - 1);
                char lineEndChar = 0;
                if (curPos > 0)
                {
                    // if it's an open brace/paren, indent one level
                    lineEndChar = static_cast<char>(CallSci(SCI_GETCHARAT, lineEndPos - 1));
                    if (strchr("{[(", lineEndChar) != nullptr)
                        indent += indentSize;
                }

                // if the next non-space character on the same line is a close
                // brace or paren, un-indent one level
                int nextChar = 0;
                INT_PTR nextCharPos = curPos;
                for (auto len = CallSci(SCI_GETLENGTH) ; nextCharPos < len ; ++nextCharPos)
                {
                    // stop at newline or non-space
                    nextChar = static_cast<int>(CallSci(SCI_GETCHARAT, nextCharPos));
                    if (nextChar == '\n' || nextChar == '\r' || !isspace(nextChar))
                        break;
                }

                // if it's a close delimiter, indent to match the matching delimiter
                switch (nextChar)
                {
                case '}':
                case ']':
                case ')':
                    // close delimiter - match the open delimiter; if not found, just
                    // un-indent one level
                    if (auto newIndent = GetMatchingIndent(nextCharPos); newIndent >= 0)
                        indent = newIndent;
                    else
                        indent -= indentSize;
                    break;
                }

                // set the new indentation
                CallSci(SCI_SETLINEINDENTATION, curLine, indent);

                // if we inserted the newline between an empty delimiter pair, 
                // insert another blank line, indented one level further
                if ((lineEndChar == '{' && nextChar == '}')
                    || (lineEndChar == '[' && nextChar == ']')
                    || (lineEndChar == '(' && nextChar == ')'))
                {
                    // insert another blank line
                    CallSci(SCI_INSERTTEXT, -1, reinterpret_cast<INT_PTR>("\r"));
                    CallSci(SCI_GOTOPOS, curPos + 2);

                    // set the indentation one level up
                    indent += indentSize;
                    CallSci(SCI_SETLINEINDENTATION, curLine, indent);
                }

                // position the cursor at the indentation point
                CallSci(SCI_GOTOPOS, CallSci(SCI_GETLINEINDENTPOSITION, curLine));
            }
        }
        break;

    case '{':
        CallSci(SCI_INSERTTEXT, -1, reinterpret_cast<INT_PTR>("}"));
        break;

    case '[':
        CallSci(SCI_INSERTTEXT, -1, reinterpret_cast<INT_PTR>("],"));
        break;

    case '(':
        CallSci(SCI_INSERTTEXT, -1, reinterpret_cast<INT_PTR>(")"));
        break;

    case '"':
        // if we're not already in a string, insert a matching close quote
        switch (CallSci(SCI_GETSTYLEAT, CallSci(SCI_GETCURRENTPOS)))
        {
        case SCE_JSON_STRING:
        case SCE_JSON_STRINGEOL:
            // already in a string - leave well enough alone
            break;

        default:
            // insert a matching close quote
            CallSci(SCI_INSERTTEXT, -1, reinterpret_cast<INT_PTR>("\""));
            break;
        }
        break;

    case ']':
    case '}':
    case ')':
        // Close brace, bracket, or paren.  If this is the only character
        // on the line, match the indent at the open delimiter.
        {
            INT_PTR curPos = CallSci(SCI_GETCURRENTPOS) - 1;
            INT_PTR ch = CallSci(SCI_GETCHARAT, curPos);
            INT_PTR curLine = CallSci(SCI_LINEFROMPOSITION, curPos);
            INT_PTR curIndentPos = CallSci(SCI_GETLINEINDENTPOSITION, curLine);
            if (curPos == curIndentPos)
                IndentCloseDelim(curPos);
        }
        break;
    }
}

void ConfigEditorWin::IndentCloseDelim(INT_PTR pos)
{
    // find the matching brace
    INT_PTR indent = GetMatchingIndent(pos);
    if (indent >= 0)
        CallSci(SCI_SETLINEINDENTATION, CallSci(SCI_LINEFROMPOSITION, pos), indent);
}

INT_PTR ConfigEditorWin::GetMatchingIndent(INT_PTR pos)
{
    // presume the indentation will be unknown
    INT_PTR indent = -1;

    // temporarily set the style of the delimiter to 'operator'
    INT_PTR oldStyle = CallSci(SCI_GETSTYLEAT, pos);
    CallSci(SCI_STARTSTYLING, pos);
    CallSci(SCI_SETSTYLING, 1, SCE_JSON_OPERATOR);
    
    // find the match; if found, use its indentation
    INT_PTR matchPos = CallSci(SCI_BRACEMATCH, pos);
    if (matchPos >= 0)
        indent = CallSci(SCI_GETLINEINDENTATION, CallSci(SCI_LINEFROMPOSITION, matchPos));

    // restore the old styling
    CallSci(SCI_STARTSTYLING, pos);
    CallSci(SCI_SETSTYLING, 1, oldStyle);

    // return the result
    return indent;
}

// --------------------------------------------------------------------------
//
// Auto-completion
//
void ConfigEditorWin::CheckBeginAutoComplete()
{
    // only proceed if we just had the right kind of key input
    if (!openAutoCompletePending)
        return;

    // consume the pending open
    openAutoCompletePending = false;

    // skip if auto-complete is disabled
    if (!options.enableAutoComplete)
        return;

    // if auto-complete is already open, leave it open
    if (CallSci(SCI_AUTOCACTIVE))
        return;

    // skip auto-complete when there's a non-empty selection, or if we're
    // at the very beginning of the document (with nothing preceding to
    // include in the auto-select)
    INT_PTR curPos = CallSci(SCI_GETCURRENTPOS);
    INT_PTR curSelStart = CallSci(SCI_GETSELECTIONSTART);
    if (curPos != curSelStart || curPos == 0)
        return;

    // check the context
    switch (CallSci(SCI_GETSTYLEAT, curPos - 1))
    {
    case SCE_JSON_BLOCKCOMMENT:
    case SCE_JSON_LINECOMMENT:
        // don't auto-complete in comments
        break;

    default:
        // Try auto-complete anywhere else.
        if (curLocSchemaObj != nullptr)
        {
            // make sure the next character isn't part of the same word;
            // we don't want auto-complete to pop up when we're editing in
            // the middle of a word
            static const auto IsSymChar = [](INT_PTR ch) { return isalpha(ch) || isdigit(ch) || ch == '_' || ch == '$'; };
            if (curPos < CallSci(SCI_GETLENGTH))
            {
                int chNxt = CallSci(SCI_GETCHARAT, curPos);
                if (IsSymChar(chNxt))
                    return;
            }

            // scan back to the start of the word
            int len = 0;
            char nameBuf[64];
            char *name = &nameBuf[_countof(nameBuf) - 1];
            *name = 0;
            char stopChar = 0;
            for (INT_PTR i = curPos - 1 ; i >= 0 && name > &nameBuf[0] ; --i, ++len)
            {
                // get the next character
                INT_PTR prvCh = CallSci(SCI_GETCHARAT, i);
                if (!IsSymChar(prvCh))
                {
                    stopChar = prvCh;
                    break;
                }

                // accumulate the name
                *--name = static_cast<char>(prvCh);
            }

            // proceed only if we've started typing something
            if (len > 0 || stopChar == '"')
            {
                // check if we're in the value portion
                bool inVal = false;
                if (curLocSchemaVal != nullptr)
                {
                    const char *txtPtr = parsedText.data() + curPos;
                    inVal = (txtPtr >= curLocSchemaVal->startTok.srcTxt && txtPtr <= curLocSchemaVal->endTok.SrcEnd());
                }

                // generate the appropriate popup list
                const char *stops = nullptr; 
                std::string list;
                if (inVal)
                {
                    // it's in the value - show the current property's enumerated value list, if any
                    if (curLocSchemaProp->enumValues.size() != 0)
                    {
                        for (auto &val : curLocSchemaProp->enumValues)
                        {
                            list.append(val);
                            list.append(" ");
                        }

                        // set string stops
                        stops = "\"\n\r";
                    }
                    else if (curLocSchemaProp->IsBool())
                    {
                        // simple boolean value
                        list = "true false";
                        stops = " \n\r,:{}[]()";
                    }
                }
                else
                {
                    // it's in the property name - show the current object property list
                    // set up a list for the current object context
                    if (curLocSchemaObj->props.size() != 0)
                    {
                        // add properties for the current schema object
                        for (auto &prop : curLocSchemaObj->props)
                        {
                            list.append(prop.name);
                            list.append(" ");
                        }

                        // add properties for the current schema subclass object, if any
                        if (curLocSchemaSubclass != nullptr && curLocSchemaSubclass->IsString())
                        {
                            auto &scs = curLocSchemaObj->subclassObjects;
                            auto it = std::find_if(scs.begin(), scs.end(), [this](const JSONSchemaObj *o) {
                                return o->subclassIdPropVal != nullptr && curLocSchemaSubclass->string == o->subclassIdPropVal; });
                            if (it != scs.end())
                            {
                                for (auto &prop : (*it)->props)
                                {
                                    list.append(prop.name);
                                    list.append(" ");
                                }
                            }

                            // set lexical stops for property names
                            stops = ".:{}[](),\n\r ";
                        }
                    }
                }

                // show the popup
                if (stops != nullptr)
                    CallSci(SCI_AUTOCSTOPS, 0, reinterpret_cast<INT_PTR>(stops));
                if (list.size() != 0)
                    CallSci(SCI_AUTOCSHOW, len, reinterpret_cast<INT_PTR>(list.c_str()));
            }
        }
        break;
    }
}

// --------------------------------------------------------------------------
//
// Preferences
//

void ConfigEditorWin::Options::Load()
{
    // get the root 'editor' object
    const auto *opts = gApp.settingsJSON.Get("editor");

    // read the options values
    indentWithTabs = opts->Get("indentWithTabs")->Bool(true);
    tabSize = opts->Get("tabSize")->Int(8);
    emacsKeyBindings = opts->Get("emacsMode")->Bool();
    confirmSaves = opts->Get("confirmSaves")->Bool(true);
    checkRulesBeforeSave = opts->Get("checkRulesBeforeSave")->Bool(true);
    enableAutoComplete = opts->Get("enableAutoComplete")->Bool(true);
    autoBackupEnabled = opts->Get("autoBackup")->Get("enabled")->Bool(true);
    autoBackupFolder = opts->Get("autoBackup")->Get("folder")->String("$(SettingsDir)\\AutoBackup");
    autoBackupCleanupEnabled = opts->Get("autoBackup.cleanup.enabled")->Bool(true);
    autoBackupCleanupDays = opts->Get("autoBackup.cleanup.keepForDays")->Int(30);
}

void ConfigEditorWin::Options::Store()
{
    // make sure our root 'editor' container exists and is an object
    auto &json = gApp.settingsJSON;
    auto *opts = json.SetObject("editor");

    // set the individual option properties
    json.SetBool(opts, "indentWithTabs", indentWithTabs);
    json.SetNum(opts, "tabSize", tabSize);
    json.SetBool(opts, "emacsMode", emacsKeyBindings);
    json.SetBool(opts, "confirmSaves", confirmSaves);
    json.SetBool(opts, "checkRulesBeforeSave", checkRulesBeforeSave);
    json.SetBool(opts, "enableAutoComplete", enableAutoComplete);
    json.SetBool(opts, "autoBackup.enabled", autoBackupEnabled);
    json.SetStr(opts, "autoBackup.folder", autoBackupFolder);
    json.SetBool(opts, "autoBackup.cleanup.enabled", autoBackupCleanupEnabled);
    json.SetNum(opts, "autoBackup.cleanup.keepForDays", autoBackupCleanupDays);
}

void ConfigEditorWin::ApplyOptions()
{
    // set the keyboard bindings for the desired mode
    SetEmacsMode(options.emacsKeyBindings);

    // set the tab mode
    CallSci(SCI_SETTABWIDTH, options.tabSize);
    CallSci(SCI_SETUSETABS, options.indentWithTabs ? 1 : 0);
}

void ConfigEditorWin::ShowPrefsDialog()
{
    class PrefsDlg : public Dialog
    {
    public:
        PrefsDlg(ConfigEditorWin *win) : Dialog(win->hInstance), win(win) { }
        ConfigEditorWin *win;

        virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override
        {
            // initialize controls from the json options
            auto &opts = win->options;
            CheckRadioButton(hDlg, IDC_RB_INDENT_WITH_TABS, IDC_RB_INDENT_WITH_SPACES,
                opts.indentWithTabs ? IDC_RB_INDENT_WITH_TABS : IDC_RB_INDENT_WITH_SPACES);
            CheckRadioButton(hDlg, IDC_RB_WINDOWS_KEYS, IDC_RB_EMACS_KEYS,
                opts.emacsKeyBindings ? IDC_RB_EMACS_KEYS : IDC_RB_WINDOWS_KEYS);
            SetDlgItemInt(hDlg, IDC_EDIT_TAB_SIZE, opts.tabSize, FALSE);
            CheckDlgButton(hDlg, IDC_CK_CONFIRM_SAVES, opts.confirmSaves ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_CK_CHECK_RULES_BEFORE_SAVE, opts.checkRulesBeforeSave ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_CK_ENABLEAUTOCOMPLETE, opts.enableAutoComplete ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_CK_AUTOBACKUPENABLE, opts.autoBackupEnabled ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemTextA(hDlg, IDC_EDIT_AUTOBACKUPFOLDER, opts.autoBackupFolder.c_str());
            CheckDlgButton(hDlg, IDC_CK_AUTOBACKUPCLEANUPENABLE, opts.autoBackupCleanupEnabled ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(hDlg, IDC_EDIT_AUTOBACKUPCLEANUPDAYS, opts.autoBackupCleanupDays, FALSE);

            // do the base class work
            return __super::OnInitDialog(wparam, lparam);
        }

        virtual bool OnCommand(UINT command, UINT code, HWND ctlFrom)
        {
            switch (command)
            {
            case IDOK:
                // apply changes
                {
                    // load the changes
                    auto &opts = win->options;
                    opts.indentWithTabs = (IsDlgButtonChecked(hDlg, IDC_RB_INDENT_WITH_TABS) == BST_CHECKED);
                    opts.emacsKeyBindings = (IsDlgButtonChecked(hDlg, IDC_RB_EMACS_KEYS) == BST_CHECKED);
                    opts.tabSize = static_cast<int>(GetDlgItemInt(hDlg, IDC_EDIT_TAB_SIZE, NULL, FALSE));
                    opts.confirmSaves = (IsDlgButtonChecked(hDlg, IDC_CK_CONFIRM_SAVES) == BST_CHECKED);
                    opts.checkRulesBeforeSave = (IsDlgButtonChecked(hDlg, IDC_CK_CHECK_RULES_BEFORE_SAVE) == BST_CHECKED);
                    opts.enableAutoComplete = (IsDlgButtonChecked(hDlg, IDC_CK_ENABLEAUTOCOMPLETE) == BST_CHECKED);
                    opts.autoBackupEnabled = (IsDlgButtonChecked(hDlg, IDC_CK_AUTOBACKUPENABLE) == BST_CHECKED);
                    opts.autoBackupFolder = GetDlgItemTextA(IDC_EDIT_AUTOBACKUPFOLDER);
                    opts.autoBackupCleanupEnabled = (IsDlgButtonChecked(hDlg, IDC_CK_AUTOBACKUPCLEANUPENABLE) == BST_CHECKED);
                    opts.autoBackupCleanupDays = static_cast<int>(GetDlgItemInt(hDlg, IDC_EDIT_AUTOBACKUPCLEANUPDAYS, NULL, FALSE));

                    // apply the new options
                    opts.Store();
                    win->ApplyOptions();
                }

                // continue on to inherit the default handling to dismiss the dialog
                break;

            case IDC_BTN_SELECT_FOLDER:
                // show a folder selection dialog
                {
                    // expand the current directory
                    std::wstring oldDir = STRINGToTSTRING(ExpandAutoCompleteDir(GetDlgItemTextA(IDC_EDIT_AUTOBACKUPFOLDER)));

                    // show the folder picker
                    GetFileNameDlg fd(_T("Select Auto-Backup Folder"), 0, _T(""), _T(""), oldDir.c_str(), oldDir.c_str());
                    if (fd.SelectFolder(hDlg))
                    {
                        // get the folder
                        char path[MAX_PATH];
                        sprintf_s(path, "%" _TSFMT, fd.GetFilename());

                        // If the folder starts with $(SettingsDir) or $(InstallDir),
                        // substitute the variable name for the hardcoded path.  Note
                        // that we deliberately try $(SettingsDir) first, so that it
                        // takes precedence if it expands to the same path as
                        // $(InstallDir).  It's preferable to base the path on the
                        // settings file location, since a user who's overriding that
                        // probably wants to keep all of their private data in that
                        // alternate location.
                        auto PathSub = [&path, this](const char *varName)
                        {
                            std::string varDir = ExpandAutoCompleteDir(varName);
                            if (_strnicmp(path, varDir.c_str(), varDir.size()) == 0
                                && (path[varDir.size()] == 0 || path[varDir.size()] == '\\'))
                            {
                                char newPath[MAX_PATH];
                                sprintf_s(newPath, "%s%s", varName, &path[varDir.size()]);
                                strcpy_s(path, newPath);
                            }
                        };
                        PathSub("$(SettingsDir)");
                        PathSub("$(InstallDir)");

                        // set the path in the text box
                        SetDlgItemTextA(hDlg, IDC_EDIT_AUTOBACKUPFOLDER, path);
                    }
                }
                return true;
            }

            // inherit the default handling
            return __super::OnCommand(command, code, ctlFrom);
        }
    };
    PrefsDlg dlg(this);
    dlg.Show(IDD_EDITOR_PREFS, GetDialogOwner());
}

// --------------------------------------------------------------------------
//
// Find and Replace
//

ConfigEditorWin::SearchFlags ConfigEditorWin::FindReplaceDialog::GetFlags() const
{
    SearchFlags flags;
    flags.wholeWord = (IsDlgButtonChecked(hDlg, IDC_CK_WORDMATCH) == BST_CHECKED);
    flags.matchCase = (IsDlgButtonChecked(hDlg, IDC_CK_CASEMATCH) == BST_CHECKED);
    flags.regex = (IsDlgButtonChecked(hDlg, IDC_CK_REGEX) == BST_CHECKED);
    flags.preserveCase = (IsDlgButtonChecked(hDlg, IDC_CK_PRESERVECASE) == BST_CHECKED);
    return flags;
}

void ConfigEditorWin::FindReplaceDialog::SetFlags(SearchFlags flags)
{
    CheckDlgButton(hDlg, IDC_CK_WORDMATCH, flags.wholeWord ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_CK_CASEMATCH, flags.matchCase ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_CK_REGEX, flags.regex ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_CK_PRESERVECASE, flags.preserveCase ? BST_CHECKED : BST_UNCHECKED);
    CheckRegexSyntax();
}

INT_PTR ConfigEditorWin::FindReplaceDialog::Proc(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_BTN_FINDNEXT:
        case IDC_BTN_FINDPREV:
        case IDC_BTN_REPLACE:
        case IDC_BTN_REPLACEALL:
            // forward all of these to the parent
            SendMessage(hwndCommand, MSG_FINDREPLACEDLG, wParam, 0);
            return 0;

        case IDCLOSE:
        case IDCANCEL:
            // close the dialog
            SendMessage(hwndCommand, MSG_FINDREPLACEDLG, IDCLOSE, 0);
            DestroyWindow(hDlg);
            break;

        case IDC_EDIT_FINDWHAT:
            // On changing the Find What box, if we're in regular expression
            // mode, test the regex for validity
            if (HIWORD(wParam) == EN_CHANGE)
                CheckRegexSyntax();
            break;

        case IDC_CK_REGEX:
            // update the regex syntax checking
            CheckRegexSyntax();
            break;
        }
        break;

    case WM_CTLCOLOREDIT:
        // for the Find What box, if there's a regex error, set distinctive
        // coloring if it contains a regex syntax error
        if (regexSyntaxError && reinterpret_cast<HWND>(lParam) == GetDlgItem(IDC_EDIT_FINDWHAT))
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, errorColor);
            return reinterpret_cast<INT_PTR>(errorBrush.hBrush);
        }
        break;

    case WM_CLOSE:
        SendMessage(hwndCommand, MSG_FINDREPLACEDLG, IDCLOSE, 0);
        DestroyWindow(hDlg);
        break;
    }

    return __super::Proc(message, wParam, lParam);
}

void ConfigEditorWin::FindReplaceDialog::CheckRegexSyntax()
{
    // note the original status, and assume it's good
    bool wasError = regexSyntaxError;
    regexSyntaxError = false;

    // if it's a regex, text its syntax
    if (IsDlgButtonChecked(hDlg, IDC_CK_REGEX) == BST_CHECKED)
    {
        try
        {
            auto r = GetDlgItemTextA(IDC_EDIT_FINDWHAT);
            std::regex re(r);
        }
        catch (std::regex_error &)
        {
            regexSyntaxError = true;
        }
    }

    // if it changed, redraw the find box
    if (regexSyntaxError != wasError)
        InvalidateRect(GetDlgItem(IDC_EDIT_FINDWHAT), NULL, TRUE);
}

INT_PTR ConfigEditorWin::FindReplaceDialog::OnInitDialog(WPARAM wparam, LPARAM lparam)
{
    return __super::OnInitDialog(wparam, lparam);
}

std::string ConfigEditorWin::FindReplaceDialog::GetFindWhat() const
{
    return this->GetDlgItemTextA(IDC_EDIT_FINDWHAT);
}

std::string ConfigEditorWin::FindReplaceDialog::GetReplaceWith() const
{
    return this->GetDlgItemTextA(IDC_EDIT_REPLACEWITH);
}

void ConfigEditorWin::FindReplaceDialog::SetFindWhat(const char *s)
{
    SetDlgItemTextA(hDlg, IDC_EDIT_FINDWHAT, s);
    CheckRegexSyntax();
}

void ConfigEditorWin::FindReplaceDialog::SetReplaceWith(const char *s)
{
    SetDlgItemTextA(hDlg, IDC_EDIT_REPLACEWITH, s);
}

LRESULT ConfigEditorWin::ControlSubclassProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass)
{
    // do some special handling for some of the controls
    if (hwnd == findBox)
    {
        // Find Box
        switch (msg)
        {
        case WM_KEYDOWN:
            switch (wparam)
            {
            case VK_ESCAPE:
                // abort the search and return focus to the Scintilla window
                CommitSearch(CommitSearchReason::Cancel, true);

                // restore the scroll position
                CallSci(SCI_SCROLLCARET);

                // forget the search term
                lastSearch.term.clear();
                lastSearch.flags.Clear();
                return true;

            case VK_RETURN:
                // save the search term for next time
                {
                    char buf[256];
                    GetWindowTextA(findBox, buf, _countof(buf));
                    lastSearch.term = buf;
                }

                // end the search, selecting the result
                CommitSearch(CommitSearchReason::Accept, true);
                return true;
            }
            break;

        case WM_CHAR:
            // suppress the bell on an escape key press
            if (wparam == 27)
                return true;
            break;
        }
    }
    else if (errorPanelVisible && hwnd == errorPanel)
    {
        // Error panel
        switch (msg)
        {
        case WM_LBUTTONDBLCLK:
            // Click in error panel - go to the line containing the error
            {
                // find the line number containing the point
                WORD lineNum = HIWORD(SendMessage(errorPanel, EM_CHARFROMPOS, 0, lparam));

                // jump to the error line
                GoToError(lineNum);

                // prevent the editor from seeing the click
                return true;
            }
            break;
        }
    }

    // use the base class handling
    return __super::ControlSubclassProc(hwnd, msg, wparam, lparam, idSubclass);
}

void ConfigEditorWin::SetFindHighlight(INT_PTR start, INT_PTR end)
{
    // select the Find indicator
    CallSci(SCI_SETINDICATORCURRENT, INDICATOR_FIND);

    // remove any existing range
    if (findStart != findEnd)
        CallSci(SCI_INDICATORCLEARRANGE, findStart, findEnd - findStart);

    // fill the new range
    findStart = start;
    findEnd = end;
    if (start != end)
    {
        // set highlighting
        CallSci(SCI_INDICATORFILLRANGE, start, end - start);

        // make sure it's in range
        INT_PTR startLine = CallSci(SCI_LINEFROMPOSITION, start);
        INT_PTR endLine = CallSci(SCI_LINEFROMPOSITION, end);
        INT_PTR topVisLine = CallSci(SCI_GETFIRSTVISIBLELINE);
        INT_PTR linesOnScreen = CallSci(SCI_LINESONSCREEN);

        // if it's out of view, re-center on the find position
        if (endLine < topVisLine || startLine > topVisLine + linesOnScreen)
        {
            // try to center the middle line, but make sure the start line
            // is in view
            INT_PTR newStartLine = (startLine + endLine)/2 - linesOnScreen/2;
            newStartLine = min(startLine, newStartLine);
            newStartLine = max(newStartLine, 0);
            
            // scroll there
            CallSci(SCI_LINESCROLL, 0, newStartLine - topVisLine);
        }
        else
        {
            // it's already in view, but ask Sci to make sure anyway in case
            // of rounding error for partial lines
            CallSci(SCI_SCROLLRANGE, start, end);
        }
    }
}

uint32_t ConfigEditorWin::SearchFlags::SciFlags() const
{
    int flags = 0;
    if (matchCase) flags |= SCFIND_MATCHCASE;
    if (wholeWord) flags |= SCFIND_WHOLEWORD;
    if (regex) flags |= SCFIND_REGEXP | SCFIND_CXX11REGEX;
    return flags;
}

void ConfigEditorWin::FindNextPrev(bool reverse)
{
    // set default flags + forward/reverse
    lastSearch.flags.Clear();
    lastSearch.flags.reverse = reverse;
    
    // If the edit box isn't empty, use the search term from the box,
    // otherwise repeat the last search
    char buf[256];
    GetWindowTextA(findBox, buf, _countof(buf));
    bool found = false;
    if (buf[0] != 0)
    {
        // use the term in the box
        found = UpdateSearch(buf, lastSearch.flags);
        lastSearch.term = buf;
    }
    else if (lastSearch.term.size() != 0)
    {
        // repeat the prior search
        found = UpdateSearch(lastSearch.term.c_str(), lastSearch.flags);
    }

    // commit the new match position, leaving focus where it is
    if (found)
        CommitSearch(CommitSearchReason::Accept, false);
}

bool ConfigEditorWin::UpdateSearch(const char *term, SearchFlags flags)
{
    // set flags
    CallSci(SCI_SETSEARCHFLAGS, flags.SciFlags());
    INT_PTR selStart = CallSci(SCI_GETSELECTIONSTART);
    INT_PTR selEnd = CallSci(SCI_GETSELECTIONEND);

    // start with a search from here to end/start of document, depending on direction
    if (flags.reverse)
        CallSci(SCI_SETTARGETRANGE, selStart, 0);
    else
        CallSci(SCI_SETTARGETRANGE, selEnd, CallSci(SCI_GETLENGTH));

    // try the search
    if (CallSci(SCI_SEARCHINTARGET, strlen(term), reinterpret_cast<INT_PTR>(term)) >= 0)
    {
        // success - highlight the result
        SetFindHighlight(CallSci(SCI_GETTARGETSTART), CallSci(SCI_GETTARGETEND));
        return true;
    }

    // not found; if allowed, try wrapping
    if (flags.wrap)
    {
        // update the range
        if (flags.reverse)
            CallSci(SCI_SETTARGETRANGE, CallSci(SCI_GETLENGTH), selEnd);
        else
            CallSci(SCI_SETTARGETRANGE, 0, selStart);

        // repeat the search with the new range
        if (CallSci(SCI_SEARCHINTARGET, strlen(term), reinterpret_cast<INT_PTR>(term)) >= 0)
        {
            // success - highlight the result
            SetFindHighlight(CallSci(SCI_GETTARGETSTART), CallSci(SCI_GETTARGETEND));
            return true;
        }
    }

    // not found anywhere - remove any search highlighting
    SetFindHighlight(-1, -1);
    return false;
}

void ConfigEditorWin::CommitSearch(CommitSearchReason reason, bool focusToSci)
{
    // If ending by Enter key, and the search was successful, set the selection 
    // to the search result.  Otherwise, restore the original scroll position,
    // except when exiting by focus change, in which case the user probably
    // clicked in the Scintilla window and expects the scrolling to stay put.
    if (reason == CommitSearchReason::Accept && findStart >= 0 && findEnd >= 0)
        CallSci(SCI_SETSEL, findStart, findEnd);
    else if (reason != CommitSearchReason::KillFocus)
        CallSci(SCI_SCROLLCARET);

    // remove search result highlighting
    SetFindHighlight(-1, -1);

    // set focus back in the Scintilla window if requested
    if (focusToSci)
    {
        // clear the find box
        SetWindowTextA(findBox, "");

        // focus on Scintilla
        SetFocus(sciWin);

        // mark the search as closed
        searchOpen = false;
    }
}

// --------------------------------------------------------------------------
//
// Syntax checking
//
bool ConfigEditorWin::CheckSyntax()
{
    // retrieve the document
    auto len = CallSci(SCI_GETLENGTH);
    std::vector<char> doc;
    doc.resize(len + 1);
    CallSci(SCI_GETTEXT, len, reinterpret_cast<INT_PTR>(doc.data()));

    // parse it
    JSONParser json;
    json.Parse(doc.data(), len);

    // if we found any errors, bring up the error panel
    if (json.errors.size() != 0)
    {
        // show the errors and return failure
        ShowErrorPanel(json.errors);
        return false;
    }

    // no syntax errors, so run a schema rules check
    CheckSchema(json);
    if (schemaCheckErrors.size() != 0)
    {
        ShowErrorPanel(schemaCheckErrors);
        return false;
    }

    // no errors round - check succeeded
    return true;
}

// Populate and display the error panel
void ConfigEditorWin::ShowErrorPanel(const std::list<JSONParser::Error> &errors)
{
    // populate the error panel with the new message list
    std::string txt;
    for (auto &err : errors)
    {
        char buf[512];
        sprintf_s(buf, "Line %d: %s\r\n", err.lineNum, err.message.c_str());
        txt.append(buf);
    }
    SetWindowTextA(errorPanel, txt.c_str());

    // show the panel
    ShowErrorPanel(true);

    // go to the first error
    GoToError(0);
}

bool ConfigEditorWin::GoToError(int errorPanelLineNum)
{
    // retrieve the line of text
    char buf[512]{ 0 };
    *reinterpret_cast<WORD*>(buf) = _countof(buf);
    int len = SendMessageA(errorPanel, EM_GETLINE, errorPanelLineNum, reinterpret_cast<LPARAM>(buf));
    buf[len] = 0;

    // select the line
    INT_PTR linePos = SendMessage(errorPanel, EM_LINEINDEX, errorPanelLineNum, 0);
    INT_PTR lineLen = SendMessage(errorPanel, EM_LINELENGTH, linePos, 0);
    SendMessage(errorPanel, EM_SETSEL, linePos, linePos + lineLen);

    // parse the error message
    static const std::regex pat("^line\\s+(\\d+):", std::regex_constants::icase);
    std::match_results<const char*> m;
    if (len != 0 && std::regex_search(buf, m, pat))
    {
        // Extract the nominal line number, and select that whole line in
        // Scintilla.  Note that Scintilla uses 0-based line numbering in
        // the API, whereas the error list is 1-based, so adjust accordingly.
        int lineNum = atoi(m[1].str().c_str()) - 1;
        CallSci(SCI_SETSEL, CallSci(SCI_POSITIONFROMLINE, lineNum), CallSci(SCI_GETLINEENDPOSITION, lineNum));

        // set focus back on scintilla
        SetFocus(sciWin);

        // successfully navigated to the error line
        return true;
    }

    // error not found in main text
    return false;
}

// Run a schema check
void ConfigEditorWin::CheckSchema(const JSONParser &json)
{
    // clear the error list
    schemaCheckErrors.clear();

    // validate starting at the root
    CheckSchemaObj(json.rootValue, jsonSchemaRoot, "");
}

void ConfigEditorWin::CheckSchemaObj(const JSONParser::Value &docObj, const JSONSchemaObj *schemaObj, const char *path)
{
    // if there's no schema object, there's nothing to check against
    if (schemaObj == nullptr)
        return;

    // check that the document object is an object
    if (!docObj.IsObject())
    {
        schemaCheckErrors.emplace_back(docObj.startTok, "Expected object");
        return;
    }

    // check each property in the document object against the schema, to
    // validate the type and check value constraints (such as enumerated
    // values)
    for (const auto &prop : *docObj.object)
    {
        // extract the name and value
        std::string name(prop.first.txt, prop.first.len);
        const auto &val = prop.second;

        // look up the name in the schema object
        auto *schemaProp = schemaObj->FindProp(name.c_str(), &docObj);
        if (schemaProp != nullptr)
        {
            // check the property
            std::string subPath = *path == 0 ? schemaProp->name : std::string(path) + "." + schemaProp->name;
            CheckSchemaProp(val, schemaObj, schemaProp, subPath.c_str());
        }
        else
        {
            schemaCheckErrors.emplace_back(val.startTok, StrPrintf("Property \"%s\" is not valid for this object", name.c_str()).c_str());
        }
    }

    // check for missing schema properties marked as required
    std::string missingProps;
    int nMissing = 0;
    schemaObj->ForEachProp([this, &missingProps, &nMissing, &docObj](const JSONSchemaObj::Prop *schemaProp) 
    {
        // if this property is required, make sure it's present
        if (schemaProp->required)
        {
            // scan for the property in the document object
            JSONParser::Value::StringWithLen name(schemaProp->name, strlen(schemaProp->name));
            auto *docProps = docObj.object;
            if (docProps->find(name) == docProps->end())
            {
                if (nMissing++ > 0) missingProps.append(", ");
                missingProps.append(std::string("\"") + schemaProp->name + "\"");
            }
        }
    }, &docObj);

    // log an error for missing properties
    if (missingProps.size() != 0)
    {
        schemaCheckErrors.emplace_back(docObj.startTok,
            StrPrintf("Required propert%s %s missing for %s", 
                nMissing > 1 ? "ies" : "y", missingProps.c_str(), path).c_str());
    }
}

void ConfigEditorWin::CheckSchemaProp(const JSONParser::Value &val,
    const JSONSchemaObj *schemaObj, const JSONSchemaObj::Prop *schemaProp,
    const char *path)
{
    // check enumerated values
    if (auto &evs = schemaProp->enumValues; evs.size() != 0)
    {
        // go by the first schema property type
        const char *propType = schemaProp->types.front().name;
        bool matched = false;
        if (strcmp(propType, "number") == 0)
        {
            // compare as numbers
            std::list<double> evNums;
            std::transform(evs.begin(), evs.end(), std::back_inserter(evNums),
                [](const std::string &s) { return strtod(s.c_str(), nullptr); });
            matched = std::find(evNums.begin(), evNums.end(), val.Double()) != evNums.end();
        }
        else
        {
            // compare anything else as strings
            matched = std::find(evs.begin(), evs.end(), val.String()) != evs.end();
        }

        if (!matched)
        {
            std::string msg = "Value must be one of: ";
            for (auto &ev : evs)
                msg += ev + " ";
            schemaCheckErrors.emplace_back(val.startTok, msg.c_str());
        }
    }

    // check for a validation regex
    if (schemaProp->validate != nullptr
        && !std::regex_match(val.String(), std::regex(schemaProp->validate)))
        schemaCheckErrors.emplace_back(val.startTok, "Value doesn't match expected pattern");

    // check the type
    bool matched = false;
    auto *subObj = schemaProp->GetSubObj();
    for (auto &t : schemaProp->types)
    {
        if (strcmp(t.name, "array") == 0)
        {
            // array
            if (val.IsArray())
            {
                // array is valid
                matched = true;

                // match the element types
                for (auto &eleVal : *val.array)
                {
                    for (auto &subtype : t.subtypes)
                    {
                        if (strcmp(subtype.name, "object") == 0)
                        {
                            // Match against the property's sub-object.  When an
                            // array of objects is allowed, the object subtype
                            // is simply encoded in the property.
                            CheckSchemaObj(*eleVal, subObj, path);
                        }
                    }
                }
            }
        }
        else if (strcmp(t.name, "object") == 0)
        {
            // object
            if (val.IsObject())
            {
                // type matched
                matched = true;

                // match the object value to the sub-object schema
                CheckSchemaObj(val, subObj, path);
            }
        }
        else
        {
            // other types match as long as they're not array or object,
            // since any scalar type can be implicitly converted to any
            // other scalar type
            matched = true;
        }
    }
    if (!matched)
        schemaCheckErrors.emplace_back(val.startTok, StrPrintf("Invalid type for %s", path).c_str());
}


// --------------------------------------------------------------------------
//
// Painting
//
void ConfigEditorWin::PaintOffScreen(HDC hdc0)
{
    // get the client size
    RECT crc;
    GetClientRect(hwnd, &crc);

    // get the mouse position in client coordinates, to detect hot items
    POINT mouse; 
    GetCursorPos(&mouse);
    ScreenToClient(hwnd, &mouse);

    // fill the background in the control bar color
    HDCHelper hdc(hdc0);
    FillRect(hdc, &crc, HBrush(HRGB(0xF0F0F0)));

    // bitmap drawing HDC
    CompatibleDC dcb(hdc);

    // frame the find box
    RECT erc = GetChildControlRect(findBox);
    InflateRect(&erc, 1, 1);
    FrameRect(hdc, &erc, HBrush(HRGB(0xB0B0B0)));

    // control bar button size - the buttons are square, so get the
    // size from the height (we can't use the bitmap width, since its
    // width is the combined width of all of the cells across the row)
    const int szBtn = szCtlBarButtons.cy;

    // draw the control bar buttons
    int xLastBtn = 0;
    for (auto &b : ctlBarButtons)
    {
        // only draw non-specially-positioned buttons on this pass
        if (!b.specialPos)
            DrawCtlBarButton(hdc, b, xLastBtn);
    }

    // draw the current parse location at the right
    std::string jsonPathStr = curLocParsedPath.ToString();
    const char *jsonPath = (jsonPathStr.size() != 0) ? jsonPathStr.c_str() : "{ Root Object }";
    int cxPath = hdc.MeasureText(mainFont, jsonPath).cx;
    int xMarginPath = 16 + szBtn + 8;
    int xPath = max(crc.right - cxPath - xMarginPath, xLastBtn + xMarginPath);
    int yPath = (cyControlBar - mainFontMetrics.tmHeight)/2;
    rcHelpLink ={ xPath, 0, crc.right - xMarginPath, cyControlBar };
    bool hotPath = IsClientRectHot(rcHelpLink);
    hdc.DrawText(xPath, yPath, 1, mainFont, HRGB(hotPath ? 0x0040FF : 0x000000), jsonPath);
    if (hotPath)
    {
        RECT rcUnderline{ xPath, yPath + mainFontMetrics.tmHeight, crc.right - xMarginPath, yPath + mainFontMetrics.tmHeight + 1 };
        FillRect(hdc, &rcUnderline, HBrush(HRGB(0x0040FF)));
    }
    SetTooltip(rcHelpLink, ID_HELP_LINK, "View the reference section in the help for this item");

    // arrange and draw the navigation drop button
    int xNavBtn = rcHelpLink.right + 8;
    navDropButton->rc.left = xNavBtn;
    navDropButton->rc.right = xNavBtn + szBtn;
    SetTooltip(navDropButton->rc, navDropButton->cmd, navDropButton->tooltip);
    DrawCtlBarButton(hdc, *navDropButton, xLastBtn);

    // draw the error panel title bar
    if (errorPanelVisible)
    {
        // get the panel rectangle
        RECT erc = GetChildControlRect(errorPanel);

        // draw the grabber bar
        rcErrorSizer = { crc.left, erc.top - cyErrorTitle - cyErrorSizer, crc.right - crc.left, erc.top - cyErrorTitle };
        FillRect(hdc, &rcErrorSizer, HBrush(HRGB(0x5D6B99)));

        // draw the title bar
        RECT trc{ crc.left, erc.top - cyErrorTitle, crc.right - crc.left, erc.top };
        FillRect(hdc, &trc, HBrush(HRGB(0x3b4f81)));
        int yTxt = (trc.bottom + trc.top - tmToolWindowTitleFont.tmHeight)/2;
        hdc.DrawText(crc.left + 8, yTxt, 1, toolWindowTitleFont, HRGB(0xffffff), "Error List");

        RECT brc{ trc.right - cyErrorTitle - 1, trc.top + 1, trc.right - 2, trc.bottom - 1 };
        rcErrorCloseBox = brc;
        bool hot = PtInRect(&brc, mouse);
        if (hot) FillRect(hdc, &brc, HBrush(HRGB(0xffffc0)));
        hdc.DrawText((brc.left + brc.right - hdc.MeasureText(toolWindowTitleFont, "x").cx)/2, 
            yTxt, 1, toolWindowTitleFont, hot ? HRGB(0xff0000) : HRGB(0xffffff), "x");
    }

    // draw a message centered in the window if scintilla isn't available
    if (sciWin == NULL)
    {
        static const char *msg1 = "The Scintilla editor control component isn't available.";
        static const char *msg2 = "You might need to reinstall the application's program files.";

        SIZE sz = hdc.MeasureText(boldFont, msg1);
        int xc = (crc.left + crc.right)/2;
        int yc = (crc.top + crc.bottom)/2;
        hdc.DrawText(xc - sz.cx/2, yc - sz.cy, 1, boldFont, HRGB(0x606060), msg1);

        sz = hdc.MeasureText(boldFont, msg2);
        hdc.DrawText(xc - sz.cx/2, yc, 1, boldFont, HRGB(0x606060), msg2);
    }
}


// --------------------------------------------------------------------------
//
// Auto-backup cleanup
//
void ConfigEditorWin::CleanUpAutoBackup()
{
    // if cleanup isn't enabled, do nothing
    if (!gApp.settingsJSON.Get("editor.autoBackup.cleanup.enabled")->Bool(true))
        return;

    // thread context
    struct ThreadCtx
    {
        ~ThreadCtx() { CloseHandle(running); }

        // days to keep old files
        int days = gApp.settingsJSON.Get("editor.autoBackup.cleanup.keepForDays")->Int(30);

        // backup folder
        std::string folder = ExpandAutoCompleteDir(gApp.settingsJSON.Get("editor.autoBackup.folder")->String());

        // thread running flag
        HANDLE running = CreateEvent(NULL, TRUE, FALSE, NULL);
    };
    std::shared_ptr<ThreadCtx> ctx(new ThreadCtx());

    // launch the thread
    DWORD tid;
    HANDLE hThread = CreateThread(NULL, 0, [](void *lparam) -> DWORD 
    {
        // take ownership of the thread context
        std::shared_ptr<ThreadCtx> ctx(*reinterpret_cast<std::shared_ptr<ThreadCtx>*>(lparam));

        // flag that the thread is running
        SetEvent(ctx->running);

        // Get the current local time as a Visual Basic VARIANT time.
        // VARIANT time the date and time as the number of days since
        // 12/30/1899 00:00:00.  The whole part is the number of whole
        // days, and the fractional part is the fraction of 24 hours
        // since midnight.  This is a convenient format for calculating
        // days between dates, since the simple arithmetic difference
        // of two VARIANT times equals the number of days between the
        // dates they represent.
        SYSTEMTIME stNow;
        double vtNow;
        GetLocalTime(&stNow);
        SystemTimeToVariantTime(&stNow, &vtNow);

        // scan for files in the auto-config folder matching the auto-config name
        char pat[MAX_PATH];
        PathCombineA(pat, ctx->folder.c_str(), "*.json");
        WIN32_FIND_DATAA fd;
        if (HANDLE hFind = FindFirstFileA(pat, &fd); hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                // if the file matches the naming pattern, process it
                static const std::regex re("\\w+_\\w+_(\\d\\d\\d\\d)(\\d\\d)(\\d\\d)_(\\d\\d)(\\d\\d)(\\d\\d)\\.json", std::regex_constants::icase);
                std::match_results<const char*> m;
                if (std::regex_match(fd.cFileName, m, re))
                {
                    // build the date
                    SYSTEMTIME st{ 0 };
                    st.wYear = atoi(m[1].str().c_str());
                    st.wMonth = atoi(m[2].str().c_str());
                    st.wDay = atoi(m[3].str().c_str());
                    st.wHour = atoi(m[4].str().c_str());
                    st.wMinute = atoi(m[5].str().c_str());
                    st.wSecond = atoi(m[6].str().c_str());

                    // convert to a VARIANT time, and calculate days between now and then
                    double vt = 0.0;
                    if (SystemTimeToVariantTime(&st, &vt) && vtNow - vt > ctx->days)
                    {
                        // it's older than the timeout period, so delete the file
                        char fullPath[MAX_PATH];
                        PathCombineA(fullPath, ctx->folder.c_str(), fd.cFileName);
                        DeleteFileA(fullPath);
                    }
                }
            } while (FindNextFileA(hFind, &fd));

            // done with the find-file context
            FindClose(hFind);
        }

        // return value unused
        return 0;
    }, &ctx, 0, &tid);

    if (hThread != NULL)
    {
        // wait for the thread to start, so that it can add its reference 
        // to the context before we leave
        WaitForSingleObject(ctx->running, 5000);

        // we don't need the handle - we just let the thread run asynchronously
        CloseHandle(hThread);
    }
}

// --------------------------------------------------------------------------
//
// Printing
//

void ConfigEditorWin::PageSetupOptions::Load()
{
    __super::Load();
    const auto *val = gApp.settingsJSON.Get(jsonKey);
    wrap = val->Get("wrap")->Bool(true);
    showLineNumbers = val->Get("lineNumbers")->Bool(true);
}

void ConfigEditorWin::PageSetupOptions::Store()
{
    __super::Store();
    auto &js = gApp.settingsJSON;
    auto *val = gApp.settingsJSON.SetObject(jsonKey);
    js.SetBool(val, "wrap", wrap);
    js.SetBool(val, "lineNumbers", showLineNumbers);
}

void ConfigEditorWin::PageSetup()
{
    class PSDlg : public PageSetupDialog
    {
    public:
        PSDlg(HINSTANCE hInstance) : PageSetupDialog(hInstance, opts) { }

        PageSetupOptions opts;

        virtual void LoadControls()
        {
            __super::LoadControls();
            CheckRadioButton(hDlg, IDC_RB_WRAP, IDC_RB_TRUNC, opts.wrap ? IDC_RB_WRAP : IDC_RB_TRUNC);
            CheckDlgButton(hDlg, IDC_CK_LINENUMBERS, opts.showLineNumbers ? BST_CHECKED : BST_UNCHECKED);
        }

        virtual void StoreControls()
        {
            opts.wrap = (IsDlgButtonChecked(hDlg, IDC_RB_WRAP) == BST_CHECKED);
            opts.showLineNumbers = (IsDlgButtonChecked(hDlg, IDC_CK_LINENUMBERS) == BST_CHECKED);
            __super::StoreControls();
        }
    };
    PSDlg d(hInstance);
    d.Show(IDD_CONFIGEDITOR_PAGESETUP);
}


bool ConfigEditorWin::PrintPageContents(HDCHelper &hdc, int pageNum, bool skip)
{
    // set up the Scintilla print context
    auto &rtf = printingContext.rtf;
    rtf.hdc = hdc;
    rtf.hdcTarget = hdc;
    auto ToSciRect = [](const RECT &rc) -> Sci_Rectangle { return { rc.left, rc.top, rc.right, rc.bottom }; };
    rtf.rcPage = ToSciRect(BaseWindow::printingContext.rcPaper);
    rtf.rc = ToSciRect(BaseWindow::printingContext.rcPrint);

    // get my subclassed options object
    auto &opts = *static_cast<PageSetupOptions*>(BaseWindow::printingContext.opts.get());

    // on page 1, initialize the print range to cover the whole document
    if (pageNum == 1)
    {
        // set the print range
        rtf.chrg.cpMin = 0;
        rtf.chrg.cpMax = CallSci(SCI_GETLENGTH);

        // set the wrap mode
        CallSci(SCI_SETPRINTWRAPMODE, opts.wrap ? SC_WRAP_WORD : SC_WRAP_NONE);

        // set the color mode
        CallSci(SCI_SETPRINTCOLOURMODE, opts.monochrome ? SC_PRINT_BLACKONWHITE : SC_PRINT_COLOURONWHITE);

        // hide line numbers if desired
        printingContext.marginWidth0 = CallSci(SCI_GETMARGINWIDTHN, 0);
        if (!opts.showLineNumbers)
            CallSci(SCI_SETMARGINWIDTHN, 0, 0);

        // reset to the start of the document for the actual printing
        rtf.chrg.cpMin = 0;
    }

    // have Scintilla print its content area
    rtf.chrg.cpMin = CallSci(SCI_FORMATRANGEFULL, !skip, reinterpret_cast<INT_PTR>(&rtf));

    // figure if there's more
    bool more = (rtf.chrg.cpMin < rtf.chrg.cpMax);

    // on the last page, restore Scintilla settings that we altered for printing
    if (!more)
    {
        CallSci(SCI_SETMARGINWIDTHN, 0, printingContext.marginWidth0);
    }

    // return the more-to-do indication
    return more;
}

int ConfigEditorWin::Paginate(HDCHelper &hdc)
{
    // set up the Scintilla print context
    auto &rtf = printingContext.rtf;
    rtf.hdc = hdc;
    rtf.hdcTarget = hdc;
    auto ToSciRect = [](const RECT &rc) -> Sci_Rectangle { return { rc.left, rc.top, rc.right, rc.bottom }; };
    rtf.rcPage = ToSciRect(BaseWindow::printingContext.rcPaper);
    rtf.rc = ToSciRect(BaseWindow::printingContext.rcPrint);

    // set the print range to the whole document
    rtf.chrg.cpMin = 0;
    rtf.chrg.cpMax = CallSci(SCI_GETLENGTH);

// run through all the pages in non-drawing mode to get a page count
    int nPages = 0;
    for ( ; rtf.chrg.cpMin < rtf.chrg.cpMax ; ++nPages)
        rtf.chrg.cpMin = CallSci(SCI_FORMATRANGEFULL, FALSE, reinterpret_cast<INT_PTR>(&rtf));

    return nPages;
}

bool ConfigEditorWin::ExpandHeaderFooterVar(const std::string &varName, std::string &expansion)
{
    auto FromInt = [](int n) {
        char buf[32];
        sprintf_s(buf, "%d", n);
        return std::string(buf);
    };
    auto DeviceInfo = [this](std::function<std::string(const DeviceID&)> func)
    {
        DeviceID id;
        if (PinscapePico::VendorInterface::Shared::Locker l(device); l.locked)
            device->device->QueryID(id);
        return func(id);
    };
    if (varName == "type")
        return expansion = (configType == VendorRequest::CONFIG_FILE_MAIN ? "Main" : "Safe Mode"), true;
    if (varName == "unitNum")
        return expansion = DeviceInfo([FromInt](const DeviceID &id) { return FromInt(id.unitNum); }), true;
    if (varName == "unitName")
        return expansion = DeviceInfo([](const DeviceID &id) { return id.unitName; }), true;

    // unknown
    return __super::ExpandHeaderFooterVar(varName, expansion);
}

