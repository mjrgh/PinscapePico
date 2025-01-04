// Pinscape Pico - Config Tool - Configuration Editor Window
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This class implements an interactive window showing the states
// of the Pinscape Pico unit's logical output ports and physical
// output devices.  This is useful for testing and troubleshooting
// output device wiring and the software configuration settings.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tchar.h>
#include <memory>
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "../Scintilla/include/Scintilla.h"
#include "PinscapePicoAPI.h"
#include "Dialog.h"
#include "WinUtil.h"
#include "TabEmbeddableWindow.h"
#include "JSONExt.h"
#include "BaseDeviceWindow.h"

namespace PinscapePico
{
	// JSON schema import.  This is a program-readable description of the JSON
	// schema, generated from the documentation.  We use this to correlate the
	// user's source text with schema elements.
	struct JSONSchemaObj
	{
		// Subclass ID property name.  Some objects have a property that
		// declares the object as a subclass of its schema base class.
		// A subclass in this context just adds more properties to the
		// base class schema.  The ID property is usually named "type".
		//
		// This is set in a *base class* that can differentiate into
		// subclasses.
		const char *subclassIdPropName;

		// Subclass ID property value.  This is set in each *subclass*
		// of a base class that can be differentiated into subclasses.
		const char *subclassIdPropVal;

		// subclass object types
		std::list<const JSONSchemaObj*> subclassObjects;

		// find a property by name
		struct Prop;
		const Prop *FindProp(const char *name, const JSONParserExt::Value *val) const;

		// iterate properties
		void ForEachProp(std::function<void(const Prop*)> func, const JSONParserExt::Value *val) const;

		// Property list.  This is a list of the properties that this
		// object can contains.
		struct Prop
		{
			// property name
			const char *name;

			// documentation link - this is the '#' anchor in the generated schema doc file
			const char *link;

			// required
			bool required;

			// documentation summary
			const char *summary;

			// regular expression for validating a value string
			const char *validate;

			// Type list.  This is a list of the allowed value types for
			// this property.
			struct Type
			{
				// type name: "number", "string", "boolean", "array", "object"
				const char *name;

				// subtypes, for arrays
				std::list<Type> subtypes;
			};
			std::list<Type> types;

			// Get the sub-object.  If there's a direct sub-object, we'll
			// return that; otherwise, we'll look for a cross-reference
			// sub-object.
			const JSONSchemaObj *GetSubObj() const;

			// sub-object, for objects
			const JSONSchemaObj *subObj;

			// Cross-reference object.  This is a sub-object that refers
			// back to some other point in the schema tree, to re-use the
			// same schema as for another object already defined.
			const JSONSchemaObj *xrefObj;

			// enumerated value list
			std::list<std::string> enumValues;

			// Is this a boolean value property?  Returns true if the
			// type list allows only booleans.
			bool IsBool() const;
		};
		std::list<Prop> props;
	};

	class ConfigEditorWin : public BaseDeviceWindow, public TabEmbeddableWindow
	{
	public:
		// construction
		ConfigEditorWin(HINSTANCE hInstance,
			std::shared_ptr<VendorInterface::Shared> &device,
			uint8_t configType);

		// Destruction
		~ConfigEditorWin();

		// Clean up old auto-backup configuration files, according to
		// the JSON preference settings.  Runs the cleanup on a background
		// thread.
		static void CleanUpAutoBackup();

		// set my menu bar in the host application
		virtual bool InstallParentMenuBar(HWND hwndContainer) override;

		// translate accelerators
		virtual bool TranslateAccelerators(HWND hwndMenu, MSG *msg) override;

		// UI activation
		virtual void OnActivateUI(bool isAppActivate) override;

		// command handling
		virtual bool OnCommand(WORD notifyCode, WORD ctlCmdId, HWND hwndCtl, LRESULT &lresult) override;

		// 
		// TabEmbeddableWindow interface implementation
		//

		// has the "document" represented by this window been modified?
		virtual bool IsDocumentModified() const override { return isModified; }

		// local changes will be lost on a factory reset
		virtual bool PreEraseDeviceConfig(bool factoryReset) const override { return isModified; }

		// receive notification that the configuration has been cleared
		virtual void OnEraseDeviceConfig(bool factoryReset) override;

		// stay visible while the device is offline
		virtual bool IsVisibleOffline() const { return true; }

		// device reconnect notification
		virtual bool OnDeviceReconnect() override;

		// get the current key chord prefix, if any
		const char *GetKeyBindingPrefix() const { return keyBindingContext.prefix; }

	protected:
		// parsed contents
		std::vector<char> parsedText;
		std::unique_ptr<JSONParserExt> parsedJSON;
		bool parsePending = false;

		// current parse location, as a JSON path string
		JSONParserExt::Path curLocParsedPath;

		// parse location elements
		const JSONSchemaObj *curLocSchemaObj = nullptr;
		const JSONParser::Value *curLocSchemaSubclass = nullptr;
		const JSONSchemaObj::Prop *curLocSchemaProp = nullptr;
		JSONParser::Value *curLocSchemaVal = nullptr;
		
		// parse location help link
		const char *curLocHelpLink = nullptr;
		RECT rcHelpLink{ 0, 0, 0, 0 };

		// call tip help link
		const char *callTipHelpLink = nullptr;

		// Set the Scintilla editor content text
		void SetConfigText(const std::vector<char> &text, bool resetUndo);

		// Parse the contents.  This is done on a timer after any change to
		// the document.
		void ParseContents();

		// Reset the parse delay.  We call this on any UI input associated with
		// user action, such as mouse clicks or key presses.  The idea is to
		// delay parsing until the user is inactive for a brief time, so that
		// the parsing doesn't slow down UI interaction.
		void ResetParseDelay();

		// find the current caret position in the parse tree, populating the curXxx fields
		void FindParsePosition();

		// find a given text position in the parse tree
		void FindParsePosition(INT_PTR pos, 
			JSONParserExt::Path &path, const JSONSchemaObj* &obj,  const JSONParser::Value* &subclassSelector,
			const JSONSchemaObj::Prop* &prop, JSONParser::Value* &val);

		// timer IDs
		static const int TIMER_ID_PARSE = 219;	

		// window class
		const TCHAR *GetWindowClassName() const override { return _T("PinscapePicoConfigEditor"); }

		// get my context menu
		virtual HMENU GetContextMenu(POINT pt, HWND &hwndCommand) override { return hCtxMenu; }

		// option settings
		struct Options
		{
			// load from/save to JSON settings
			void Load();
			void Store();

			// indent with tabs (if false, indent with spaces instead)
			bool indentWithTabs = true;

			// tab size
			int tabSize = 8;

			// use Emacs key bindings?
			bool emacsKeyBindings = false;

			// ask for confirmation before saving to Pico
			bool confirmSaves = true;

			// check JSON schema rules before saving to Pico
			bool checkRulesBeforeSave = true;

			// enable auto-complete
			bool enableAutoComplete = true;

			// Auto-save backup folder.  Whenever the user saves a
			// configuration file update, we'll make a backup copy
			// here, if the option is enabled.
			bool autoBackupEnabled = false;
			std::string autoBackupFolder;

			// Auto-backup file cleanup.  If enabled, we'll delete
			// files older than the specified number of days each
			// time the Config Tool starts up.
			bool autoBackupCleanupEnabled = true;
			int autoBackupCleanupDays = 30;
		};
		// use a global singleton of this object, so that it's shared
		// across all editor windows
		static Options options;

		// expand substitution variables in the auto-complete filename
		static std::string ExpandAutoCompleteDir(const std::string &dir);

		// apply the options settings in this->options - updates the Scintilla
		// control and key accelerators with the new settings
		void ApplyOptions();

		// set Emacs/regular key binding mode
		void SetEmacsMode(bool f);

		// indent a closing delimiter to match the opening line's indentation
		void IndentCloseDelim(INT_PTR pos);

		// perform auto-indenting and auto brace matching
		void AutoIndent(int ch);

		// perform auto-completion
		void CheckBeginAutoComplete();

		// open-auto-indent pending
		bool openAutoCompletePending = false;
		
		// get the indentation level at the delimiter matching the one at the given position
		INT_PTR GetMatchingIndent(INT_PTR pos);

		// Quoted character pending.  The next key-down event is treated as
		// a literal character to be inserted into the buffer, rather than
		// as a command accelerator.
		bool quotedCharPending = false;
		
		// Key binding.  This replaces the standard accelerator (HACCEL) mechanism
		// in this window, to accommodate Emacs's multi-key sequences.  A key or
		// key sequence can be bound to either a Scintilla SCI_xxx keyboard command
		// or to a WM_COMMAND code.  WM_COMMAND codes can be used to implement
		// composite editing commands that are constructed from multiple SCI_xxx
		// commands or using SCI_xxx commands that require arguments, as well as
		// for commands external to the Scintilla control, such as saving a file
		// or checking syntax.
		struct KeyBinding
		{
			KeyBinding(const char *keys, int sciCommand, int command = 0, int contextSelector = 0) :
				keys(keys), sciCommand(sciCommand), command(command), contextSelector(contextSelector) { }

			// Key sequence.  This is written out the way it appears in
			// a Windows menu, and may consist of a single chord or a
			// sequence of chords.  For example, "Ctrl+A" for a single
			// key chord, or "Ctrl+X Ctrl+F" for a two-chord sequence.
			std::string keys;

			// Scintilla keyboard command, or 0 if none
			int sciCommand;

			// WM_COMMAND command, used if sciCommand == 0
			int command;

			// Prefix context selector.  If this is non-zero, pressing 
			// the key doesn't carry out a command directly, but shifts
			// the context for the next key to the new table at this
			// index.
			int contextSelector;
		};

		// Current key binding maps.  Each map is keyed by an int value
		// representing a chord, which is a combination of a VK_xxx code
		// and the modifier-key bits defined below.  The 0th map is the
		// base map used for the first key of a sequence.  Each additional
		// map is selected by a contextSelector index in a key binding.
		std::unordered_map<int, KeyBinding> keyBinding[10];
		static const int SHIFT = 0x00010000;
		static const int CTRL =  0x00020000;
		static const int ALT =   0x00040000;

		// Current key sequence context
		struct
		{
			// reset the binding context to the root table
			void Clear() { index = 0; prefix = nullptr; }

			// index into the keyBinding[] array of the table used to
			// translate the next keystroke
			int index = 0;

			// current key prefix sequence leading to the selected table
			const char *prefix = nullptr;

		} keyBindingContext;

		// name -> VKEY mapping table for decoding key bindings
		static const std::unordered_map<std::string_view, UINT> vkeyMap;

		// menu popup updates
		virtual void OnInitMenuPopup(HMENU hMenu, WORD itemPos, bool isSysMenu) override;

		// update command status through a callback - common handler for InitMenuPopup
		// and toolbar button enablers
		virtual void UpdateCommandStatus(std::function<void(int cmd, bool enabled)> apply) override;

		// Install a key binding.  This populates the keyBinding[] maps
		// and updates the menus.
		void InstallKeyBinding(const KeyBinding *keys, size_t numKeys);

		// update the accelerator labels for the new key bindings
		void UpdateMenuAccelLabels(HMENU hMenu, const std::unordered_map<UINT, const char*> &commandToKeyName);

		// configuration type - one of the VendorRequest::CONFIG_FILE_xxx constants
		uint8_t configType = VendorRequest::CONFIG_FILE_MAIN;

		// load the config text from the device
		void LoadFromDevice(bool resetUndo);

		// file command handlers
		void SaveToDevice();
		void ReloadFromDevice();
		void ImportFile();
		void SaveToFile();
		bool SaveToFile(std::ofstream &f, const char *text, size_t len);

		// import a file by name
		void ImportFile(const TCHAR *filename);

		// import from a template file
		void ImportTemplateFile();

		// offer to load a template file
		void OfferTemplateFile();

		// get the Config Templates folder path
		void GetConfigTemplatesFolder(TCHAR path[MAX_PATH]);

		// show the Go To Line Number dialog
		void GoToLineNum();

		// If the buffer has been modified from the last device fetch,
		// warn the user about an operation that will discard changes,
		// and ask for confirmation.  Returns true if the caller should
		// proceed, false if the user rejected the confirmation.  If the
		// buffer hasn't been modified, this returns true without asking
		// for confirmation, since no work will be lost; otherwise, it
		// presents a Yes/No/Cancel dialog and returns true if the user
		// selects Yes, false otherwise.  (No and Cancel are redundant,
		// but we include them both just in case one makes more sense
		// than the other to the user's intuition.)
		bool ConfirmDiscard();

		// has the document been modified since the last savepoint?
		bool isModified = false;

		// window message handlers
		virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam) override;
		virtual void OnCreateWindow() override;
		virtual void OnDestroy() override;
		virtual void OnShowWindow(bool show, LPARAM source) override;
		virtual void OnSizeWindow(WPARAM type, WORD width, WORD height) override;
		virtual bool OnNotify(WPARAM id, NMHDR *nmhdr, LRESULT &lresult) override;
		virtual bool OnLButtonDown(WPARAM keys, int x, int y) override;
		virtual bool OnLButtonUp(WPARAM keys, int x, int y) override;
		virtual bool OnMouseMove(WPARAM keys, int x, int y) override;
		virtual bool OnCaptureChange(HWND hwnd) override;
		virtual void OnTimer(WPARAM timerId) override;
		virtual bool OnSetCursor(HWND hwndCursor, UINT hitTest, UINT msg) override;

		// control subclass handler
		virtual LRESULT ControlSubclassProc(
			HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR idSubclass) override;

		// Paint off-screen.  This prepares a bitmap with the window
		// contents filled in, for display in the next WM_PAINT.
		virtual void PaintOffScreen(HDC hdc) override;

		// adjust the window layout
		void AdjustLayout();

		// show the navigation drop menu
		void ShowNavDropMenu();

		// Last text loaded from the device.  We use this on a reconnect to
		// determine if the file stored on the device changed since the last
		// time we loaded it.
		std::vector<char> lastDeviceText;

		// flag: the device has reconnected since lastDeviceText was retrieved
		bool reconnected = false;

		// show the preferences dialog
		void ShowPrefsDialog();

		// editor font
		HFONT editorFont = NULL;
		TEXTMETRIC tmEditorFont{ 0 };

		// tool window title bar font
		HFONT toolWindowTitleFont = NULL;
		TEXTMETRIC tmToolWindowTitleFont{ 0 };

		// top control bar
		int cyControlBar = 0;

		// control IDs
		static const UINT_PTR ID_SCINTILLA = 101;
		static const UINT_PTR ID_EDIT_FINDBOX = 102;
		static const UINT_PTR ID_EDIT_ERRORS = 103;
		static const UINT_PTR ID_NAV_DROP_BUTTON = 104;
		static const UINT_PTR ID_HELP_LINK = 105;

		// main menu bar 
		HMENU hMenuBar = NULL;

		// context menu
		HMENU hCtxMenu = NULL;

		// control bar Find box
		HWND findBox = NULL;

		// nav drop button
		CtlBarButton *navDropButton = nullptr;

		// error message panel
		HWND errorPanel = NULL;
		bool errorPanelVisible = false;
		int cyErrorPanel = 0;
		int cyErrorSizer = 6;
		int cyErrorTitle = 16;
		RECT rcErrorCloseBox{ 0 };
		RECT rcErrorSizer{ 0 };

		// tracking an error panel resize
		struct ErrorPanelResize
		{
			bool tracking = false;
			POINT start{ 0, 0 };
			int cyInitial = 0;
		};
		ErrorPanelResize errorPanelResize;

		// Show/hide the error panel
		void ShowErrorPanel(bool show);

		// Populate and show the error panel
		void ShowErrorPanel(const std::list<JSONParser::Error> &errors);

		// Go to an error, by line number (starting at 0) in the error panel.
		// This selects the line of text in the error panel, and jumps to the
		// location of the error in the main document window.  Returns true
		// if the error location was found in the main text, false if not.
		bool GoToError(int errorPanelLineNum);

		// search flags
		struct SearchFlags
		{
			SearchFlags() { }
			SearchFlags(bool matchCase, bool wholeWord, bool regex, bool wrap) :
				matchCase(matchCase), wholeWord(wholeWord), regex(regex), wrap(wrap) { }

			void Clear() { matchCase = wholeWord = regex = false; }

			// get the flags as a combination of SCFIND_xxx bits
			uint32_t SciFlags() const;

			bool matchCase = false;
			bool wholeWord = false;
			bool regex = false;
			bool wrap = true;
			bool preserveCase = false;
			bool reverse = false;
		};

		// last search term
		struct LastSearch
		{
			std::string term;
			SearchFlags flags;
		};
		LastSearch lastSearch;

		// flag: search is in progress
		bool searchOpen = false;

		// current incremental search result range
		INT_PTR findStart = 0;
		INT_PTR findEnd = 0;

		// set the incremental search result range
		void SetFindHighlight(INT_PTR start, INT_PTR end);

		// execute a Find Next/Previous command
		void FindNextPrev(bool prev);

		// Update the search in progress.  Returns true if a match was
		// found, false if not.
		bool UpdateSearch(const char *term, SearchFlags flags);

		// end search mode
		enum class CommitSearchReason
		{
			Accept,       // accept the results of the search (Enter key from box, F3)
			Cancel,       // cancel the search
			KillFocus,    // exit by losing focus without an accept/cancel action
		};
		void CommitSearch(CommitSearchReason reason, bool focusToSci);

		// Run a syntax check on the file contents.  Returns true on
		// success, false if any syntax errors were detected.
		bool CheckSyntax();

		// Run a schema check on the file contents.  If any errors are
		// found, they're logged to the schemaCheckErrors list.
		void CheckSchema(const JSONParser &json);

		// validate a JSON document object against its schema definition counterpart
		void CheckSchemaObj(const JSONParser::Value &docObj, const JSONSchemaObj *schemaObj, const char *path);

		// validate a JSON property value against its schema definition
		void CheckSchemaProp(const JSONParser::Value &val, 
			const JSONSchemaObj *schemaObj, const JSONSchemaObj::Prop *schemaProp,
			const char *path);

		// schema check error list
		std::list<JSONParser::Error> schemaCheckErrors;

		// Scintilla window
		HWND sciWin = NULL;

		// Lexilla JSON lexer
		void *lexer = nullptr;

		// Scintilla direct access function pointer
		typedef INT_PTR __cdecl ScintillaFunc_t(void*, int, INT_PTR, INT_PTR);
		ScintillaFunc_t *sciFunc = nullptr;
		void *sciFuncCtx = nullptr;
		INT_PTR CallSci(int msg, INT_PTR param1 = 0, INT_PTR param2 = 0) {
			return sciFunc != nullptr ? sciFunc(sciFuncCtx, msg, param1, param2) : 0; }

		// Find/Replace dialog
		FINDREPLACEA findReplaceParams{ sizeof(findReplaceParams) };
		std::string findWhat;
		std::string replaceWith;
		SearchFlags findReplaceFlags;

		// custom dialog class
		class FindReplaceDialog : public Dialog
		{
		public:
			FindReplaceDialog(HINSTANCE hInstance, HWND hwndCommand) : 
				Dialog(hInstance), hwndCommand(hwndCommand) { }

			// Command target window.  This is our associated editor window.
			// We send MSG_FINDREPLACEDLG messages to this window for events
			// in the dialog.
			HWND hwndCommand;

			// get/set the search flags from/to the checkboxes
			void SetFlags(SearchFlags flags);
			SearchFlags GetFlags() const;

			// get/set the Find What text, Replace With text
			std::string GetFindWhat() const;
			std::string GetReplaceWith() const;
			void SetFindWhat(const char *s);
			void SetReplaceWith(const char *s);

			// dialog overrides
			virtual INT_PTR Proc(UINT message, WPARAM wParam, LPARAM lParam) override;
			virtual INT_PTR OnInitDialog(WPARAM wparam, LPARAM lparam) override;

			// brush for error coloring in the find box
			static const COLORREF errorColor = RGB(0xff, 0xe0, 0xe0);
			HBrush errorBrush{ errorColor };

			// check regex syntax in the find box
			void CheckRegexSyntax();
			bool regexSyntaxError = false;
		};
		FindReplaceDialog *findReplaceDlg = nullptr;
		std::shared_ptr<Dialog> findReplaceDlgSharedRef;

		// Printing
		virtual void PageSetup() override;
		virtual bool IsPrintingEnabled() const override { return true; }
		virtual std::basic_string<TCHAR> GetPrintDocName() const override { return _T("Pinscape Pico Configuration"); }
		virtual PageSetupOptions *CreatePageSetupOptions() const { return new PageSetupOptions(); }
		virtual bool ExpandHeaderFooterVar(const std::string &varName, std::string &expansion) override;
		virtual int Paginate(HDCHelper &hdc) override;
		virtual bool PrintPageContents(HDCHelper &hdc, int pageNum, bool skip) override;

		// Page setup options.  This provides the basic set of options
		// common to most document types, and it can extended with
		// additional properties as needed.
		struct PageSetupOptions : BaseWindow::PageSetupOptions
		{
			PageSetupOptions() : BaseWindow::PageSetupOptions("editor.pageSetup") { Load(); }
			virtual void Load() override;
			virtual void Store() override;

			// wrap long lines - true -> wrap, false -> truncate
			bool wrap = true;

			// show line numbers
			bool showLineNumbers = true;
		};

		// print context
		struct PrintingContext
		{
			// current print range
			Sci_RangeToFormatFull rtf{ 0 };

			// original line number margin width
			INT_PTR marginWidth0 = 0;
		};
		PrintingContext printingContext;

		// Execute one replace operation.  If the current target matches
		// the search query, the replacement is applied, otherwise the buffer
		// isn't changed.  In either case, searches for the next instance of
		// the target, and returns true if a match was found, false if not.
		//
		// The semantics match those of clicking on the Replace Next button
		// in a standard Windows Find/Replace dialog, so this can be used to
		// trivially implement a Replace Next command from the dialog.  It
		// can also be used to implement Replace All by calling it repeatedly
		// as long as it returns true, with the caveat that if wrapping is
		// enabled, the caller has to end the loop when the new match is
		// beyond the original starting point.  No extra loop conditions are
		// needed if wrapping is disabled.  Most applications implement
		// Replace All as non-wrapping (i.e., replace from here to end of
		// document), so I think this is what most users expect by default.
		bool ExecReplace(const char *findWhat, const char *replaceWith, SearchFlags flags);
	};
}
