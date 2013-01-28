// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library pub_tests;

import 'dart:io';

import '../../test_pub.dart';

main() {
  integration("removes a dependency that's been removed from the pubspec", () {
    servePackages([
      package("foo", "1.0.0"),
      package("bar", "1.0.0")
    ]);

    appDir([dependency("foo"), dependency("bar")]).scheduleCreate();

    schedulePub(args: ['install'],
        output: new RegExp(r"Dependencies installed!$"));

    packagesDir({
      "foo": "1.0.0",
      "bar": "1.0.0"
    }).scheduleValidate();

    appDir([dependency("foo")]).scheduleCreate();

    schedulePub(args: ['install'],
        output: new RegExp(r"Dependencies installed!$"));

    packagesDir({
      "foo": "1.0.0",
      "bar": null
    }).scheduleValidate();
  });
}
