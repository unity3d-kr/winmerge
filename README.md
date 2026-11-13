# WinMerge &mdash; SVN blame fork

A fork of [WinMerge](https://winmerge.org/) that adds an **SVN blame gutter** to
the file compare view, so a diff can be read together with who last changed
each line, when, and in which revision.

## Upstream

WinMerge is an open source differencing and merging tool for Windows, developed
since 2000. This repository is a fork of
[unity3d-kr/winmerge](https://github.com/unity3d-kr/winmerge), which tracks
[WinMerge/winmerge](https://github.com/WinMerge/winmerge).

Everything not described below is unmodified upstream code, and the upstream
documentation still applies.

- Project site: <https://winmerge.org/>
- Upstream source: <https://github.com/WinMerge/winmerge>
- License: GPL-2.0, see [LICENSE.md](LICENSE.md)

## What this fork adds

### Blame gutter

Open a file compare and choose **File > SVN Blame...** (also on the toolbar).
The left pane gains a gutter showing the revision, author and date of every
line, retrieved with `svn blame --xml` on a worker thread so the view stays
responsive.

- **Repository URL** is derived from the working copy when the file is inside
  one. Files opened from TortoiseSVN's log dialog live in `%TEMP%` and have no
  working copy, so the URL TortoiseSVN passes with `%burl` is used, and a
  dropdown next to the field keeps the URLs used before for manual selection.
- **Peg revision** uses `%brev` or the working copy revision, so blaming a diff
  opened at an older revision stops there instead of reporting HEAD.
- **Revision range** can be limited from a given revision to HEAD or to a
  specific revision.
- **Diff options** cover `--ignore-eol-style`, the three whitespace modes and
  `-g` for merge info.
- **Columns** (revision / author / time) are toggled from the View menu without
  downloading the blame data again; a tooltip reveals the columns a narrow pane
  hides.
- **Cancellation** stops the running `svn` process immediately.

### Per-author tint

The gutter background is tinted per author so contributions can be told apart
at a glance.

- Twelve accent colours are handed out in line-count order, so the busiest
  contributors get the most distinct tones; the author with the most lines
  keeps the plain margin colour.
- Each accent is blended 45% into the current margin colour, so the tint
  follows whatever colour scheme is active instead of being a fixed colour.
- The twelve colours are options (`Settings/BlameTintColor0..11`) and can be
  edited in **Settings > Colors > Text Colors** under *Blame tint*.
- Shipped schemes carry their own values: neutral accents that land around
  `#E1E1E1` for light schemes, low chroma greys for dark ones.

## Build

Identical to upstream:

```bash
git clone --recurse-submodules https://github.com/unity3d-kr/winmerge
cd winmerge
DownloadDeps.cmd
BuildAll.cmd [x86|x64|ARM64]
```

Requires Visual Studio 2022 with the MFC/ATL components and a Windows 10 or
later SDK. The executable is written to `Build\<arch>\Release\WinMergeU.exe`.
