/**
 * @file  BlameGutter.h
 *
 * @brief SVN blame retrieval and per-line metadata for the compare view gutter.
 *
 * The blame data is fetched with "svn blame --xml" which is the only stable
 * machine readable interface.  Retrieval runs on a worker thread because it may
 * take seconds on a network repository, and the caller is expected to poll the
 * job so it can show progress and offer cancellation.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include "UnicodeString.h"

/**
 * @brief Blame metadata of a single source line.
 */
struct CBlameLine
{
	/** SVN revision that introduced the line, or -1 when the line is a local
	 *  modification that has not been committed yet. */
	int revision = -1;

	/** Commit author. Empty for local modifications. */
	std::wstring author;

	/** Commit date and time, already formatted for the current locale.
	 *  Empty for local modifications. */
	std::wstring when;
};

/**
 * @brief Options the user selected in the blame dialog.
 */
struct CBlameOptions
{
	/** First revision to look at. 0 means "from the very first revision". */
	int fromRevision = 0;

	/** Last revision to look at. -1 means HEAD. */
	int toRevision = -1;

	/** Pass --ignore-eol-style so pure end-of-line changes are not blamed. */
	bool ignoreEol = false;

	/** 0 = compare whitespace, 1 = ignore whitespace changes (-b),
	 *  2 = ignore all whitespace (-w). */
	int whitespaceMode = 0;

	/** Pass -g so revisions reachable through a merge are reported as well. */
	bool includeMergeInfo = false;

	/** Show the revision column. */
	bool showRevision = true;

	/** Show the author column. */
	bool showAuthor = true;

	/** Show the date and time column. */
	bool showTime = true;
};

/**
 * @brief Result of one blame run.
 */
struct CBlameResult
{
	/** True when the run completed and the XML was parsed. */
	bool ok = false;

	/** Message to show the user when ok is false. */
	std::wstring errorText;

	/** Path svn blamed, as reported in the <target> element. */
	std::wstring targetPath;

	/** Blame entries indexed by 0 based real line number. Entries can be
	 *  missing when the requested revision range does not cover every line. */
	std::vector<CBlameLine> lines;
};

/**
 * @brief Accent colours the blame gutter mixes into its background.
 *
 * One slot per author; the busiest author keeps the plain gutter colour and
 * does not consume a slot.  A colour scheme may override the values, which is
 * how the dark schemes get tones that suit a dark gutter while the default
 * scheme gets pale ones that suit a light one.
 */
namespace BlameTint
{
	/** @brief Number of slots handed out before the colours start repeating. */
	inline constexpr int COUNT = 12;

	/** @brief Option holding the colour of one slot. */
	const String& GetOptionName(int index);

	/** @brief Colour configured for one slot. */
	COLORREF GetColor(int index);

	/** @brief Built in colour of one slot, tuned for a light scheme. */
	COLORREF GetDefaultColor(int index);
}

/**
 * @brief Runs "svn blame" on a worker thread and parses its XML output.
 *
 * Usage: Start() once, then poll IsFinished() (typically from a timer), and
 * finally TakeResult().  RequestCancel() may be called at any time; the worker
 * kills the child process and finishes with a cancelled result.
 */
/** @brief Diagnostic trace to %TEMP%\winmerge-blame-trace.log. Temporary. */
void BlameTrace(const wchar_t* format, ...);

class CBlameJob
{
public:
	CBlameJob();
	~CBlameJob();

	CBlameJob(const CBlameJob&) = delete;
	CBlameJob& operator=(const CBlameJob&) = delete;

	/**
	 * @brief Start the background run.
	 * @param [in] path Local path of the file to blame.
	 * @param [in] url Repository URL to use when the file is not inside a
	 *             working copy (for example a file exported from the log
	 *             dialog). Empty means "derive it from the working copy".
	 * @param [in] options Revision range and diff options.
	 * @param [in] totalLines Line count used as the progress bar range.
	 * @return false when the thread could not be created.
	 */
	bool Start(const std::wstring& path, const std::wstring& url,
		const CBlameOptions& options, int totalLines);

	/** @brief True while the worker thread is alive. */
	bool IsRunning() const;

	/** @brief True once the worker finished, either with a result or cancelled. */
	bool IsFinished() const;

	/** @brief Number of blame entries parsed so far, used for progress. */
	int GetParsedLines() const;

	/** @brief Total number of lines, used as the progress bar range. */
	int GetTotalLines() const;

	/** @brief Ask the worker to stop. Returns immediately. */
	void RequestCancel();

	/** @brief True when the user cancelled the run. */
	bool WasCancelled() const;

	/**
	 * @brief Take the finished result.
	 * @return The result, or nullptr while the job is still running. Ownership
	 *         transfers to the caller, a second call returns nullptr.
	 */
	std::shared_ptr<CBlameResult> TakeResult();

	/**
	 * @brief Locate svn.exe.
	 * @return Full path of svn.exe, or an empty string when nothing was found.
	 */
	static std::wstring FindSvnExecutable();

	/**
	 * @brief Turn a working copy path into a repository URL.
	 *
	 * Local paths are converted to a URL because svn's command line handling
	 * mangles non ASCII paths through the system ANSI code page.  URLs are pure
	 * ASCII and therefore immune to that, so a Korean or Japanese path still
	 * works.
	 *
	 * @param [in]  path  Local file path.
	 * @param [out] url   Repository URL of the file.
	 * @param [out] error Failure description when the function returns false.
	 * @return true on success.
	 */
	static bool ResolveFileUrl(const std::wstring& path, std::wstring& url, std::wstring& error);

	/**
	 * @brief Tell whether a path belongs to a Subversion working copy.
	 *
	 * Purely a filesystem check (looks for a .svn directory in the parent
	 * chain), so it never spawns a process.
	 */
	static bool IsInWorkingCopy(const std::wstring& path);

	/**
	 * @brief Convert a "file://" URL to a local path.
	 * @return The path, or an empty string when the URL is not a file URL.
	 */
	static std::wstring LocalPathFromFileUrl(const std::wstring& url);

	/**
	 * @brief Tell whether an URL is the file:// form of a file in a working copy.
	 *
	 * TortoiseSVN expands %burl to such an URL for paths inside a working
	 * copy. It names the comparison file, not a repository, so it must not be
	 * handed to svn blame; the working copy it points into is used to derive
	 * the real repository URL instead.
	 */
	static bool IsWorkingCopyFileUrl(const std::wstring& url);

	/**
	 * @brief Remember a repository URL supplied on the command line.
	 *
	 * TortoiseSVN passes %burl for the base file, which makes blame work for
	 * files opened from the log dialog even though those are extracted to the
	 * temp folder and therefore sit outside any working copy.
	 *
	 * @param [in] path Local path of the file the URL belongs to.
	 * @param [in] url  Repository URL of that file.
	 */
	static void RegisterRepositoryUrl(const std::wstring& path, const std::wstring& url);

	/**
	 * @brief Remember a peg revision supplied on the command line.
	 *
	 * TortoiseSVN passes %brev for the base file. When a diff is opened from
	 * the log dialog the left pane shows that revision, so blame has to stop
	 * there; blaming HEAD would report lines from a newer file state.
	 *
	 * @param [in] path     Local path of the file the revision belongs to.
	 * @param [in] revision Revision number, 0 or less when unknown.
	 */
	static void RegisterRepositoryRevision(const std::wstring& path, int revision);

	/**
	 * @brief Look up the URL registered for a path.
	 * @return The URL, or an empty string when nothing was registered.
	 */
	static std::wstring FindRepositoryUrl(const std::wstring& path);

	/**
	 * @brief Look up the peg revision registered for a path.
	 * @return The revision, or 0 when nothing was registered.
	 */
	static int FindRepositoryRevision(const std::wstring& path);

	/**
	 * @brief Repository URLs that were used for blame before, most recent first.
	 *
	 * There is no reliable way to work out the URL of a temp copy the log
	 * dialog exports, so the dialog offers the URLs the user already confirmed
	 * and lets them pick one instead of guessing.
	 */
	static std::vector<std::wstring> GetRepositoryUrlHistory();

	/**
	 * @brief Add a URL to the history, moving it to the front.
	 *
	 * An existing entry is moved rather than duplicated, and only the most
	 * recent entries are kept.
	 */
	static void AddRepositoryUrlToHistory(const std::wstring& url);

	/**
	 * @brief Tell whether a repository URL describes the given local file.
	 *
	 * Compares the last segment of the URL with the file name, ignoring the
	 * extension because the log dialog exports revisions to the temp folder
	 * under names that may lack it. The " Revision <number>" suffix the log
	 * dialog appends to those temp names is dropped as well, so a pane showing
	 * "DynamicBoneNodeArt.cs Revision 5316825" still matches the URL of
	 * "DynamicBoneNodeArt.cs". Used to reject a URL left over from an earlier
	 * compare, which would otherwise blame an unrelated file.
	 */
	static bool UrlMatchesFile(const std::wstring& url, const std::wstring& path);

	/**
	 * @brief Ask svn which revision a working copy file is at.
	 *
	 * This is the revision the BASE copy shows, so it is the right peg
	 * revision when blaming a file that was diffed from the working copy.
	 * @return The revision, or 0 when it could not be determined.
	 */
	static int QueryWorkingCopyRevision(const std::wstring& path);

private:
	static unsigned __stdcall ThreadProc(void* param);
	void Run();

	/** @brief File the user asked us to blame. */
	std::wstring m_path;

	/** @brief Repository URL override, used when the path is not in a working copy. */
	std::wstring m_url;

	/** @brief Options captured when the job was started. */
	CBlameOptions m_options;

	HANDLE m_hThread = nullptr;
	HANDLE m_hCancel = nullptr;

	std::atomic<bool> m_running{ false };
	std::atomic<bool> m_finished{ false };
	std::atomic<bool> m_cancelled{ false };
	std::atomic<int> m_parsedLines{ 0 };
	std::atomic<int> m_totalLines{ 0 };

	std::shared_ptr<CBlameResult> m_result;
};
