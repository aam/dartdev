// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library pub_tests;

import 'dart:io';

import '../../../../pkg/unittest/lib/unittest.dart';
import '../test_pub.dart';

main() {
  group('requires', () {
    integration('a pubspec', () {
      dir(appPath, []).scheduleCreate();

      schedulePub(args: ['install'],
          error: new RegExp(r'^Could not find a file named "pubspec\.yaml"'),
          exitCode: 1);
    });

    integration('a pubspec with a "name" key', () {
      dir(appPath, [
        pubspec({"dependencies": {"foo": null}})
      ]).scheduleCreate();

      schedulePub(args: ['install'],
          error: new RegExp(r'^pubspec.yaml is missing the required "name" '
              r'field \(e\.g\. "name: myapp"\)\.'),
          exitCode: 1);
    });
  });

  integration('adds itself to the packages', () {
    // The symlink should use the name in the pubspec, not the name of the
    // directory.
    dir(appPath, [
      pubspec({"name": "myapp_name"}),
      libDir('foo'),
    ]).scheduleCreate();

    schedulePub(args: ['install'],
        output: new RegExp(r"Dependencies installed!$"));

    dir(packagesPath, [
      dir("myapp_name", [
        file('foo.dart', 'main() => "foo";')
      ])
    ]).scheduleValidate();
  });

  integration('does not adds itself to the packages if it has no "lib" directory', () {
    // The symlink should use the name in the pubspec, not the name of the
    // directory.
    dir(appPath, [
      pubspec({"name": "myapp_name"}),
    ]).scheduleCreate();

    schedulePub(args: ['install'],
        output: new RegExp(r"Dependencies installed!$"));

    dir(packagesPath, [
      nothing("myapp_name")
    ]).scheduleValidate();
  });

  integration('does not add a package if it does not have a "lib" directory', () {
    // Using an SDK source, but this should be true of all sources.
    dir(sdkPath, [
      file('version', '0.1.2.3'),
      dir('pkg', [
        dir('foo', [
          libPubspec('foo', '0.0.0-not.used')
        ])
      ])
    ]).scheduleCreate();

    dir(appPath, [
      pubspec({"name": "myapp", "dependencies": {"foo": {"sdk": "foo"}}})
    ]).scheduleCreate();

    schedulePub(args: ['install'],
        error: new RegExp(r'Warning: Package "foo" does not have a "lib" '
            'directory so you will not be able to import any libraries from '
            'it.'),
        output: new RegExp(r"Dependencies installed!$"));
  });

  integration('does not warn if the root package lacks a "lib" directory', () {
    dir(appPath, [
      appPubspec([])
    ]).scheduleCreate();

    schedulePub(args: ['install'],
        error: '',
        output: new RegExp(r"Dependencies installed!$"));
  });

  integration('overwrites the existing packages directory', () {
    dir(appPath, [
      appPubspec([]),
      dir('packages', [
        dir('foo'),
        dir('myapp'),
      ]),
      libDir('myapp')
    ]).scheduleCreate();

    schedulePub(args: ['install'],
        output: new RegExp(r"Dependencies installed!$"));

    dir(packagesPath, [
      nothing('foo'),
      dir('myapp', [file('myapp.dart', 'main() => "myapp";')])
    ]).scheduleValidate();
  });

  group('creates a packages directory in', () {
    integration('"test/" and its subdirectories', () {
      dir(appPath, [
        appPubspec([]),
        libDir('foo'),
        dir("test", [dir("subtest")])
      ]).scheduleCreate();

      schedulePub(args: ['install'],
          output: new RegExp(r"Dependencies installed!$"));

      dir(appPath, [
        dir("test", [
          dir("packages", [
            dir("myapp", [
              file('foo.dart', 'main() => "foo";')
            ])
          ]),
          dir("subtest", [
            dir("packages", [
              dir("myapp", [
                file('foo.dart', 'main() => "foo";')
              ])
            ])
          ])
        ])
      ]).scheduleValidate();
    });

    integration('"example/" and its subdirectories', () {
      dir(appPath, [
        appPubspec([]),
        libDir('foo'),
        dir("example", [dir("subexample")])
      ]).scheduleCreate();

      schedulePub(args: ['install'],
          output: new RegExp(r"Dependencies installed!$"));

      dir(appPath, [
        dir("example", [
          dir("packages", [
            dir("myapp", [
              file('foo.dart', 'main() => "foo";')
            ])
          ]),
          dir("subexample", [
            dir("packages", [
              dir("myapp", [
                file('foo.dart', 'main() => "foo";')
              ])
            ])
          ])
        ])
      ]).scheduleValidate();
    });

    integration('"tool/" and its subdirectories', () {
      dir(appPath, [
        appPubspec([]),
        libDir('foo'),
        dir("tool", [dir("subtool")])
      ]).scheduleCreate();

      schedulePub(args: ['install'],
          output: new RegExp(r"Dependencies installed!$"));

      dir(appPath, [
        dir("tool", [
          dir("packages", [
            dir("myapp", [
              file('foo.dart', 'main() => "foo";')
            ])
          ]),
          dir("subtool", [
            dir("packages", [
              dir("myapp", [
                file('foo.dart', 'main() => "foo";')
              ])
            ])
          ])
        ])
      ]).scheduleValidate();
    });

    integration('"web/" and its subdirectories', () {
      dir(appPath, [
        appPubspec([]),
        libDir('foo'),
        dir("web", [dir("subweb")])
      ]).scheduleCreate();

      schedulePub(args: ['install'],
          output: new RegExp(r"Dependencies installed!$"));

      dir(appPath, [
        dir("web", [
          dir("packages", [
            dir("myapp", [
              file('foo.dart', 'main() => "foo";')
            ])
          ]),
          dir("subweb", [
            dir("packages", [
              dir("myapp", [
                file('foo.dart', 'main() => "foo";')
              ])
            ])
          ])
        ])
      ]).scheduleValidate();
    });

    integration('"bin/"', () {
      dir(appPath, [
        appPubspec([]),
        libDir('foo'),
        dir("bin")
      ]).scheduleCreate();

      schedulePub(args: ['install'],
          output: new RegExp(r"Dependencies installed!$"));

      dir(appPath, [
        dir("bin", [
          dir("packages", [
            dir("myapp", [
              file('foo.dart', 'main() => "foo";')
            ])
          ])
        ])
      ]).scheduleValidate();
    });
  });
}
