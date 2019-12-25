/* Copyright 2019 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/Log.h"
#include "utils/WinUtil.h"

#include "wingui/WinGui.h"
#include "wingui/Layout.h"
#include "wingui/Window.h"
#include "wingui/TreeModel.h"
#include "wingui/TreeCtrl.h"

/*
Tree view, checkboxes and other info:
- https://devblogs.microsoft.com/oldnewthing/20171127-00/?p=97465
- https://devblogs.microsoft.com/oldnewthing/20171128-00/?p=97475
- https://devblogs.microsoft.com/oldnewthing/20171129-00/?p=97485
- https://devblogs.microsoft.com/oldnewthing/20171130-00/?p=97495
- https://devblogs.microsoft.com/oldnewthing/20171201-00/?p=97505
- https://devblogs.microsoft.com/oldnewthing/20171204-00/?p=97515
- https://devblogs.microsoft.com/oldnewthing/20171205-00/?p=97525
-
https://stackoverflow.com/questions/34161879/how-to-remove-checkboxes-on-specific-tree-view-items-with-the-tvs-checkboxes-sty
*/

Kind kindTree = "treeV";

bool IsTree(Kind kind) {
    return kind == kindTree;
}

bool IsTree(ILayout* l) {
    return IsLayoutOfKind(l, kindTree);
}

ILayout* NewTreeLayout(TreeCtrl* e) {
    return new WindowBaseLayout(e, kindTree);
}

static void TreeViewExpandRecursively(HWND hTree, HTREEITEM hItem, UINT flag, bool subtree) {
    while (hItem) {
        TreeView_Expand(hTree, hItem, flag);
        HTREEITEM child = TreeView_GetChild(hTree, hItem);
        if (child) {
            TreeViewExpandRecursively(hTree, child, flag, false);
        }
        if (subtree) {
            break;
        }
        hItem = TreeView_GetNextSibling(hTree, hItem);
    }
}

// the result only valid until the next GetItem call
static TVITEMW* GetTVITEM(TreeCtrl* tree, HTREEITEM hItem) {
    TVITEMW* ti = &tree->item;
    ZeroStruct(ti);
    ti->hItem = hItem;
    // https: // docs.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-tvitemexa
    ti->mask = TVIF_HANDLE | TVIF_PARAM | TVIF_STATE | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    ti->stateMask = TVIS_SELECTED | TVIS_CUT | TVIS_DROPHILITED | TVIS_BOLD | TVIS_EXPANDED | TVIS_STATEIMAGEMASK;
    BOOL ok = TreeView_GetItem(tree->hwnd, ti);
    if (!ok) {
        return nullptr;
    }
    return ti;
}

static TVITEMW* GetTVITEM(TreeCtrl* tree, TreeItem* ti) {
    HTREEITEM hi = tree->GetHandleByTreeItem(ti);
    return GetTVITEM(tree, hi);
}

#include "utils/BitManip.h"

// expand if collapse, collapse if expanded
static void TreeViewToggle(TreeCtrl* tree, HTREEITEM hItem, bool recursive) {
    HWND hTree = tree->hwnd;
    HTREEITEM child = TreeView_GetChild(hTree, hItem);
    if (!child) {
        // only applies to nodes with children
        return;
    }

    TVITEMW* item = GetTVITEM(tree, hItem);
    if (!item) {
        return;
    }
    UINT flag = TVE_EXPAND;
    bool isExpanded = bitmask::IsSet(item->state, TVIS_EXPANDED);
    if (isExpanded) {
        flag = TVE_COLLAPSE;
    }
    if (recursive) {
        TreeViewExpandRecursively(hTree, hItem, flag, false);
    } else {
        TreeView_Expand(hTree, hItem, flag);
    }
}

void TreeCtrl::WndProcParent(WndProcArgs* args) {
    auto* w = (TreeCtrl*)this;
    HWND hwnd = args->hwnd;
    UINT msg = args->msg;
    WPARAM wp = args->wparam;
    LPARAM lp = args->lparam;

    CrashIf(GetParent(w->hwnd) != (HWND)hwnd);

    if (msg == WM_NOTIFY) {
        NMTREEVIEWW* nm = (NMTREEVIEWW*)(lp);
        if (w->onTreeNotify) {
            TreeNotifyArgs nargs{};
            nargs.w = w;
            nargs.treeView = nm;
            nargs.procArgs = args;

            w->onTreeNotify(&nargs);
            if (args->didHandle) {
                return;
            }
        }

        auto code = nm->hdr.code;
        if (code == TVN_GETINFOTIP) {
            if (w->onGetTooltip) {
                TreeItmGetTooltipArgs cbArgs{};
                cbArgs.w = w;
                cbArgs.info = (NMTVGETINFOTIPW*)(nm);
                cbArgs.treeItem = w->GetTreeItemByHandle(cbArgs.info->hItem);
                w->onGetTooltip(&cbArgs);
                args->didHandle = true;
                args->result = 0;
                return;
            }
        }
    }

    if (msg == WM_CONTEXTMENU) {
        if (w->onContextMenu) {
            TreeContextMenuArgs a;
            a.procArgs = args;
            a.w = w;
            a.x = GET_X_LPARAM(lp);
            a.y = GET_Y_LPARAM(lp);
            onContextMenu(&a);
        }
        return;
    }
}

static bool HandleKey(TreeCtrl* tree, WPARAM wp) {
    HWND hwnd = tree->hwnd;
    // consistently expand/collapse whole (sub)trees
    if (VK_MULTIPLY == wp) {
        if (IsShiftPressed()) {
            TreeViewExpandRecursively(hwnd, TreeView_GetRoot(hwnd), TVE_EXPAND, false);
        } else {
            TreeViewExpandRecursively(hwnd, TreeView_GetSelection(hwnd), TVE_EXPAND, true);
        }
    } else if (VK_DIVIDE == wp) {
        if (IsShiftPressed()) {
            HTREEITEM root = TreeView_GetRoot(hwnd);
            if (!TreeView_GetNextSibling(hwnd, root))
                root = TreeView_GetChild(hwnd, root);
            TreeViewExpandRecursively(hwnd, root, TVE_COLLAPSE, false);
        } else {
            TreeViewExpandRecursively(hwnd, TreeView_GetSelection(hwnd), TVE_COLLAPSE, true);
        }
    } else if (wp == 13) {
        // this is Enter key
        bool recursive = IsShiftPressed();
        TreeViewToggle(tree, TreeView_GetSelection(hwnd), recursive);
    } else {
        return false;
    }
    TreeView_EnsureVisible(hwnd, TreeView_GetSelection(hwnd));
    return true;
}

void TreeCtrl::WndProc(WndProcArgs* args) {
    HWND hwnd = args->hwnd;
    UINT msg = args->msg;
    WPARAM wp = args->wparam;

    TreeCtrl* w = this;
    CrashIf(w->hwnd != (HWND)hwnd);

    if (w->msgFilter) {
        w->msgFilter(args);
        if (args->didHandle) {
            return;
        }
    }

    if (WM_ERASEBKGND == msg) {
        args->didHandle = true;
        args->result = FALSE;
        return;
    }

    if (WM_KEYDOWN == msg) {
        if (HandleKey(w, wp)) {
            args->didHandle = true;
            return;
        }
    }
}

TreeCtrl::TreeCtrl(HWND p) {
    dwStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
              TVS_TRACKSELECT | TVS_DISABLEDRAGDROP | TVS_NOHSCROLL | TVS_INFOTIP;
    dwExStyle = 0;
    winClass = WC_TREEVIEWW;
    parent = p;
    SetRect(&initialPos, 0, 0, 48, 120);
}

bool TreeCtrl::Create(const WCHAR* title) {
    if (!title) {
        title = L"";
    }

    bool ok = WindowBase::Create();
    if (!ok) {
        return false;
    }
    Subclass();
    SubclassParent();

    TreeView_SetUnicodeFormat(hwnd, true);

    // TVS_CHECKBOXES has to be set with SetWindowLong before populating with data
    // https: // docs.microsoft.com/en-us/windows/win32/controls/tree-view-control-window-styles
    if (withCheckboxes) {
        ToggleWindowStyle(hwnd, TVS_CHECKBOXES, true);
    }

    return true;
}

bool TreeCtrl::IsExpanded(TreeItem* ti) {
    auto state = GetItemState(ti);
    return state.isExpanded;
}

// https://docs.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-treeview_getitemrect
bool TreeCtrl::GetTreeItemRect(TreeItem* ti, bool justText, RECT& r) {
    HTREEITEM hi = GetHandleByTreeItem(ti);
    BOOL b = toBOOL(justText);
    BOOL ok = TreeView_GetItemRect(hwnd, hi, &r, b);
    return ok == TRUE;
}

TreeItem* TreeCtrl::GetSelection() {
    HTREEITEM hi = TreeView_GetSelection(hwnd);
    return GetTreeItemByHandle(hi);
}

bool TreeCtrl::SelectItem(TreeItem* ti) {
    auto hi = GetHandleByTreeItem(ti);
    BOOL ok = TreeView_SelectItem(hwnd, hi);
    return ok == TRUE;
}

void TreeCtrl::SetBackgroundColor(COLORREF bgCol) {
    this->backgroundColor = bgCol;
    TreeView_SetBkColor(this->hwnd, bgCol);
}

void TreeCtrl::SetTextColor(COLORREF col) {
    this->textColor = col;
    TreeView_SetTextColor(this->hwnd, col);
}

void TreeCtrl::ExpandAll() {
    auto root = TreeView_GetRoot(this->hwnd);
    TreeViewExpandRecursively(this->hwnd, root, TVE_EXPAND, false);
}

void TreeCtrl::CollapseAll() {
    auto root = TreeView_GetRoot(this->hwnd);
    TreeViewExpandRecursively(this->hwnd, root, TVE_COLLAPSE, false);
}

void TreeCtrl::Clear() {
    treeModel = nullptr;
    insertedItems.clear();

    HWND hwnd = this->hwnd;
    ::SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(hwnd);
    SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
    UINT flags = RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN;
    ::RedrawWindow(hwnd, nullptr, nullptr, flags);
}

TreeCtrl::~TreeCtrl() {
    // DeleteObject(w->bgBrush);
}

str::WStr TreeCtrl::GetTooltip(TreeItem* ti) {
    auto hItem = GetHandleByTreeItem(ti);
    WCHAR buf[INFOTIPSIZE + 1] = {}; // +1 just in case

    TVITEMW item = {0};
    item.hItem = hItem;
    item.mask = TVIF_TEXT;
    item.pszText = buf;
    item.cchTextMax = dimof(buf);
    TreeView_GetItem(hwnd, &item);

    return str::WStr(buf);
}

HTREEITEM TreeCtrl::GetHandleByTreeItem(TreeItem* item) {
    for (auto t : this->insertedItems) {
        auto* i = std::get<0>(t);
        if (i == item) {
            return std::get<1>(t);
        }
    }
    return nullptr;
}

TreeItem* TreeCtrl::GetTreeItemByHandle(HTREEITEM item) {
    for (auto t : this->insertedItems) {
        auto* i = std::get<1>(t);
        if (i == item) {
            return std::get<0>(t);
        }
    }
    return nullptr;
}

static HTREEITEM InsertItem(TreeCtrl* tree, HTREEITEM parent, TreeItem* item) {
    TVINSERTSTRUCTW toInsert{};
    UINT mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;

    toInsert.hParent = parent;
    toInsert.hInsertAfter = TVI_LAST;
    toInsert.itemex.mask = mask;

    UINT stateMask = TVIS_EXPANDED;
    UINT state = 0;
    if (item->IsExpanded()) {
        state = TVIS_EXPANDED;
    }

    if (tree->withCheckboxes) {
        stateMask |= TVIS_STATEIMAGEMASK;
        bool isChecked = item->IsChecked();
        UINT imgIdx = isChecked ? 2 : 1;
        UINT imgState = INDEXTOSTATEIMAGEMASK(imgIdx);
        state |= imgState;
    }

    toInsert.itemex.state = state;
    toInsert.itemex.stateMask = stateMask;
    toInsert.itemex.lParam = reinterpret_cast<LPARAM>(item);
    auto title = item->Text();
    toInsert.itemex.pszText = title;
    HTREEITEM res = TreeView_InsertItem(tree->hwnd, &toInsert);
    return res;
}

static void PopulateTreeItem(TreeCtrl* tree, TreeItem* item, HTREEITEM parent) {
    int n = item->ChildCount();
    for (int i = 0; i < n; i++) {
        auto* ti = item->ChildAt(i);
        HTREEITEM h = InsertItem(tree, parent, ti);
        auto v = std::make_tuple(ti, h);
        tree->insertedItems.push_back(v);
        PopulateTreeItem(tree, ti, h);
    }
}

static void PopulateTree(TreeCtrl* tree, TreeModel* tm) {
    HTREEITEM parent = nullptr;
    int n = tm->RootCount();
    for (int i = 0; i < n; i++) {
        auto* ti = tm->RootAt(i);
        HTREEITEM h = InsertItem(tree, parent, ti);
        auto v = std::make_tuple(ti, h);
        tree->insertedItems.push_back(v);
        PopulateTreeItem(tree, ti, h);
    }
}

void TreeCtrl::SetTreeModel(TreeModel* tm) {
    CrashIf(!tm);

    SuspendRedraw();

    insertedItems.clear();
    TreeView_DeleteAllItems(hwnd);

    treeModel = tm;
    PopulateTree(this, tm);
    ResumeRedraw();

    UINT flags = RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN;
    RedrawWindow(hwnd, nullptr, nullptr, flags);
}

void TreeCtrl::SetCheckState(TreeItem* item, bool enable) {
    HTREEITEM hi = GetHandleByTreeItem(item);
    CrashIf(!hi);
    TreeView_SetCheckState(hwnd, hi, enable);
}

bool TreeCtrl::GetCheckState(TreeItem* item) {
    HTREEITEM hi = GetHandleByTreeItem(item);
    CrashIf(!hi);
    auto res = TreeView_GetCheckState(hwnd, hi);
    return res != 0;
}

TreeItemState TreeCtrl::GetItemState(TreeItem* ti) {
    TVITEMW* item = GetTVITEM(this, ti);
    CrashIf(!item);

    TreeItemState res;
    res.isExpanded = bitmask::IsSet(item->state, TVIS_EXPANDED);
    res.isSelected = bitmask::IsSet(item->state, TVIS_SELECTED);
    res.nChildren = item->cChildren;

    UINT n = (item->state >> 12) - 1;
    res.isChecked = n != 0;
    return res;
}
