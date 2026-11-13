/**
 * @file  BlameDlg.cpp
 *
 * @brief Implementation of the SVN blame options and progress dialog.
 */

#include "stdafx.h"
#include "BlameDlg.h"
#include "TrDialogs.h"
#include "OptionsMgr.h"
#include "OptionsDef.h"

#include <algorithm>
#include <vector>
#include <afxcmn.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	/** @brief Timer id used to poll the blame worker. */
	const UINT_PTR POLL_TIMER_ID = 1;

	/** @brief Poll interval in milliseconds. */
	const UINT POLL_INTERVAL_MS = 80;

	/**
	 * @brief True when the URL names a local file instead of a repository path.
	 *
	 * TortoiseSVN passes such a URL as %burl for files inside a working copy.
	 * It cannot be blamed, so the field asks for a real repository URL instead.
	 */
	bool IsFileUrl(const std::wstring& url)
	{
		return url.size() >= 7 && _wcsnicmp(url.c_str(), L"file://", 7) == 0;
	}
}

/**
 * @brief The real dialog window.
 */
class CBlameDlgImpl : public CTrDialog
{
public:
	explicit CBlameDlgImpl(CBlameDlg* owner, CWnd* parent)
		: CTrDialog(IDD_BLAME, parent)
		, m_owner(owner)
	{
	}

protected:
	BOOL OnInitDialog() override;
	void OnOK() override;
	void OnCancel() override;

	afx_msg void OnToRevisionChanged();
	afx_msg void OnPaneChanged();
	afx_msg void OnUrlChanged();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	DECLARE_MESSAGE_MAP()

private:
	void SetBusy(bool busy);
	void UpdateToRevisionState();
	/** @brief Enable OK only when the options can produce a run. */
	void UpdateOkState();
	/** @brief Fill the revision, URL and status fields for the selected pane. */
	void ApplyPaneSelection();
	void StartRun(const CBlameOptions& options);
	/** @brief Collapse the URL row when the file belongs to a working copy. */
	void ApplyUrlRowVisibility(bool showUrlRow);
	/** @brief True when the URL row must be filled in before blame can run. */
	bool NeedsRepositoryUrl() const { return m_needsUrl; }

	/** @brief Move the URL field left when its label is shorter than the widest translation. */
	void CompactUrlRow();
	/** @brief Remember the template geometry, so resizing can anchor to it. */
	void CaptureLayoutAnchors();

	/**
	 * @brief A child window whose place in the layout follows the dialog size.
	 *
	 * Only the controls that have to grow with the dialog (the URL field, the
	 * full width group boxes, the status line and the progress bar) and the two
	 * buttons are tracked; everything else stays where the URL row logic left it.
	 */
	struct LayoutAnchor
	{
		HWND hwnd;
		CRect rect;     /**< Position in the template layout, parent client pixels. */
		bool stretch;   /**< Right edge follows the dialog width. */
		bool toBottom;  /**< Moves with the dialog's bottom right corner. */
	};

	CBlameDlg* m_owner;
	std::unique_ptr<CBlameJob> m_job;
	bool m_busy = false;
	/** The file is not inside a working copy, so a repository URL is required. */
	bool m_needsUrl = false;
	/** True once the URL row has been removed from the layout. */
	bool m_urlRowHidden = false;
	/** Repository URL to blame, empty when it is derived from the working copy. */
	std::wstring m_url;
	std::vector<LayoutAnchor> m_anchors;
	/** Template client size and window size, both in pixels. */
	CSize m_initialClient;
	CSize m_minWindowSize;
};

BEGIN_MESSAGE_MAP(CBlameDlgImpl, CTrDialog)
	ON_BN_CLICKED(IDC_BLAME_TO_HEAD, &CBlameDlgImpl::OnToRevisionChanged)
	ON_BN_CLICKED(IDC_BLAME_TO_REV, &CBlameDlgImpl::OnToRevisionChanged)
	ON_BN_CLICKED(IDC_BLAME_PANE_LEFT, &CBlameDlgImpl::OnPaneChanged)
	ON_BN_CLICKED(IDC_BLAME_PANE_RIGHT, &CBlameDlgImpl::OnPaneChanged)
	ON_CBN_EDITCHANGE(IDC_BLAME_URL, &CBlameDlgImpl::OnUrlChanged)
	ON_CBN_SELCHANGE(IDC_BLAME_URL, &CBlameDlgImpl::OnUrlChanged)
	ON_WM_TIMER()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
END_MESSAGE_MAP()

BOOL CBlameDlgImpl::OnInitDialog()
{
	CTrDialog::OnInitDialog();

	SetDlgItemInt(IDC_BLAME_FROM, 1, FALSE);
	CheckDlgButton(IDC_BLAME_TO_HEAD, BST_CHECKED);
	CheckRadioButton(IDC_BLAME_WS_COMPARE, IDC_BLAME_WS_IGNORE_ALL, IDC_BLAME_WS_COMPARE);
	SetDlgItemText(IDC_BLAME_STATUS, L"Ready");

	// Columns are a global preference: the gutter is wide, so the user decides
	// once which of the three values it should show.
	CheckDlgButton(IDC_BLAME_SHOW_REV,
		GetOptionsMgr()->GetBool(OPT_BLAME_SHOW_REV) ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_BLAME_SHOW_AUTHOR,
		GetOptionsMgr()->GetBool(OPT_BLAME_SHOW_AUTHOR) ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_BLAME_SHOW_TIME,
		GetOptionsMgr()->GetBool(OPT_BLAME_SHOW_TIME) ? BST_CHECKED : BST_UNCHECKED);

	// Blame belongs to the file the user wants history for, and only one of the
	// two panes lives in a working copy in the usual diff of a modified file.
	CheckRadioButton(IDC_BLAME_PANE_LEFT, IDC_BLAME_PANE_RIGHT,
		m_owner->GetPane() == 1 ? IDC_BLAME_PANE_RIGHT : IDC_BLAME_PANE_LEFT);
	if (!m_owner->HasSecondPane())
	{
		if (CWnd* left = GetDlgItem(IDC_BLAME_PANE_LEFT))
			left->EnableWindow(FALSE);
		if (CWnd* right = GetDlgItem(IDC_BLAME_PANE_RIGHT))
			right->EnableWindow(FALSE);
	}

	// The URL field offers the URLs blame was run with before. A temp copy
	// exported by the log dialog cannot be asked for its repository path, so
	// picking a URL that worked for this file is the reliable way in.
	if (auto* combo = static_cast<CComboBox*>(GetDlgItem(IDC_BLAME_URL)))
	{
		for (const std::wstring& url : CBlameJob::GetRepositoryUrlHistory())
			combo->AddString(url.c_str());
	}

	CompactUrlRow();
	CaptureLayoutAnchors();

	ApplyPaneSelection();
	UpdateOkState();

	return TRUE;
}

/**
 * @brief Close the gap the template leaves between the URL label and its field.
 *
 * The template is laid out for the longest translation, so in most languages
 * the field sits further right than the text needs. Measured text decides the
 * new position, and the template position stays the limit: a language whose
 * label is wider than the template allows keeps the field where it is instead
 * of squeezing it.
 */
void CBlameDlgImpl::CompactUrlRow()
{
	CWnd* label = GetDlgItem(IDC_BLAME_URL_LABEL);
	CWnd* field = GetDlgItem(IDC_BLAME_URL);
	if (label == nullptr || field == nullptr)
		return;

	CString text;
	label->GetWindowText(text);
	CFont* font = label->GetFont();
	if (text.IsEmpty() || font == nullptr)
		return;

	CRect rcLabel, rcField;
	label->GetWindowRect(&rcLabel);
	ScreenToClient(&rcLabel);
	field->GetWindowRect(&rcField);
	ScreenToClient(&rcField);

	int textWidth = 0;
	{
		// DT_CALCRECT measures without drawing, and DT_HIDEPREFIX keeps an
		// accelerator introduced by a translation out of the width: the
		// ampersand is not drawn, so it must not count either.
		CClientDC dc(label);
		CFont* oldFont = dc.SelectObject(font);
		CRect rcText(0, 0, 0, 0);
		dc.DrawText(text, &rcText, DT_CALCRECT | DT_SINGLELINE | DT_HIDEPREFIX);
		textWidth = rcText.Width();
		dc.SelectObject(oldFont);
	}

	CRect rcGap(0, 0, 4, 0);
	MapDialogRect(&rcGap);
	CRect rcMinWidth(0, 0, 150, 0);
	MapDialogRect(&rcMinWidth);

	const int left = (std::min)(rcLabel.left + textWidth + rcGap.Width(), rcField.left);
	if (left <= rcLabel.left || rcField.right - left < rcMinWidth.Width())
		return;
	field->SetWindowPos(nullptr, left, rcField.top, rcField.right - left, rcField.Height(),
		SWP_NOZORDER | SWP_NOACTIVATE);
}

/**
 * @brief Remember the template geometry used to lay the dialog out again on resize.
 */
void CBlameDlgImpl::CaptureLayoutAnchors()
{
	CRect rcClient;
	GetClientRect(&rcClient);
	m_initialClient = rcClient.Size();

	CRect rcWindow;
	GetWindowRect(&rcWindow);
	m_minWindowSize = rcWindow.Size();

	m_anchors.clear();

	// Every full width control ends at the same margin as the URL field, which
	// is what identifies them without naming each of the group boxes.
	int rightEdge = rcClient.right;
	if (CWnd* field = GetDlgItem(IDC_BLAME_URL))
	{
		CRect rc;
		field->GetWindowRect(&rc);
		ScreenToClient(&rc);
		rightEdge = rc.right;
	}

	for (CWnd* child = GetWindow(GW_CHILD); child != nullptr; child = child->GetWindow(GW_HWNDNEXT))
	{
		CRect rc;
		child->GetWindowRect(&rc);
		ScreenToClient(&rc);

		const UINT id = static_cast<UINT>(child->GetDlgCtrlID());
		const bool toBottom = (id == IDOK || id == IDCANCEL);
		const bool stretch = !toBottom && rc.right == rightEdge;
		if (stretch || toBottom)
			m_anchors.push_back({ child->GetSafeHwnd(), rc, stretch, toBottom });
	}
}

void CBlameDlgImpl::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CTrDialog::OnGetMinMaxInfo(lpMMI);

	// Keep the dialog from being dragged smaller than the template, which is
	// the smallest size the layout is known to fit in.
	if (m_minWindowSize.cx > 0 && m_minWindowSize.cy > 0)
	{
		lpMMI->ptMinTrackSize.x = m_minWindowSize.cx;
		lpMMI->ptMinTrackSize.y = m_minWindowSize.cy;
	}
}

void CBlameDlgImpl::OnSize(UINT nType, int cx, int cy)
{
	CTrDialog::OnSize(nType, cx, cy);

	if (m_anchors.empty() || cx <= 0 || cy <= 0)
		return;

	// Positions are recomputed from the template rather than accumulated, so a
	// size change caused by hiding the URL row lands on the same result as the
	// move ApplyUrlRowVisibility() already did.
	const int dx = cx - m_initialClient.cx;
	const int dy = cy - m_initialClient.cy;

	for (const LayoutAnchor& anchor : m_anchors)
	{
		if (!::IsWindow(anchor.hwnd))
			continue;

		CRect rc = anchor.rect;
		if (anchor.stretch)
			rc.right += dx;
		if (anchor.toBottom)
			rc.OffsetRect(dx, dy);
		if (rc.Width() <= 0 || rc.Height() <= 0)
			continue;

		::SetWindowPos(anchor.hwnd, nullptr, rc.left, rc.top, rc.Width(), rc.Height(),
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

void CBlameDlgImpl::ApplyPaneSelection()
{
	const CBlamePane& pane = m_owner->CurrentPane();

	// A diff opened from the log dialog shows one specific revision of the file.
	// Blaming HEAD would attribute lines to changes the user cannot even see, so
	// stop at the revision TortoiseSVN reported with -brev, or at the revision
	// the companion pane is at when the compare supplied the URL.
	int pegRevision = CBlameJob::FindRepositoryRevision(pane.path);
	if (pegRevision <= 0)
		pegRevision = pane.hintRevision;
	if (pegRevision > 0)
	{
		CheckDlgButton(IDC_BLAME_TO_HEAD, BST_UNCHECKED);
		CheckDlgButton(IDC_BLAME_TO_REV, BST_CHECKED);
		SetDlgItemInt(IDC_BLAME_TO, static_cast<UINT>(pegRevision), FALSE);
	}
	else
	{
		CheckDlgButton(IDC_BLAME_TO_HEAD, BST_CHECKED);
		CheckDlgButton(IDC_BLAME_TO_REV, BST_UNCHECKED);
	}

	if (auto* progress = static_cast<CProgressCtrl*>(GetDlgItem(IDC_BLAME_PROGRESS)))
	{
		progress->SetRange32(0, (std::max)(1, pane.totalLines));
		progress->SetPos(0);
	}
	UpdateToRevisionState();

	// Files exported from the log dialog live in the temp folder and carry no
	// working copy information, so blame needs an explicit repository URL.
	m_needsUrl = !CBlameJob::IsInWorkingCopy(pane.path);
	if (m_needsUrl)
	{
		// A URL that provably belongs to this file is used as is: one
		// TortoiseSVN sent with -burl, then one derived from the companion
		// pane. Anything else stays empty and the user picks a URL they used
		// before from the history, or types one.
		m_url.clear();
		m_url = CBlameJob::FindRepositoryUrl(pane.path);
		if (IsFileUrl(m_url))
			m_url.clear();
		if (m_url.empty())
			m_url = pane.hintUrl;

		SetDlgItemText(IDC_BLAME_URL, m_url);
		SetDlgItemText(IDC_BLAME_STATUS,
			m_url.empty()
				? L"This file is not inside a working copy. Enter the repository URL of this file."
				: L"Blame will be read from the repository URL shown above.");
		if (m_url.empty())
		{
			if (CWnd* edit = GetDlgItem(IDC_BLAME_URL))
				edit->SetFocus();
		}
	}
	else
	{
		m_url.clear();
		SetDlgItemText(IDC_BLAME_URL, L"");
		SetDlgItemText(IDC_BLAME_STATUS,
			L"Blame will be read from the working copy.");
	}
	ApplyUrlRowVisibility(m_needsUrl);
}

void CBlameDlgImpl::OnPaneChanged()
{
	if (m_busy)
		return;
	m_owner->SetPane(IsDlgButtonChecked(IDC_BLAME_PANE_RIGHT) == BST_CHECKED ? 1 : 0);
	ApplyPaneSelection();
	UpdateOkState();
}

void CBlameDlgImpl::OnUrlChanged()
{
	UpdateOkState();
}

void CBlameDlgImpl::UpdateOkState()
{
	bool enable = !m_busy;
	if (enable && m_needsUrl)
	{
		String url;
		GetDlgItemText(IDC_BLAME_URL, url);
		// An empty URL cannot produce a run, so OK stays disabled until the
		// user supplies one instead of letting the click fail with a message.
		enable = !url.empty();
	}
	if (CWnd* ok = GetDlgItem(IDOK))
		ok->EnableWindow(enable);
}

void CBlameDlgImpl::ApplyUrlRowVisibility(bool showUrlRow)
{
	const bool hide = !showUrlRow;
	if (hide == m_urlRowHidden)
		return;

	// Nothing below the URL row is needed, so remove the row and pull the rest
	// of the dialog up by its height to avoid leaving an empty band. Switching
	// panes can bring the row back, which is why this has to be reversible.
	CRect rcRow(0, 0, 0, 22);
	MapDialogRect(&rcRow);
	const int delta = hide ? -rcRow.Height() : rcRow.Height();

	for (CWnd* child = GetWindow(GW_CHILD); child != nullptr; child = child->GetWindow(GW_HWNDNEXT))
	{
		const UINT id = static_cast<UINT>(child->GetDlgCtrlID());
		if (id == IDC_BLAME_URL || id == IDC_BLAME_URL_LABEL)
		{
			child->ShowWindow(hide ? SW_HIDE : SW_SHOW);
			continue;
		}
		CRect rc;
		child->GetWindowRect(&rc);
		ScreenToClient(&rc);
		child->SetWindowPos(nullptr, rc.left, rc.top + delta, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
	}

	CRect rcWindow;
	GetWindowRect(&rcWindow);
	SetWindowPos(nullptr, 0, 0, rcWindow.Width(), rcWindow.Height() + delta,
		SWP_NOMOVE | SWP_NOZORDER);

	m_urlRowHidden = hide;
}

void CBlameDlgImpl::UpdateToRevisionState()
{
	const bool specificRevision = IsDlgButtonChecked(IDC_BLAME_TO_REV) == BST_CHECKED;
	if (CWnd* edit = GetDlgItem(IDC_BLAME_TO))
		edit->EnableWindow(!m_busy && specificRevision);
}

void CBlameDlgImpl::OnToRevisionChanged()
{
	UpdateToRevisionState();
}

void CBlameDlgImpl::SetBusy(bool busy)
{
	m_busy = busy;

	const UINT optionControls[] =
	{
		IDC_BLAME_FROM, IDC_BLAME_TO_HEAD, IDC_BLAME_TO_REV, IDC_BLAME_TO,
		IDC_BLAME_IGNORE_EOL, IDC_BLAME_WS_COMPARE, IDC_BLAME_WS_IGNORE_CHANGES,
		IDC_BLAME_WS_IGNORE_ALL, IDC_BLAME_MERGE_INFO, IDC_BLAME_URL,
		IDC_BLAME_SHOW_REV, IDC_BLAME_SHOW_AUTHOR, IDC_BLAME_SHOW_TIME,
		IDC_BLAME_PANE_LEFT, IDC_BLAME_PANE_RIGHT
	};
	for (const UINT id : optionControls)
	{
		if (CWnd* control = GetDlgItem(id))
			control->EnableWindow(!busy);
	}
	if (!busy && !m_owner->HasSecondPane())
	{
		if (CWnd* left = GetDlgItem(IDC_BLAME_PANE_LEFT))
			left->EnableWindow(FALSE);
		if (CWnd* right = GetDlgItem(IDC_BLAME_PANE_RIGHT))
			right->EnableWindow(FALSE);
	}
	UpdateToRevisionState();
	UpdateOkState();
}

void CBlameDlgImpl::OnOK()
{
	if (m_busy)
		return;

	CBlameOptions options;
	String text;

	if (m_needsUrl)
	{
		String url;
		GetDlgItemText(IDC_BLAME_URL, url);
		if (url.empty() || url.find(_T("://")) == String::npos)
		{
			MessageBox(L"Enter the repository URL of this file, for example\n"
				L"https://server/repo/trunk/path/to/file.cs", L"Blame",
				MB_ICONWARNING | MB_OK);
			if (CWnd* edit = GetDlgItem(IDC_BLAME_URL))
				edit->SetFocus();
			return;
		}
		m_url = url;
		// Keep it for the dropdown, so the next compare that cannot work the
		// URL out can pick it instead of typing it again.
		CBlameJob::AddRepositoryUrlToHistory(url);
	}

	GetDlgItemText(IDC_BLAME_FROM, text);
	options.fromRevision = _wtoi(text.c_str());
	if (options.fromRevision < 0)
		options.fromRevision = 0;

	if (IsDlgButtonChecked(IDC_BLAME_TO_HEAD) == BST_CHECKED)
	{
		options.toRevision = -1;
	}
	else
	{
		GetDlgItemText(IDC_BLAME_TO, text);
		options.toRevision = _wtoi(text.c_str());
		if (options.toRevision <= 0)
		{
			MessageBox(L"Enter a revision number greater than zero.", L"Blame",
				MB_ICONWARNING | MB_OK);
			if (CWnd* edit = GetDlgItem(IDC_BLAME_TO))
				edit->SetFocus();
			return;
		}
		if (options.fromRevision > options.toRevision)
		{
			MessageBox(L"The \"From\" revision cannot be later than the \"To\" revision.",
				L"Blame", MB_ICONWARNING | MB_OK);
			return;
		}
	}

	options.ignoreEol = IsDlgButtonChecked(IDC_BLAME_IGNORE_EOL) == BST_CHECKED;
	if (IsDlgButtonChecked(IDC_BLAME_WS_IGNORE_ALL) == BST_CHECKED)
		options.whitespaceMode = 2;
	else if (IsDlgButtonChecked(IDC_BLAME_WS_IGNORE_CHANGES) == BST_CHECKED)
		options.whitespaceMode = 1;
	else
		options.whitespaceMode = 0;
	options.includeMergeInfo = IsDlgButtonChecked(IDC_BLAME_MERGE_INFO) == BST_CHECKED;

	options.showRevision = IsDlgButtonChecked(IDC_BLAME_SHOW_REV) == BST_CHECKED;
	options.showAuthor = IsDlgButtonChecked(IDC_BLAME_SHOW_AUTHOR) == BST_CHECKED;
	options.showTime = IsDlgButtonChecked(IDC_BLAME_SHOW_TIME) == BST_CHECKED;
	if (!options.showRevision && !options.showAuthor && !options.showTime)
	{
		MessageBox(L"Select at least one column to show in the blame gutter.",
			L"Blame", MB_ICONWARNING | MB_OK);
		return;
	}
	GetOptionsMgr()->SaveOption(OPT_BLAME_SHOW_REV, options.showRevision);
	GetOptionsMgr()->SaveOption(OPT_BLAME_SHOW_AUTHOR, options.showAuthor);
	GetOptionsMgr()->SaveOption(OPT_BLAME_SHOW_TIME, options.showTime);

	m_owner->m_options = options;
	StartRun(options);
}

void CBlameDlgImpl::StartRun(const CBlameOptions& options)
{
	const CBlamePane& pane = m_owner->CurrentPane();
	if (auto* progress = static_cast<CProgressCtrl*>(GetDlgItem(IDC_BLAME_PROGRESS)))
	{
		progress->SetRange32(0, (std::max)(1, pane.totalLines));
		progress->SetPos(0);
	}
	SetDlgItemText(IDC_BLAME_STATUS, L"Retrieving blame...");
	SetBusy(true);

	m_job = std::make_unique<CBlameJob>();
	if (!m_job->Start(pane.path, m_url, options, pane.totalLines))
	{
		m_job.reset();
		SetBusy(false);
		SetDlgItemText(IDC_BLAME_STATUS, L"Ready");
		MessageBox(L"Could not start the blame worker thread.", L"Blame",
			MB_ICONERROR | MB_OK);
		return;
	}

	SetTimer(POLL_TIMER_ID, POLL_INTERVAL_MS, nullptr);
}

void CBlameDlgImpl::OnCancel()
{
	if (m_busy)
	{
		if (m_job != nullptr)
			m_job->RequestCancel();
		SetDlgItemText(IDC_BLAME_STATUS, L"Cancelling...");
		if (CWnd* cancel = GetDlgItem(IDCANCEL))
			cancel->EnableWindow(false);
		return;
	}
	CTrDialog::OnCancel();
}

void CBlameDlgImpl::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent != POLL_TIMER_ID || m_job == nullptr)
	{
		CTrDialog::OnTimer(nIDEvent);
		return;
	}

	if (!m_job->IsFinished())
	{
		const int total = (std::max)(1, m_job->GetTotalLines());
		const int parsed = (std::min)(m_job->GetParsedLines(), total);
		if (auto* progress = static_cast<CProgressCtrl*>(GetDlgItem(IDC_BLAME_PROGRESS)))
			progress->SetPos(parsed);

		wchar_t status[160] = {};
		swprintf_s(status, L"Retrieving blame... %d of %d lines", parsed, m_job->GetTotalLines());
		SetDlgItemText(IDC_BLAME_STATUS, status);
		return;
	}

	KillTimer(POLL_TIMER_ID);

	std::shared_ptr<CBlameResult> result = m_job->TakeResult();
	const bool cancelled = m_job->WasCancelled();
	m_job.reset();

	if (cancelled)
	{
		m_owner->m_cancelled = true;
		EndDialog(IDCANCEL);
		return;
	}

	if (!result || !result->ok)
	{
		const std::wstring error = (result != nullptr && !result->errorText.empty())
			? result->errorText : std::wstring(L"Could not retrieve blame data.");
		BlameTrace(L"BlameDlg: run failed: %s", error.c_str());
		SetBusy(false);
		SetDlgItemText(IDC_BLAME_STATUS, L"Ready");
		if (auto* progress = static_cast<CProgressCtrl*>(GetDlgItem(IDC_BLAME_PROGRESS)))
			progress->SetPos(0);
		MessageBox(error.c_str(), L"Blame", MB_ICONWARNING | MB_OK);
		return;
	}

	m_owner->m_result = result;
	EndDialog(IDOK);
}

CBlameDlg::CBlameDlg(const CBlamePane& left, const CBlamePane& right)
{
	m_panes[0] = left;
	m_panes[1] = right;
}

void CBlameDlg::SetPane(int pane)
{
	if (pane < 0 || pane > 1)
		return;
	if (pane == 1 && m_panes[1].path.empty())
		return;
	m_pane = pane;
}

CBlameDlg::~CBlameDlg() = default;

bool CBlameDlg::DoModal(CWnd* parent)
{
	CBlameDlgImpl dialog(this, parent);
	return dialog.DoModal() == IDOK;
}
