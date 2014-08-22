#!/usr/bin/env python

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os
import sys

here = os.path.abspath(os.path.dirname(__file__))
sys.path.insert(0, os.path.join(here, "harness"))

marionette_deps = [('client', 'marionette'),
                   ('transport', 'marionette_transport')]

mozbase_deps = ['manifestparser',
                'mozcrash',
                'mozdevice',
                'mozfile',
                'mozhttpd',
                'mozinfo',
                'mozlog',
                'moznetwork',
                'mozprocess',
                'mozprofile',
                'mozrunner',
                'moztest',
                'mozversion']

deps = ([(os.path.join('marionette', item[0]), item[1]) for item in marionette_deps] +
        [(os.path.join('mozbase', item), item) for item in mozbase_deps])

testing = os.path.realpath(os.path.join(here, '..'))

for rel_path, module in deps:
    path = os.path.join(testing, rel_path)
    if (os.path.exists(path) and
        path not in sys.path and
        module not in sys.modules):
        try:
            __import__(module)
        except ImportError:
            sys.path.insert(0, path)

from wptrunner import wptrunner

if __name__ == "__main__":
    success = wptrunner.main()
    if not success:
        sys.exit(1)
