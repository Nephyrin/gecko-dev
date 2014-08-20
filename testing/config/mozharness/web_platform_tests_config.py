# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from platform import system, architecture

if system() == "Linux":
    if architecture()[0] == '64bit':
        MINIDUMP_STACKWALK_PATH = "%(abs_work_dir)s/tools/breakpad/linux64/minidump_stackwalk"
    else:
        MINIDUMP_STACKWALK_PATH = "%(abs_work_dir)s/tools/breakpad/linux/minidump_stackwalk"
elif system() == "Windows":
    MINIDUMP_STACKWALK_PATH = "%(abs_work_dir)s/tools/breakpad/win32/minidump_stackwalk.exe"
elif system() == "Darwin":
    MINIDUMP_STACKWALK_PATH = "%(abs_work_dir)s/tools/breakpad/osx64/minidump_stackwalk"
else:
    MINIDUMP_STACKWALK_PATH = None

config = {
    "options": [
        "--prefs-root=%(test_path)s/prefs",
        "--processes=1",
        "--config=%(test_path)s/wptrunner.ini",
    ],
}

if MINIDUMP_STACKWALK_PATH is not None:
    config["options"].append("--stackwalk-binary=%s" % MINIDUMP_STACKWALK_PATH)
