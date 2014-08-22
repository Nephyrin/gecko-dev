# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import print_function

import os
import sys

from mozpack.files import FileFinder
from mozpack.copier import Jarrer
from mozbuild.base import MozbuildObject

def package_gcno_tree():
    build_obj = MozbuildObject.from_environment()
    # XXX JarWriter doesn't support unicode strings, see bug 1056859
    finder = FileFinder(build_obj.topobjdir.encode('utf-8'))
    jarrer = Jarrer(optimize=False)

    for p, f in finder.find("**/*.gcno"):
        jarrer.add(p, f)
    name = os.path.join(build_obj.distdir, '%s-code-coverage-gcno.zip'
                        % build_obj.pkg_basename)
    jarrer.copy(name)


if __name__ == '__main__':
    sys.exit(package_gcno_tree())
