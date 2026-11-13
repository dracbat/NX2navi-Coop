"""NX2navi Co-op's identity: the names that keep its install apart from the official NX2navi, so that the two install,
update and run side by side, each with its own folder, programs, Apps entry, shortcuts, taskbar button, Steam
shortcut and releases. launcher/native/brand.h holds the same names for the setup and the launcher's own program,
runtime/brand.h the game window's title, and tools/make_launcher.py names the release's files after these."""

NAME = 'NX2navi Co-op'                 # the launcher's window and messages, the Apps entry, the shortcuts
FOLDER = 'NX2navi-Coop'                # %LOCALAPPDATA%\Programs\<FOLDER>, C:\<FOLDER>\toolchain, <drive>\Games\<FOLDER>
STUB = 'NX2navi-Coop.exe'              # the launcher's own program in its folder (launcher/native/nx2navi.c)
SETUP = 'NX2navi-Coop-Setup'           # the setup players download: <SETUP>-<version>.exe (launcher/native/setup.c)
APP_ID = 'NX2navi.Coop.Launcher'       # the window's and its shortcuts' taskbar identity (brand.h APP_USER_MODEL_ID)
UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\NX2navi-Coop'   # brand.h UNINSTALL_KEY
FEED_REPO = 'dracbat/NX2navi-Coop'     # the GitHub repository whose releases update it (launcher/feed.py)
ASSET_PREFIX = 'NX2navi-Coop'          # a release's files: <prefix>-app-<version>.zip, <prefix>-runtime-<id>.zip
MANIFEST = f'{ASSET_PREFIX}-update.json'   # the manifest a release carries; the official one's name differs, so
#                                            neither launcher ever takes the other's releases
STEAM_NAME = 'Ocarina of Time Co-op'   # the Steam shortcut's name (launcher/steam.py): the official one keeps its own
STEAM_BACKUP = 'shortcuts.vdf.nx2navi-coop-backup'   # the copy of Steam's shortcuts file made before the first change
