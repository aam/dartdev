// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library pub_tests;

import 'dart:io';

import '../../test_pub.dart';

main() {
  integration('resolves version constraints from a pub server', () {
    servePackages([
      package("foo", "1.2.3", [dependency("baz", ">=2.0.0")]),
      package("bar", "2.3.4", [dependency("baz", "<3.0.0")]),
      package("baz", "2.0.3"),
      package("baz", "2.0.4"),
      package("baz", "3.0.1")
    ]);

    appDir([dependency("foo"), dependency("bar")]).scheduleCreate();

    schedulePub(args: ['install'],
        output: new RegExp("Dependencies installed!\$"));

    cacheDir({
      "foo": "1.2.3",
      "bar": "2.3.4",
      "baz": "2.0.4"
    }).scheduleValidate();

    packagesDir({
      "foo": "1.2.3",
      "bar": "2.3.4",
      "baz": "2.0.4"
    }).scheduleValidate();
  });
}
