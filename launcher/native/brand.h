/* NX2navi Co-op's identity in the launcher's own program (nx2navi.c) and the setup (setup.c): its folder, programs,
   Apps entry, shortcuts and taskbar identity differ from the official NX2navi's, so the two install side by side.
   launcher/brand.py holds the same names for the launcher. */
#pragma once

#define APP_NAME L"NX2navi Co-op"              /* windows, messages, the Apps entry and the shortcuts' names */
#define APP_FOLDER L"NX2navi-Coop"             /* %LOCALAPPDATA%\Programs\<APP_FOLDER> (brand.py FOLDER) */
#define STUB_EXE L"NX2navi-Coop.exe"           /* the launcher's own program in that folder (brand.py STUB) */
#define SETUP_EXE L"NX2navi-Coop-Setup.exe"    /* the setup, as messages name it (brand.py SETUP) */
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NX2navi-Coop"
/* The launcher window's taskbar identity (launcher/winsys.py app_id, brand.py APP_ID): a shortcut that carries it is
   the one the taskbar groups and pins the window under, with the shortcut's icon. */
#define APP_USER_MODEL_ID L"NX2navi.Coop.Launcher"
