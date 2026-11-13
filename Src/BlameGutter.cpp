/**
 * @file  BlameGutter.cpp
 *
 * @brief Implementation of the SVN blame retrieval used by the compare view gutter.
 */

#include "stdafx.h"
#include "BlameGutter.h"
#include "MergeApp.h"
#include "OptionsMgr.h"
#include "OptionsDef.h"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <process.h>
#include <vector>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	const wchar_t* const SVN_EXE_NAME = L"svn.exe";

	/** @brief Standard locations to look for svn.exe when it is not on PATH. */
	const wchar_t* const SVN_FALLBACK_PATHS[] =
	{
		L"%ProgramFiles%\\TortoiseSVN\\bin\\svn.exe",
		L"%ProgramW6432%\\TortoiseSVN\\bin\\svn.exe",
		L"%ProgramFiles(x86)%\\TortoiseSVN\\bin\\svn.exe",
		L"%ProgramFiles%\\SlikSvn\\bin\\svn.exe",
		L"%SystemDrive%\\SlikSvn\\bin\\svn.exe",
	};

	/** @brief Handles of a child process whose stdin/stdout/stderr are redirected. */
	struct ChildProcess
	{
		HANDLE hProcess = nullptr;
		HANDLE hStdOut = nullptr;
		HANDLE hStdErr = nullptr;
	};

	std::wstring ExpandEnvironment(const wchar_t* value)
	{
		wchar_t buffer[MAX_PATH * 2] = {};
		const DWORD length = ExpandEnvironmentStringsW(value, buffer, _countof(buffer));
		if (length == 0 || length > _countof(buffer))
			return std::wstring();
		return std::wstring(buffer);
	}

	void CloseChild(ChildProcess& child)
	{
		if (child.hStdOut != nullptr)
			CloseHandle(child.hStdOut);
		if (child.hStdErr != nullptr)
			CloseHandle(child.hStdErr);
		if (child.hProcess != nullptr)
			CloseHandle(child.hProcess);
		child = ChildProcess();
	}

	void KillChild(ChildProcess& child)
	{
		if (child.hProcess != nullptr)
		{
			TerminateProcess(child.hProcess, static_cast<UINT>(-1));
			WaitForSingleObject(child.hProcess, 3000);
		}
	}

	/** @brief Quote one command line argument. */
	std::wstring QuoteArg(const std::wstring& value)
	{
		std::wstring quoted = L"\"";
		for (const wchar_t c : value)
		{
			if (c == L'"')
				quoted += L'\\';
			quoted += c;
		}
		quoted += L'"';
		return quoted;
	}

	/**
	 * @brief Start a child process with stdout and stderr redirected to pipes.
	 */
	bool Spawn(const std::wstring& commandLine, const std::wstring& workDir,
		ChildProcess& child, std::wstring& error)
	{
		SECURITY_ATTRIBUTES security = {};
		security.nLength = sizeof security;
		security.bInheritHandle = TRUE;

		HANDLE outRead = nullptr, outWrite = nullptr;
		HANDLE errRead = nullptr, errWrite = nullptr;
		if (!CreatePipe(&outRead, &outWrite, &security, 0))
		{
			error = L"CreatePipe failed";
			return false;
		}
		if (!CreatePipe(&errRead, &errWrite, &security, 0))
		{
			CloseHandle(outRead);
			CloseHandle(outWrite);
			error = L"CreatePipe failed";
			return false;
		}

		// Only the write ends may be inherited by the child.
		SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW startup = {};
		startup.cb = sizeof startup;
		startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = outWrite;
		startup.hStdError = errWrite;

		PROCESS_INFORMATION info = {};
		std::wstring mutableCommandLine = commandLine;
		const BOOL created = CreateProcessW(nullptr, &mutableCommandLine[0], nullptr, nullptr,
			TRUE, CREATE_NO_WINDOW, nullptr,
			workDir.empty() ? nullptr : workDir.c_str(), &startup, &info);

		// The child owns the write ends now; keeping them open here would prevent
		// the read ends from ever reporting end of file.
		CloseHandle(outWrite);
		CloseHandle(errWrite);

		if (!created)
		{
			CloseHandle(outRead);
			CloseHandle(errRead);
			error = L"Could not start svn.exe";
			return false;
		}

		CloseHandle(info.hThread);
		child.hProcess = info.hProcess;
		child.hStdOut = outRead;
		child.hStdErr = errRead;
		return true;
	}

	/** @brief Called for every chunk read from stdout. */
	using ReadCallback = std::function<void(const char* data, size_t length)>;

	/** @brief Called in the polling loop to ask whether the work should stop. */
	using StopPredicate = std::function<bool()>;

	/**
	 * @brief Drain stdout and stderr until both are closed.
	 *
	 * Both pipes are polled together so a chatty stderr can never deadlock the
	 * child while we sit blocked on stdout.
	 *
	 * @return false when the timeout expired or the stop predicate fired. The
	 *         child process is killed in both cases.
	 */
	bool PumpPipes(ChildProcess& child, std::string& out, std::string& err,
		ReadCallback onStdout, StopPredicate stop, int timeoutMs, bool& stopped)
	{
		const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
		char buffer[8192];
		bool outOpen = true;
		bool errOpen = true;
		stopped = false;

		while (outOpen || errOpen)
		{
			if (stop && stop())
			{
				stopped = true;
				KillChild(child);
				return false;
			}
			if (GetTickCount64() > deadline)
			{
				KillChild(child);
				return false;
			}

			bool progressed = false;
			for (int i = 0; i < 2; ++i)
			{
				HANDLE pipe = (i == 0) ? child.hStdOut : child.hStdErr;
				bool& open = (i == 0) ? outOpen : errOpen;
				std::string& sink = (i == 0) ? out : err;
				if (!open || pipe == nullptr)
					continue;

				DWORD available = 0;
				if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
				{
					open = false;
					continue;
				}
				if (available == 0)
					continue;

				DWORD read = 0;
				const DWORD want = static_cast<DWORD>(std::min<size_t>(available, sizeof buffer));
				if (!ReadFile(pipe, buffer, want, &read, nullptr) || read == 0)
				{
					open = false;
					continue;
				}
				sink.append(buffer, read);
				if (i == 0 && onStdout)
					onStdout(buffer, read);
				progressed = true;
			}

			if (!progressed)
				Sleep(10);
		}

		WaitForSingleObject(child.hProcess, 5000);
		return true;
	}

	DWORD GetChildExitCode(ChildProcess& child)
	{
		DWORD exitCode = static_cast<DWORD>(-1);
		if (child.hProcess != nullptr)
			GetExitCodeProcess(child.hProcess, &exitCode);
		return exitCode;
	}

	/**
	 * @brief Convert bytes coming from svn into a wide string.
	 *
	 * The XML output is always UTF-8, but diagnostic messages on stderr use the
	 * console code page. Try UTF-8 first and fall back to the ANSI code page.
	 */
	std::wstring FromBytes(const std::string& bytes)
	{
		if (bytes.empty())
			return std::wstring();

		const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			bytes.c_str(), static_cast<int>(bytes.size()), nullptr, 0);
		const UINT codePage = (length > 0) ? CP_UTF8 : CP_ACP;
		const DWORD flags = (codePage == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;

		const int needed = MultiByteToWideChar(codePage, flags, bytes.c_str(),
			static_cast<int>(bytes.size()), nullptr, 0);
		if (needed <= 0)
			return std::wstring();

		std::wstring result(static_cast<size_t>(needed), L'\0');
		MultiByteToWideChar(codePage, flags, bytes.c_str(), static_cast<int>(bytes.size()),
			&result[0], needed);
		return result;
	}

	std::wstring TrimWhitespace(const std::wstring& value)
	{
		const size_t first = value.find_first_not_of(L" \t\r\n");
		if (first == std::wstring::npos)
			return std::wstring();
		const size_t last = value.find_last_not_of(L" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	/** @brief Last path segment without its extension. Defined below. */
	std::wstring BaseNameWithoutExtension(const std::wstring& value);

	/** @brief Run a command and capture its stdout. Used for the short svn info call. */	bool RunCapture(const std::wstring& commandLine, const std::wstring& workDir,
		std::wstring& output, std::wstring& error)
	{
		ChildProcess child;
		if (!Spawn(commandLine, workDir, child, error))
			return false;

		std::string out, err;
		bool stopped = false;
		PumpPipes(child, out, err, ReadCallback(), StopPredicate(), 15000, stopped);
		const DWORD exitCode = GetChildExitCode(child);
		CloseChild(child);

		output = TrimWhitespace(FromBytes(out));
		if (exitCode != 0 || output.empty())
		{
			error = TrimWhitespace(FromBytes(err));
			if (error.empty())
				error = L"svn info failed";
			return false;
		}
		return true;
	}

	/** @brief Find the working copy root above a path, or an empty string. */
	std::wstring FindWorkingCopyRoot(const std::wstring& path)
	{
		std::wstring dir = path;
		const size_t slash = dir.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			dir.erase(slash);

		for (;;)
		{
			if (dir.empty())
				return std::wstring();

			// Since Subversion 1.7 the metadata lives in a single .svn directory
			// at the root of the working copy.
			const std::wstring candidate = dir + L"\\.svn";
			const DWORD attributes = GetFileAttributesW(candidate.c_str());
			if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
				return dir;

			const size_t parent = dir.find_last_of(L"\\/");
			if (parent == std::wstring::npos)
				return std::wstring();

			// Keep a trailing separator for drive roots such as "C:\".
			if (parent == 2 && dir[1] == L':')
				dir.erase(2);
			else
				dir.erase(parent);
		}
	}

	/** @brief Encode one path segment as UTF-8 percent escapes. */
	std::wstring EncodeUrlSegment(const std::wstring& segment)
	{
		static const wchar_t* const hexDigits = L"0123456789ABCDEF";

		const int needed = WideCharToMultiByte(CP_UTF8, 0, segment.c_str(),
			static_cast<int>(segment.size()), nullptr, 0, nullptr, nullptr);
		if (needed <= 0)
			return std::wstring();

		std::string bytes(static_cast<size_t>(needed), '\0');
		WideCharToMultiByte(CP_UTF8, 0, segment.c_str(), static_cast<int>(segment.size()),
			&bytes[0], needed, nullptr, nullptr);

		std::wstring encoded;
		encoded.reserve(bytes.size() * 3);
		for (const unsigned char c : bytes)
		{
			const bool unreserved =
				(c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') ||
				c == '-' || c == '_' || c == '.' || c == '~';
			if (unreserved)
			{
				encoded += static_cast<wchar_t>(c);
			}
			else
			{
				encoded += L'%';
				encoded += hexDigits[(c >> 4) & 0x0F];
				encoded += hexDigits[c & 0x0F];
			}
		}
		return encoded;
	}

	/** @brief Encode a relative path, keeping the directory separators. */
	std::wstring EncodeRelativePath(std::wstring relative)
	{
		std::replace(relative.begin(), relative.end(), L'\\', L'/');

		std::wstring encoded;
		size_t start = 0;
		while (start <= relative.size())
		{
			const size_t slash = relative.find(L'/', start);
			const std::wstring segment = relative.substr(start,
				slash == std::wstring::npos ? std::wstring::npos : slash - start);
			if (!segment.empty())
			{
				if (!encoded.empty())
					encoded += L'/';
				encoded += EncodeUrlSegment(segment);
			}
			if (slash == std::wstring::npos)
				break;
			start = slash + 1;
		}
		return encoded;
	}

	/** @brief Decode the XML entities that can appear inside author names. */
	std::wstring DecodeXmlEntities(const std::wstring& value)
	{
		if (value.find(L'&') == std::wstring::npos)
			return value;

		std::wstring decoded;
		decoded.reserve(value.size());
		for (size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] != L'&')
			{
				decoded += value[i];
				continue;
			}

			const size_t end = value.find(L';', i);
			if (end == std::wstring::npos)
			{
				decoded += value[i];
				continue;
			}

			const std::wstring entity = value.substr(i + 1, end - i - 1);
			bool replaced = true;
			if (entity == L"amp")
				decoded += L'&';
			else if (entity == L"lt")
				decoded += L'<';
			else if (entity == L"gt")
				decoded += L'>';
			else if (entity == L"quot")
				decoded += L'"';
			else if (entity == L"apos")
				decoded += L'\'';
			else if (entity.size() > 1 && entity[0] == L'#')
			{
				// Numeric character reference, decimal or hexadecimal.
				const bool hex = (entity[1] == L'x' || entity[1] == L'X');
				const std::wstring digits = entity.substr(hex ? 2 : 1);
				const unsigned long code = wcstoul(digits.c_str(), nullptr, hex ? 16 : 10);
				if (code > 0 && code <= 0x10FFFF)
					decoded += static_cast<wchar_t>(code);
				else
					replaced = false;
			}
			else
				replaced = false;

			if (replaced)
				i = end;
			else
				decoded += value[i];
		}
		return decoded;
	}

	/**
	 * @brief Read an integer attribute from a start tag.
	 * @return The value, or fallback when the attribute is absent.
	 */
	int ReadIntAttribute(const std::wstring& tag, const wchar_t* name, int fallback)
	{
		const std::wstring pattern = std::wstring(L" ") + name + L"=\"";
		const size_t pos = tag.find(pattern);
		if (pos == std::wstring::npos)
			return fallback;
		const size_t valueStart = pos + pattern.size();
		const size_t valueEnd = tag.find(L'"', valueStart);
		if (valueEnd == std::wstring::npos)
			return fallback;
		return _wtoi(tag.substr(valueStart, valueEnd - valueStart).c_str());
	}

	/**
	 * @brief Read a string attribute from a start tag, decoding entities.
	 * @return The value, or an empty string when the attribute is absent.
	 */
	std::wstring ReadAttribute(const std::wstring& tag, const wchar_t* name)
	{
		const std::wstring pattern = std::wstring(L" ") + name + L"=\"";
		const size_t pos = tag.find(pattern);
		if (pos == std::wstring::npos)
			return std::wstring();
		const size_t valueStart = pos + pattern.size();
		const size_t valueEnd = tag.find(L'"', valueStart);
		if (valueEnd == std::wstring::npos)
			return std::wstring();
		return DecodeXmlEntities(tag.substr(valueStart, valueEnd - valueStart));
	}

	/** @brief Extract the text between <name> and </name>, or an empty string. */
	std::wstring ReadElementText(const std::wstring& block, const wchar_t* name)
	{
		const std::wstring open = std::wstring(L"<") + name + L">";
		const std::wstring close = std::wstring(L"</") + name + L">";
		const size_t start = block.find(open);
		if (start == std::wstring::npos)
			return std::wstring();
		const size_t end = block.find(close, start + open.size());
		if (end == std::wstring::npos)
			return std::wstring();
		return DecodeXmlEntities(block.substr(start + open.size(), end - start - open.size()));
	}

	/**
	 * @brief Format an ISO-8601 UTC timestamp for display.
	 *
	 * The date uses the locale short date format and the time uses the locale
	 * time format, so both the field order and the 12/24 hour convention follow
	 * the system settings.
	 */
	std::wstring FormatIsoTimestamp(const std::wstring& iso)
	{
		int parts[6] = {};
		const wchar_t* cursor = iso.c_str();
		for (int i = 0; i < 6; ++i)
		{
			if (*cursor == 0)
				return std::wstring();
			wchar_t* stop = nullptr;
			parts[i] = static_cast<int>(wcstol(cursor, &stop, 10));
			if (stop == cursor)
				return std::wstring();
			cursor = stop;
			if (*cursor != 0)
				++cursor; // skip '-', '-', 'T', ':' or ':'
		}

		SYSTEMTIME utc = {};
		utc.wYear = static_cast<WORD>(parts[0]);
		utc.wMonth = static_cast<WORD>(parts[1]);
		utc.wDay = static_cast<WORD>(parts[2]);
		utc.wHour = static_cast<WORD>(parts[3]);
		utc.wMinute = static_cast<WORD>(parts[4]);
		utc.wSecond = static_cast<WORD>(parts[5]);

		SYSTEMTIME localTime = {};
		if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &localTime))
			localTime = utc;

		wchar_t dateBuffer[128] = {};
		wchar_t timeBuffer[128] = {};
		GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &localTime, nullptr,
			dateBuffer, _countof(dateBuffer));
		GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &localTime, nullptr,
			timeBuffer, _countof(timeBuffer));

		std::wstring result = dateBuffer;
		if (timeBuffer[0] != 0)
		{
			if (!result.empty())
				result += L' ';
			result += timeBuffer;
		}
		return result;
	}

	/**
	 * @brief Parse the <blame> document produced by svn blame --xml.
	 */
	void ParseBlameXml(const std::string& utf8, CBlameResult& result)
	{
		const std::wstring xml = FromBytes(utf8);
		if (xml.find(L"<blame") == std::wstring::npos)
		{
			result.ok = false;
			result.errorText = L"Unexpected output from svn blame";
			return;
		}

		size_t pos = 0;

		// svn reports the blamed path in a <target path="..."> element. Keep it
		// so the caller can verify that the URL really belongs to the file the
		// user is looking at.
		if (const size_t targetPos = xml.find(L"<target"); targetPos != std::wstring::npos)
		{
			const size_t targetEnd = xml.find(L'>', targetPos);
			if (targetEnd != std::wstring::npos)
				result.targetPath = ReadAttribute(
					xml.substr(targetPos, targetEnd - targetPos + 1), L"path");
		}

		while ((pos = xml.find(L"<entry", pos)) != std::wstring::npos)
		{
			const size_t tagEnd = xml.find(L'>', pos);
			if (tagEnd == std::wstring::npos)
				break;

			const std::wstring startTag = xml.substr(pos, tagEnd - pos + 1);
			const int lineNumber = ReadIntAttribute(startTag, L"line-number", 0);
			const size_t bodyEnd = xml.find(L"</entry>", tagEnd);
			if (bodyEnd == std::wstring::npos)
				break;

			const std::wstring body = xml.substr(tagEnd, bodyEnd - tagEnd);
			CBlameLine line;

			const size_t commitPos = body.find(L"<commit");
			if (commitPos != std::wstring::npos)
			{
				const size_t commitTagEnd = body.find(L'>', commitPos);
				if (commitTagEnd != std::wstring::npos)
				{
					const std::wstring commitTag = body.substr(commitPos, commitTagEnd - commitPos + 1);
					line.revision = ReadIntAttribute(commitTag, L"revision", -1);
					line.author = ReadElementText(body, L"author");
					line.when = FormatIsoTimestamp(ReadElementText(body, L"date"));
				}
			}
			// No <commit> element means the line has local, uncommitted changes.

			if (lineNumber > 0)
			{
				if (result.lines.size() < static_cast<size_t>(lineNumber))
					result.lines.resize(static_cast<size_t>(lineNumber));
				result.lines[static_cast<size_t>(lineNumber) - 1] = line;
			}

			pos = bodyEnd + 8;
		}

		result.ok = true;
	}

	/** @brief Build the svn blame command line for the given options. */
	std::wstring BuildBlameCommand(const std::wstring& svnExe, const std::wstring& url,
		const CBlameOptions& options)
	{
		std::wstring command = QuoteArg(svnExe) + L" blame --xml";

		std::wstring diffOptions;
		if (options.ignoreEol)
			diffOptions += L"--ignore-eol-style ";
		if (options.whitespaceMode == 1)
			diffOptions += L"-b ";
		else if (options.whitespaceMode == 2)
			diffOptions += L"-w ";

		if (!diffOptions.empty())
		{
			diffOptions.erase(diffOptions.size() - 1);
			command += L" -x " + QuoteArg(diffOptions);
		}

		if (options.includeMergeInfo)
			command += L" -g";

		const std::wstring from = (options.fromRevision > 0)
			? std::to_wstring(options.fromRevision) : std::wstring(L"1");
		const std::wstring to = (options.toRevision > 0)
			? std::to_wstring(options.toRevision) : std::wstring(L"HEAD");
		command += L" -r " + from + L":" + to;

		// Pin the URL to the requested revision. Without a peg revision svn
		// resolves the path at HEAD, which fails outright when the file was
		// deleted or moved after the revision the user is looking at.
		std::wstring target = url;
		if (options.toRevision > 0 && target.find(L'@') == std::wstring::npos)
			target += L"@" + to;

		command += L" " + QuoteArg(target);
		return command;
	}
}

CBlameJob::CBlameJob()
{
	m_hCancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

CBlameJob::~CBlameJob()
{
	if (m_hThread != nullptr)
	{
		RequestCancel();
		WaitForSingleObject(m_hThread, 10000);
		CloseHandle(m_hThread);
		m_hThread = nullptr;
	}
	if (m_hCancel != nullptr)
	{
		CloseHandle(m_hCancel);
		m_hCancel = nullptr;
	}
}

bool CBlameJob::Start(const std::wstring& path, const std::wstring& url,
	const CBlameOptions& options, int totalLines)
{
	if (m_hCancel == nullptr)
		m_hCancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (m_hCancel == nullptr)
		return false;

	m_path = path;
	m_url = url;
	m_options = options;
	m_totalLines.store(totalLines);
	m_parsedLines.store(0);
	m_cancelled.store(false);
	m_finished.store(false);
	m_running.store(true);

	unsigned threadId = 0;
	m_hThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &CBlameJob::ThreadProc, this, 0, &threadId));
	if (m_hThread == nullptr)
	{
		m_running.store(false);
		m_finished.store(true);
		return false;
	}
	return true;
}

unsigned __stdcall CBlameJob::ThreadProc(void* param)
{
	static_cast<CBlameJob*>(param)->Run();
	return 0;
}

void CBlameJob::Run()
{
	std::shared_ptr<CBlameResult> result = std::make_shared<CBlameResult>();

	const std::wstring svnExe = FindSvnExecutable();
	if (svnExe.empty())
	{
		result->errorText = L"svn.exe could not be found. Install TortoiseSVN or add Subversion to PATH.";
		result->ok = false;
	}
	else
	{
		std::wstring url = m_url;
		std::wstring error;

		// %burl is a file:// URL whenever the file lives in a working copy, so
		// it cannot be blamed directly. It is still useful: the working copy
		// it points into yields the repository URL of the same file.
		if (IsWorkingCopyFileUrl(url))
		{
			const std::wstring local = LocalPathFromFileUrl(url);
			std::wstring derived;
			if (!local.empty() && ResolveFileUrl(local, derived, error))
				url = derived;
			else
				url.clear();
		}
		if (url.empty() && !IsInWorkingCopy(m_path))
			url = FindRepositoryUrl(m_path);
		if (!url.empty() || ResolveFileUrl(m_path, url, error))
		{
			const std::wstring command = BuildBlameCommand(svnExe, url, m_options);

			ChildProcess child;
			if (!Spawn(command, std::wstring(), child, error))
			{
				result->errorText = error;
				result->ok = false;
			}
			else
			{
				std::string out, err;

				// Count "<entry" tokens while the data streams in so the progress
				// bar advances during the download. A five byte carry keeps tokens
				// that straddle two reads from being missed.
				int entryCount = 0;
				std::string carry;
				auto onRead = [&](const char* data, size_t length)
				{
					std::string window = carry;
					window.append(data, length);
					size_t scan = 0;
					while ((scan = window.find("<entry", scan)) != std::string::npos)
					{
						++entryCount;
						scan += 6;
					}
					carry = (window.size() >= 5) ? window.substr(window.size() - 5) : window;
					m_parsedLines.store(entryCount);
				};

				bool stopped = false;
				const bool completed = PumpPipes(child, out, err, onRead,
					[this]() { return WaitForSingleObject(m_hCancel, 0) == WAIT_OBJECT_0; },
					300000, stopped);
				const DWORD exitCode = GetChildExitCode(child);
				CloseChild(child);

				if (stopped || WaitForSingleObject(m_hCancel, 0) == WAIT_OBJECT_0)
				{
					m_cancelled.store(true);
					result->ok = false;
					result->errorText = L"Cancelled";
				}
				else if (!completed)
				{
					result->ok = false;
					result->errorText = L"svn blame timed out";
				}
				else if (exitCode != 0)
				{
					result->ok = false;
					result->errorText = TrimWhitespace(FromBytes(err));
					if (result->errorText.empty())
						result->errorText = L"svn blame failed";
				}
				else
				{
					ParseBlameXml(out, *result);

					// Record what svn actually blamed. TortoiseSVN writes the
					// BASE copy to the temp folder under a generated name, so
					// the blamed name legitimately differs from the local one
					// and a mismatch must not be treated as an error here; the
					// URL is chosen from a live source (see ToggleBlame) and
					// the stored default is only reused when its name matches.
					BlameTrace(L"Run: blamed target=%s local=%s",
						result->targetPath.c_str(), m_path.c_str());

					m_parsedLines.store(static_cast<int>(result->lines.size()));
				}
			}
		}
		else
		{
			result->errorText = error;
			result->ok = false;
		}
	}

	m_result = result;
	m_running.store(false);
	m_finished.store(true);
}

bool CBlameJob::IsRunning() const
{
	return m_running.load();
}

bool CBlameJob::IsFinished() const
{
	return m_finished.load();
}

int CBlameJob::GetParsedLines() const
{
	return m_parsedLines.load();
}

int CBlameJob::GetTotalLines() const
{
	return m_totalLines.load();
}

void CBlameJob::RequestCancel()
{
	if (m_hCancel != nullptr)
		SetEvent(m_hCancel);
}

bool CBlameJob::WasCancelled() const
{
	return m_cancelled.load();
}

std::shared_ptr<CBlameResult> CBlameJob::TakeResult()
{
	std::shared_ptr<CBlameResult> result = m_result;
	m_result.reset();
	return result;
}

std::wstring CBlameJob::FindSvnExecutable()
{
	wchar_t buffer[MAX_PATH * 2] = {};
	if (SearchPathW(nullptr, SVN_EXE_NAME, nullptr, _countof(buffer), buffer, nullptr) != 0)
		return std::wstring(buffer);

	for (const wchar_t* pattern : SVN_FALLBACK_PATHS)
	{
		const std::wstring candidate = ExpandEnvironment(pattern);
		if (candidate.empty())
			continue;
		const DWORD attributes = GetFileAttributesW(candidate.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
			return candidate;
	}
	return std::wstring();
}

bool CBlameJob::ResolveFileUrl(const std::wstring& path, std::wstring& url, std::wstring& error)
{
	const std::wstring svnExe = FindSvnExecutable();
	if (svnExe.empty())
	{
		error = L"svn.exe could not be found";
		return false;
	}

	const std::wstring wcRoot = FindWorkingCopyRoot(path);
	if (wcRoot.empty())
	{
		error = L"This file is not inside a Subversion working copy";
		return false;
	}

	// Ask the working copy for the URL of its root directory. Using the URL of
	// the working copy root (and not the repository root) is what makes the
	// relative path resolve: a checkout of .../trunk or of a branch maps its own
	// directory to the repository sub directory, not to the repository root.
	// The call runs with the root as working directory and "." as the target, so
	// no local path is ever handed to svn: that keeps non-ANSI working copy paths
	// working, which plain "svn info <path>" does not.
	std::wstring rootUrl;
	std::wstring infoError;
	const std::wstring infoCommand = QuoteArg(svnExe) + L" info --show-item url .";
	if (!RunCapture(infoCommand, wcRoot, rootUrl, infoError))
	{
		// Older Subversion clients do not know --show-item, so fall back to XML.
		const std::wstring xmlCommand = QuoteArg(svnExe) + L" info --xml .";
		std::wstring xmlOutput;
		if (!RunCapture(xmlCommand, wcRoot, xmlOutput, infoError))
		{
			error = infoError;
			return false;
		}
		const std::wstring root = ReadElementText(xmlOutput, L"url");
		if (root.empty())
		{
			error = L"Could not determine the repository URL";
			return false;
		}
		rootUrl = root;
	}

	std::wstring relative;
	if (path.size() > wcRoot.size())
	{
		relative = path.substr(wcRoot.size());
		while (!relative.empty() && (relative[0] == L'\\' || relative[0] == L'/'))
			relative.erase(0, 1);
	}
	if (relative.empty())
	{
		error = L"Could not compute the path inside the working copy";
		return false;
	}

	url = rootUrl;
	if (!url.empty() && url.back() != L'/')
		url += L'/';
	url += EncodeRelativePath(relative);
	return true;
}

void BlameTrace(const wchar_t* format, ...)
{
	wchar_t buffer[1024] = {};
	va_list args;
	va_start(args, format);
	_vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
	va_end(args);

	wchar_t path[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, path);
	wcscat_s(path, L"winmerge-blame-trace.log");

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"a, ccs=UTF-8") == 0 && file != nullptr)
	{
		fwprintf(file, L"%s\n", buffer);
		fclose(file);
	}
}

bool CBlameJob::IsInWorkingCopy(const std::wstring& path)
{
	return !FindWorkingCopyRoot(path).empty();
}

/**
 * @brief Convert a "file://" URL back to a local path.
 * @return The path, or an empty string when the URL is not a file URL.
 */
std::wstring CBlameJob::LocalPathFromFileUrl(const std::wstring& url)
{
	if (_wcsnicmp(url.c_str(), L"file://", 7) != 0)
		return std::wstring();

	std::wstring path = url.substr(7);

	// file:///J:/temp/x -> J:/temp/x ; file://server/share -> //server/share
	if (!path.empty() && path[0] == L'/')
		path.erase(0, 1);

	std::wstring decoded;
	decoded.reserve(path.size());
	for (size_t i = 0; i < path.size(); ++i)
	{
		if (path[i] == L'%' && i + 2 < path.size())
		{
			const std::wstring hex = path.substr(i + 1, 2);
			wchar_t* end = nullptr;
			const long value = wcstol(hex.c_str(), &end, 16);
			if (end != nullptr && *end == L'\0')
			{
				decoded.push_back(static_cast<wchar_t>(value));
				i += 2;
				continue;
			}
		}
		decoded.push_back((path[i] == L'/') ? L'\\' : path[i]);
	}
	return decoded;
}

/**
 * @brief Tells whether an URL is the file:// form of a file inside a working copy.
 *
 * TortoiseSVN expands %burl to a file:// URL for files that live in a working
 * copy. That URL names the comparison file itself, not a repository, so blaming
 * it fails. A real local repository URL never maps to an existing file on disk,
 * because the repository keeps its own internal layout.
 */
bool CBlameJob::IsWorkingCopyFileUrl(const std::wstring& url)
{
	const std::wstring path = LocalPathFromFileUrl(url);
	if (path.empty())
		return false;
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

namespace
{
	/** @brief Command line hints about where a left file came from. */
	struct RepositoryHint
	{
		std::wstring url;
		int revision = 0;
	};

	/** @brief Hints collected from the command line, keyed by lower case path. */
	std::map<std::wstring, RepositoryHint> g_repositoryHints;
	std::mutex g_repositoryHintsMutex;

	std::wstring LowerCase(std::wstring text)
	{
		std::transform(text.begin(), text.end(), text.begin(),
			[](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
		return text;
	}

	/**
	 * @brief Remove the " Revision <number>" suffix from an exported temp file name.
	 *
	 * The log dialog exports each side of a diff to the temp folder as
	 * "<file name> Revision <revision>", so a name read back from the pane looks
	 * like "DynamicBoneNodeArt.cs Revision 5316825" even though the repository
	 * holds "DynamicBoneNodeArt.cs". Only a suffix that ends in digits counts,
	 * so a file that merely carries the word in its name is left alone.
	 */
	std::wstring StripRevisionSuffix(const std::wstring& name)
	{
		static const std::wstring marker = L" revision ";
		const size_t pos = LowerCase(name).rfind(marker);
		if (pos == std::wstring::npos)
			return name;

		size_t firstDigit = pos + marker.length();
		size_t afterDigits = firstDigit;
		while (afterDigits < name.length() && iswdigit(name[afterDigits]) != 0)
			++afterDigits;
		if (afterDigits == firstDigit)
			return name;

		// An extension after the number is tolerated; anything else means the
		// marker was part of the name rather than an export suffix.
		if (afterDigits < name.length() && name[afterDigits] != L'.')
			return name;
		return name.substr(0, pos);
	}

	/** @brief Base name of a path or URL, without the export suffix or extension. */
	std::wstring BaseNameWithoutExtension(const std::wstring& value)
	{
		const size_t slash = value.find_last_of(L"\\/");
		std::wstring name = (slash == std::wstring::npos) ? value : value.substr(slash + 1);
		name = StripRevisionSuffix(name);
		const size_t dot = name.find_last_of(L'.');
		if (dot != std::wstring::npos && dot > 0)
			name.erase(dot);
		return name;
	}

	/**
	 * @brief Separator between the URLs kept in the history.
	 *
	 * A tab cannot appear in a URL, and the options storage escapes the
	 * characters it cannot round trip, so this stays readable on disk.
	 */
	constexpr TCHAR URL_HISTORY_SEPARATOR = _T('\t');

	/** @brief How many URLs the dropdown offers. */
	constexpr size_t URL_HISTORY_MAX_ENTRIES = 20;

	/** @brief Split the stored history string into URLs, most recent first. */
	std::vector<std::wstring> SplitUrlHistory(const String& value)
	{
		std::vector<std::wstring> urls;
		size_t start = 0;
		while (start < value.length())
		{
			size_t end = value.find(URL_HISTORY_SEPARATOR, start);
			if (end == String::npos)
				end = value.length();
			if (end > start)
				urls.push_back(value.substr(start, end - start));
			start = end + 1;
		}
		return urls;
	}
}

std::vector<std::wstring> CBlameJob::GetRepositoryUrlHistory()
{
	return SplitUrlHistory(GetOptionsMgr()->GetString(OPT_BLAME_URL_HISTORY));
}

void CBlameJob::AddRepositoryUrlToHistory(const std::wstring& url)
{
	if (url.empty())
		return;

	std::vector<std::wstring> urls = GetRepositoryUrlHistory();
	urls.erase(std::remove_if(urls.begin(), urls.end(),
		[&url](const std::wstring& entry) { return _wcsicmp(entry.c_str(), url.c_str()) == 0; }),
		urls.end());
	urls.insert(urls.begin(), url);
	if (urls.size() > URL_HISTORY_MAX_ENTRIES)
		urls.resize(URL_HISTORY_MAX_ENTRIES);

	String value;
	for (const std::wstring& entry : urls)
	{
		if (!value.empty())
			value += URL_HISTORY_SEPARATOR;
		value += entry;
	}
	GetOptionsMgr()->SaveOption(OPT_BLAME_URL_HISTORY, value);
}

void CBlameJob::RegisterRepositoryUrl(const std::wstring& path, const std::wstring& url)
{
	if (path.empty() || url.empty())
		return;
	std::lock_guard<std::mutex> lock(g_repositoryHintsMutex);
	g_repositoryHints[LowerCase(path)].url = url;
}

void CBlameJob::RegisterRepositoryRevision(const std::wstring& path, int revision)
{
	if (path.empty() || revision <= 0)
		return;
	std::lock_guard<std::mutex> lock(g_repositoryHintsMutex);
	g_repositoryHints[LowerCase(path)].revision = revision;
}

std::wstring CBlameJob::FindRepositoryUrl(const std::wstring& path)
{
	std::lock_guard<std::mutex> lock(g_repositoryHintsMutex);
	const auto it = g_repositoryHints.find(LowerCase(path));
	if (it == g_repositoryHints.end() || it->second.url.empty())
		return std::wstring();
	if (!UrlMatchesFile(it->second.url, path))
		return std::wstring();
	return it->second.url;
}

bool CBlameJob::UrlMatchesFile(const std::wstring& url, const std::wstring& path)
{
	if (url.empty() || path.empty())
		return false;

	// Extensions are ignored on purpose: the log dialog exports a revision to
	// the temp folder under a name that may lack the original extension
	// ("LobbyScene" for "LobbyScene.cs").
	return _wcsicmp(BaseNameWithoutExtension(url).c_str(),
		BaseNameWithoutExtension(path).c_str()) == 0;
}

int CBlameJob::QueryWorkingCopyRevision(const std::wstring& path)
{
	const std::wstring svnExe = FindSvnExecutable();
	if (svnExe.empty() || path.empty())
		return 0;

	// Run from the containing folder and pass only the file name, so a working
	// copy path with characters outside the ANSI code page still works.
	const size_t slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return 0;
	const std::wstring folder = path.substr(0, slash);
	const std::wstring name = path.substr(slash + 1);
	if (folder.empty() || name.empty())
		return 0;

	std::wstring output, error;
	const std::wstring command =
		QuoteArg(svnExe) + L" info --show-item revision " + QuoteArg(name);
	if (!RunCapture(command, folder, output, error))
		return 0;

	const int revision = _wtoi(output.c_str());
	return (revision > 0) ? revision : 0;
}

int CBlameJob::FindRepositoryRevision(const std::wstring& path)
{
	std::lock_guard<std::mutex> lock(g_repositoryHintsMutex);
	const auto it = g_repositoryHints.find(LowerCase(path));
	return (it == g_repositoryHints.end()) ? 0 : it->second.revision;
}

namespace BlameTint
{

const String& GetOptionName(int index)
{
	switch (index)
	{
	case 0: return OPT_BLAME_TINT_COLOR0;
	case 1: return OPT_BLAME_TINT_COLOR1;
	case 2: return OPT_BLAME_TINT_COLOR2;
	case 3: return OPT_BLAME_TINT_COLOR3;
	case 4: return OPT_BLAME_TINT_COLOR4;
	case 5: return OPT_BLAME_TINT_COLOR5;
	case 6: return OPT_BLAME_TINT_COLOR6;
	case 7: return OPT_BLAME_TINT_COLOR7;
	case 8: return OPT_BLAME_TINT_COLOR8;
	case 9: return OPT_BLAME_TINT_COLOR9;
	case 10: return OPT_BLAME_TINT_COLOR10;
	case 11: return OPT_BLAME_TINT_COLOR11;
	default: return OPT_BLAME_TINT_COLOR0;
	}
}

COLORREF GetDefaultColor(int index)
{
	// Near neutral accents, all centred on the same light grey so a blended
	// band lands around #E1E1E1 and the gutter keeps reading as grey with only
	// a temperature shift. The hue offsets are small by design, which is what
	// keeps a light scheme calm; a dark scheme would lose them entirely, so the
	// dark schemes override these values.
	static const COLORREF defaults[COUNT] =
	{
		RGB(172, 190, 208),  // blue
		RGB(176, 200, 182),  // green
		RGB(208, 198, 172),  // amber
		RGB(210, 176, 182),  // red
		RGB(192, 176, 208),  // violet
		RGB(170, 198, 200),  // teal
		RGB(194, 200, 172),  // olive
		RGB(206, 174, 202),  // magenta
		RGB(168, 174, 206),  // indigo
		RGB(174, 202, 190),  // mint
		RGB(192, 204, 168),  // lime
		RGB(212, 188, 168),  // peach
	};
	if (index < 0 || index >= COUNT)
		return defaults[0];
	return defaults[index];
}

COLORREF GetColor(int index)
{
	return static_cast<COLORREF>(GetOptionsMgr()->GetInt(GetOptionName(index)));
}

}
