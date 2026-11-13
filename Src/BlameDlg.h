/**
 * @file  BlameDlg.h
 *
 * @brief Options dialog for the SVN blame gutter.
 */
#pragma once

#include <memory>
#include <string>

#include "BlameGutter.h"

class CWnd;

/**
 * @brief One side of the compare, as far as blame is concerned.
 *
 * Both panes are described up front so the dialog can switch between them
 * without going back to the view: blame is often wanted for the right pane,
 * which is the file that actually lives in a working copy.
 */
struct CBlamePane
{
	/** Local path of the file shown in the pane. */
	std::wstring path;
	/** Buffer line count, used to scale the progress bar. */
	int totalLines = 0;
	/** Repository URL the compare suggests for this pane. */
	std::wstring hintUrl;
	/** Revision the hinted file is at, used as the default peg revision. */
	int hintRevision = 0;
};

/**
 * @brief Lets the user pick a revision range and diff options, then shows
 *        progress while the blame data is being fetched.
 *
 * The dialog stays modal for the whole operation: the fetching itself happens
 * on a worker thread owned by CBlameJob, and the dialog polls it from a timer
 * so the progress bar advances and Cancel stays responsive.
 */
class CBlameDlg
{
public:
	/**
	 * @param [in] left  The left pane of the compare.
	 * @param [in] right The right pane of the compare.
	 */
	CBlameDlg(const CBlamePane& left, const CBlamePane& right);
	~CBlameDlg();

	/**
	 * @brief Show the dialog and run the blame when the user confirms.
	 * @return true when blame data was fetched successfully.
	 */
	bool DoModal(CWnd* parent);

	/** @brief Result of a successful run. Never null after DoModal returns true. */
	std::shared_ptr<CBlameResult> GetResult() const { return m_result; }

	/** @brief Options the user confirmed. */
	const CBlameOptions& GetOptions() const { return m_options; }

	/** @brief Pane the user chose to blame: 0 for the left, 1 for the right. */
	int GetPane() const { return m_pane; }

	/** @brief Pane description currently selected. */
	const CBlamePane& CurrentPane() const { return m_panes[m_pane]; }

	/** @brief Switch the selected pane, clamped to the panes that exist. */
	void SetPane(int pane);

	/** @brief True when the user cancelled the fetch. */
	bool WasCancelled() const { return m_cancelled; }

	/** @brief True when both panes hold a file, so the pane selector is useful. */
	bool HasSecondPane() const { return !m_panes[1].path.empty(); }

private:
	friend class CBlameDlgImpl;

	CBlamePane m_panes[2];
	int m_pane = 0;
	CBlameOptions m_options;
	std::shared_ptr<CBlameResult> m_result;
	bool m_cancelled = false;
};
