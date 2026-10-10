# SOUI newDemo

The file region supports multi-select video importing, case-insensitive duplicate filtering, Media Foundation metadata probing (size, duration, dimensions and subtype), persistent row selection, per-row deletion, clear, and a live count/size summary. Metadata is probed on a worker thread and delivered back to the UI thread; unsupported or unreadable media is reported in the row instead of receiving invented values. No conversion is performed.

Build Debug Win32 with the repository's v141 configuration. Media Foundation is provided by Windows and is linked through the project file's native system libraries.
