# windows/extensions - unpacked browser extensions for the WebView2 shell

The Windows shell loads **unpacked** browser extensions from this folder into its
WebView2 profile. Ad blocking is implemented this way: the shell itself has no
rule engine for the Windows side, so an extension (a content blocker) does the
filtering inside Chromium.

Nothing is bundled here on purpose - no extension code is vendored or committed,
and the repository ships no extension. You install the one you trust.

## Where to put an extension

Create one folder per extension, with the extension's `manifest.json` directly
inside it:

```
windows/
  extensions/
    some-adblocker/
      manifest.json     <- required; folders without it are skipped
      ...               <- the rest of the unpacked extension
```

- Use the **unpacked** form (a folder), not a `.crx` file, not a zip, and not a
  copy of another browser profile's cached `Extensions/...` directory.
- Manifest V3 extensions are expected (WebView2 uses the Chromium extension
  engine of the installed Edge WebView2 runtime).
- Everything in this folder except this README is ignored by git
  (`windows/extensions/*` in `.gitignore`), so third-party code cannot be
  committed by accident.

## How loading works

- On start the shell enables the WebView2 browser-extension feature and, once the
  first tab exists, calls `ICoreWebView2Profile7::AddBrowserExtension()` for every
  sub-folder here (the profile is shared, so this happens once per run).
- Each attempt is logged in `windows\zephium_win.log`:

  ```
  extension add requested: some-adblocker (0x00000000)
  extension loaded: Some Adblocker (id=<extension id>) from some-adblocker
  ```

  A failure only costs ad blocking and never stops browsing:

  ```
  extension failed: some-adblocker (0x8000....)
  extensions: ICoreWebView2Profile7 missing (0x80004002), ad blocking not loaded
  ```

- Extensions live in the WebView2 profile directory
  (`windows\zephium_webview2_profile`), which is separate from your Edge/Chrome
  profile.

## Turning it off

```powershell
.\zephium_win.exe --no-adblock https://example.com   # this run only
$env:ZEPHIUM_ADBLOCK = '0'; .\zephium_win.exe        # from this shell session
```

With either switch the shell logs `ad blocking: disabled` and does not touch this
folder at all.

## Before you install one

- Extensions run with the permissions they declare on **every** page the shell
  opens (that is what makes a content blocker work). Pick one from a source you
  trust and check its permissions.
- Third-party extension code is not covered by this repository's license; keep it
  out of git.
- WebView2's extension support requires a recent **Microsoft Edge WebView2
  Runtime** (the extension APIs appeared in the WebView2 SDK / runtime 1.0.2210.55
  and later).
