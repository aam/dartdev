// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Test infrastructure for testing pub. Unlike typical unit tests, most pub
/// tests are integration tests that stage some stuff on the file system, run
/// pub, and then validate the results. This library provides an API to build
/// tests like that.
library test_pub;

import 'dart:async';
import 'dart:collection' show Queue;
import 'dart:io';
import 'dart:json' as json;
import 'dart:math';
import 'dart:uri';

import '../../../pkg/http/lib/testing.dart';
import '../../../pkg/oauth2/lib/oauth2.dart' as oauth2;
import '../../../pkg/path/lib/path.dart' as path;
import '../../../pkg/unittest/lib/unittest.dart';
import '../../../pkg/yaml/lib/yaml.dart';
import '../../lib/file_system.dart' as fs;
import '../../pub/entrypoint.dart';
// TODO(rnystrom): Using "gitlib" as the prefix here is ugly, but "git" collides
// with the git descriptor method. Maybe we should try to clean up the top level
// scope a bit?
import '../../pub/git.dart' as gitlib;
import '../../pub/git_source.dart';
import '../../pub/hosted_source.dart';
import '../../pub/http.dart';
import '../../pub/io.dart';
import '../../pub/sdk_source.dart';
import '../../pub/system_cache.dart';
import '../../pub/utils.dart';
import '../../pub/validator.dart';
import 'command_line_config.dart';

/// This should be called at the top of a test file to set up an appropriate
/// test configuration for the machine running the tests.
initConfig() {
  // If we aren't running on the bots, use the human-friendly config.
  if (new Options().arguments.contains('--human')) {
    configure(new CommandLineConfiguration());
  }
}

/// Creates a new [FileDescriptor] with [name] and [contents].
FileDescriptor file(Pattern name, String contents) =>
    new FileDescriptor(name, contents);

/// Creates a new [DirectoryDescriptor] with [name] and [contents].
DirectoryDescriptor dir(Pattern name, [List<Descriptor> contents]) =>
    new DirectoryDescriptor(name, contents);

/// Creates a new [FutureDescriptor] wrapping [future].
FutureDescriptor async(Future<Descriptor> future) =>
    new FutureDescriptor(future);

/// Creates a new [GitRepoDescriptor] with [name] and [contents].
GitRepoDescriptor git(Pattern name, [List<Descriptor> contents]) =>
    new GitRepoDescriptor(name, contents);

/// Creates a new [TarFileDescriptor] with [name] and [contents].
TarFileDescriptor tar(Pattern name, [List<Descriptor> contents]) =>
    new TarFileDescriptor(name, contents);

/// Creates a new [NothingDescriptor] with [name].
NothingDescriptor nothing(String name) => new NothingDescriptor(name);

/// The current [HttpServer] created using [serve].
var _server;

/// The cached value for [_portCompleter].
Completer<int> _portCompleterCache;

/// The completer for [port].
Completer<int> get _portCompleter {
  if (_portCompleterCache != null) return _portCompleterCache;
  _portCompleterCache = new Completer<int>();
  _scheduleCleanup((_) {
    _portCompleterCache = null;
  });
  return _portCompleterCache;
}

/// A future that will complete to the port used for the current server.
Future<int> get port => _portCompleter.future;

/// Creates an HTTP server to serve [contents] as static files. This server will
/// exist only for the duration of the pub run.
///
/// Subsequent calls to [serve] will replace the previous server.
void serve([List<Descriptor> contents]) {
  var baseDir = dir("serve-dir", contents);

  _schedule((_) {
    return _closeServer().then((_) {
      _server = new HttpServer();
      _server.defaultRequestHandler = (request, response) {
        var path = request.uri.replaceFirst("/", "").split("/");
        response.persistentConnection = false;
        var stream;
        try {
          stream = baseDir.load(path);
        } catch (e) {
          response.statusCode = 404;
          response.contentLength = 0;
          response.outputStream.close();
          return;
        }

        var future = consumeInputStream(stream);
        future.then((data) {
          response.statusCode = 200;
          response.contentLength = data.length;
          response.outputStream.write(data);
          response.outputStream.close();
        }).catchError((e) {
          print("Exception while handling ${request.uri}: $e");
          response.statusCode = 500;
          response.reasonPhrase = e.message;
          response.outputStream.close();
        });
      };
      _server.listen("127.0.0.1", 0);
      _portCompleter.complete(_server.port);
      _scheduleCleanup((_) => _closeServer());
      return null;
    });
  });
}

/// Closes [_server]. Returns a [Future] that will complete after the [_server]
/// is closed.
Future _closeServer() {
  if (_server == null) return new Future.immediate(null);
  _server.close();
  _server = null;
  _portCompleterCache = null;
  // TODO(nweiz): Remove this once issue 4155 is fixed. Pumping the event loop
  // *seems* to be enough to ensure that the server is actually closed, but I'm
  // putting this at 10ms to be safe.
  return sleep(10);
}

/// The [DirectoryDescriptor] describing the server layout of packages that are
/// being served via [servePackages]. This is `null` if [servePackages] has not
/// yet been called for this test.
DirectoryDescriptor _servedPackageDir;

/// A map from package names to version numbers to YAML-serialized pubspecs for
/// those packages. This represents the packages currently being served by
/// [servePackages], and is `null` if [servePackages] has not yet been called
/// for this test.
Map<String, Map<String, String>> _servedPackages;

/// Creates an HTTP server that replicates the structure of pub.dartlang.org.
/// [pubspecs] is a list of unserialized pubspecs representing the packages to
/// serve.
///
/// Subsequent calls to [servePackages] will add to the set of packages that
/// are being served. Previous packages will continue to be served.
void servePackages(List<Map> pubspecs) {
  if (_servedPackages == null || _servedPackageDir == null) {
    _servedPackages = <String, Map<String, String>>{};
    _servedPackageDir = dir('packages', []);
    serve([_servedPackageDir]);

    _scheduleCleanup((_) {
      _servedPackages = null;
      _servedPackageDir = null;
    });
  }

  _schedule((_) {
    return _awaitObject(pubspecs).then((resolvedPubspecs) {
      for (var spec in resolvedPubspecs) {
        var name = spec['name'];
        var version = spec['version'];
        var versions = _servedPackages.putIfAbsent(
            name, () => <String, String>{});
        versions[version] = yaml(spec);
      }

      _servedPackageDir.contents.clear();
      for (var name in _servedPackages.keys) {
        var versions = _servedPackages[name].keys.toList();
        _servedPackageDir.contents.addAll([
          file('$name.json',
              json.stringify({'versions': versions})),
          dir(name, [
            dir('versions', flatten(versions.mappedBy((version) {
              return [
                file('$version.yaml', _servedPackages[name][version]),
                tar('$version.tar.gz', [
                  file('pubspec.yaml', _servedPackages[name][version]),
                  libDir(name, '$name $version')
                ])
              ];
            })))
          ])
        ]);
      }
    });
  });
}

/// Converts [value] into a YAML string.
String yaml(value) => json.stringify(value);

/// Describes a package that passes all validation.
Descriptor get normalPackage => dir(appPath, [
  libPubspec("test_pkg", "1.0.0"),
  file("LICENSE", "Eh, do what you want."),
  dir("lib", [
    file("test_pkg.dart", "int i = 1;")
  ])
]);

/// Describes a file named `pubspec.yaml` with the given YAML-serialized
/// [contents], which should be a serializable object.
///
/// [contents] may contain [Future]s that resolve to serializable objects,
/// which may in turn contain [Future]s recursively.
Descriptor pubspec(Map contents) {
  return async(_awaitObject(contents).then((resolvedContents) =>
      file("pubspec.yaml", yaml(resolvedContents))));
}

/// Describes a file named `pubspec.yaml` for an application package with the
/// given [dependencies].
Descriptor appPubspec(List dependencies) {
  return pubspec({
    "name": "myapp",
    "dependencies": _dependencyListToMap(dependencies)
  });
}

/// Describes a file named `pubspec.yaml` for a library package with the given
/// [name], [version], and [dependencies].
Descriptor libPubspec(String name, String version, [List dependencies]) =>
  pubspec(package(name, version, dependencies));

/// Describes a directory named `lib` containing a single dart file named
/// `<name>.dart` that contains a line of Dart code.
Descriptor libDir(String name, [String code]) {
  // Default to printing the name if no other code was given.
  if (code == null) {
    code = name;
  }

  return dir("lib", [
    file("$name.dart", 'main() => "$code";')
  ]);
}

/// Describes a map representing a library package with the given [name],
/// [version], and [dependencies].
Map package(String name, String version, [List dependencies]) {
  var package = {
    "name": name,
    "version": version,
    "author": "Nathan Weizenbaum <nweiz@google.com>",
    "homepage": "http://pub.dartlang.org",
    "description": "A package, I guess."
  };
  if (dependencies != null) {
    package["dependencies"] = _dependencyListToMap(dependencies);
  }
  return package;
}

/// Describes a map representing a dependency on a package in the package
/// repository.
Map dependency(String name, [String versionConstraint]) {
  var url = port.then((p) => "http://localhost:$p");
  var dependency = {"hosted": {"name": name, "url": url}};
  if (versionConstraint != null) dependency["version"] = versionConstraint;
  return dependency;
}

/// Describes a directory for a package installed from the mock package server.
/// This directory is of the form found in the global package cache.
DirectoryDescriptor packageCacheDir(String name, String version) {
  return dir("$name-$version", [
    libDir(name, '$name $version')
  ]);
}

/// Describes a directory for a Git package. This directory is of the form
/// found in the revision cache of the global package cache.
DirectoryDescriptor gitPackageRevisionCacheDir(String name, [int modifier]) {
  var value = name;
  if (modifier != null) value = "$name $modifier";
  return dir(new RegExp("$name${r'-[a-f0-9]+'}"), [
    libDir(name, value)
  ]);
}

/// Describes a directory for a Git package. This directory is of the form
/// found in the repo cache of the global package cache.
DirectoryDescriptor gitPackageRepoCacheDir(String name) {
  return dir(new RegExp("$name${r'-[a-f0-9]+'}"), [
    dir('hooks'),
    dir('info'),
    dir('objects'),
    dir('refs')
  ]);
}

/// Describes the `packages/` directory containing all the given [packages],
/// which should be name/version pairs. The packages will be validated against
/// the format produced by the mock package server.
///
/// A package with a null version should not be installed.
DirectoryDescriptor packagesDir(Map<String, String> packages) {
  var contents = <Descriptor>[];
  packages.forEach((name, version) {
    if (version == null) {
      contents.add(nothing(name));
    } else {
      contents.add(dir(name, [
        file("$name.dart", 'main() => "$name $version";')
      ]));
    }
  });
  return dir(packagesPath, contents);
}

/// Describes the global package cache directory containing all the given
/// [packages], which should be name/version pairs. The packages will be
/// validated against the format produced by the mock package server.
///
/// A package's value may also be a list of versions, in which case all
/// versions are expected to be installed.
DirectoryDescriptor cacheDir(Map packages) {
  var contents = <Descriptor>[];
  packages.forEach((name, versions) {
    if (versions is! List) versions = [versions];
    for (var version in versions) {
      contents.add(packageCacheDir(name, version));
    }
  });
  return dir(cachePath, [
    dir('hosted', [
      async(port.then((p) => dir('localhost%58$p', contents)))
    ])
  ]);
}

/// Describes the file in the system cache that contains the client's OAuth2
/// credentials. The URL "/token" on [server] will be used as the token
/// endpoint for refreshing the access token.
Descriptor credentialsFile(
    ScheduledServer server,
    String accessToken,
    {String refreshToken,
     DateTime expiration}) {
  return async(server.url.then((url) {
    return dir(cachePath, [
      file('credentials.json', new oauth2.Credentials(
          accessToken,
          refreshToken,
          url.resolve('/token'),
          ['https://www.googleapis.com/auth/userinfo.email'],
          expiration).toJson())
    ]);
  }));
}

/// Describes the application directory, containing only a pubspec specifying
/// the given [dependencies].
DirectoryDescriptor appDir(List dependencies) =>
  dir(appPath, [appPubspec(dependencies)]);

/// Converts a list of dependencies as passed to [package] into a hash as used
/// in a pubspec.
Future<Map> _dependencyListToMap(List<Map> dependencies) {
  return _awaitObject(dependencies).then((resolvedDependencies) {
    var result = <String, Map>{};
    for (var dependency in resolvedDependencies) {
      var keys = dependency.keys.where((key) => key != "version");
      var sourceName = only(keys);
      var source;
      switch (sourceName) {
      case "git":
        source = new GitSource();
        break;
      case "hosted":
        source = new HostedSource();
        break;
      case "sdk":
        source = new SdkSource();
        break;
      default:
        throw 'Unknown source "$sourceName"';
      }

      result[_packageName(sourceName, dependency[sourceName])] = dependency;
    }
    return result;
  });
}

/// Return the name for the package described by [description] and from
/// [sourceName].
String _packageName(String sourceName, description) {
  switch (sourceName) {
  case "git":
    var url = description is String ? description : description['url'];
    return basename(url.replaceFirst(new RegExp(r"(\.git)?/?$"), ""));
  case "hosted":
    if (description is String) return description;
    return description['name'];
  case "sdk":
    return description;
  default:
    return description;
  }
}

/// The path of the package cache directory used for tests. Relative to the
/// sandbox directory.
final String cachePath = "cache";

/// The path of the mock SDK directory used for tests. Relative to the sandbox
/// directory.
final String sdkPath = "sdk";

/// The path of the mock app directory used for tests. Relative to the sandbox
/// directory.
final String appPath = "myapp";

/// The path of the packages directory in the mock app used for tests. Relative
/// to the sandbox directory.
final String packagesPath = "$appPath/packages";

/// The type for callbacks that will be fired during [schedulePub]. Takes the
/// sandbox directory as a parameter.
typedef Future _ScheduledEvent(Directory parentDir);

/// The list of events that are scheduled to run as part of the test case.
List<_ScheduledEvent> _scheduled;

/// The list of events that are scheduled to run after the test case, even if
/// it failed.
List<_ScheduledEvent> _scheduledCleanup;

/// The list of events that are scheduled to run after the test case only if it
/// failed.
List<_ScheduledEvent> _scheduledOnException;

/// Set to true when the current batch of scheduled events should be aborted.
bool _abortScheduled = false;

/// The time (in milliseconds) to wait for the entire scheduled test to
/// complete.
final _TIMEOUT = 30000;

/// Defines an integration test. The [body] should schedule a series of
/// operations which will be run asynchronously.
void integration(String description, void body()) {
  test(description, () {
    body();
    _run();
  });
}

/// Runs all the scheduled events for a test case. This should only be called
/// once per test case.
void _run() {
  var createdSandboxDir;
  var asyncDone = expectAsync0(() {});

  Future cleanup() {
    return _runScheduled(createdSandboxDir, _scheduledCleanup).then((_) {
      _scheduled = null;
      _scheduledCleanup = null;
      _scheduledOnException = null;
      if (createdSandboxDir != null) return deleteDir(createdSandboxDir);
    });
  }

  timeout(_setUpSandbox().then((sandboxDir) {
    createdSandboxDir = sandboxDir;
    return _runScheduled(sandboxDir, _scheduled);
  }).catchError((e) {
    // If an error occurs during testing, delete the sandbox, throw the error so
    // that the test framework sees it, then finally call asyncDone so that the
    // test framework knows we're done doing asynchronous stuff.
    return _runScheduled(createdSandboxDir, _scheduledOnException)
        .then((_) => registerException(e.error, e.stackTrace)).catchError((e) {
      print("Exception while cleaning up: ${e.error}");
      print(e.stackTrace);
      registerException(e.error, e.stackTrace);
    });
  }), _TIMEOUT, 'waiting for a test to complete')
      .then((_) => cleanup())
      .then((_) => asyncDone());
}

/// Get the path to the root "util/test/pub" directory containing the pub
/// tests.
String get testDirectory {
  var dir = new Options().script;
  while (basename(dir) != 'pub') dir = dirname(dir);

  return getFullPath(dir);
}

/// Schedules a call to the Pub command-line utility. Runs Pub with [args] and
/// validates that its results match [output], [error], and [exitCode].
void schedulePub({List args, Pattern output, Pattern error,
    Future<Uri> tokenEndpoint, int exitCode: 0}) {
  _schedule((sandboxDir) {
    return _doPub(runProcess, sandboxDir, args, tokenEndpoint)
        .then((result) {
      var failures = [];

      _validateOutput(failures, 'stdout', output, result.stdout);
      _validateOutput(failures, 'stderr', error, result.stderr);

      if (result.exitCode != exitCode) {
        failures.add(
            'Pub returned exit code ${result.exitCode}, expected $exitCode.');
      }

      if (failures.length > 0) {
        if (error == null) {
          // If we aren't validating the error, still show it on failure.
          failures.add('Pub stderr:');
          failures.addAll(result.stderr.mappedBy((line) => '| $line'));
        }

        throw new ExpectException(Strings.join(failures, '\n'));
      }

      return null;
    });
  });
}

/// Starts a Pub process and returns a [ScheduledProcess] that supports
/// interaction with that process.
///
/// Any futures in [args] will be resolved before the process is started.
ScheduledProcess startPub({List args, Future<Uri> tokenEndpoint}) {
  var process = _scheduleValue((sandboxDir) =>
      _doPub(startProcess, sandboxDir, args, tokenEndpoint));
  return new ScheduledProcess("pub", process);
}

/// Like [startPub], but runs `pub lish` in particular with [server] used both
/// as the OAuth2 server (with "/token" as the token endpoint) and as the
/// package server.
///
/// Any futures in [args] will be resolved before the process is started.
ScheduledProcess startPubLish(ScheduledServer server, {List args}) {
  var tokenEndpoint = server.url.then((url) =>
      url.resolve('/token').toString());
  if (args == null) args = [];
  args = flatten(['lish', '--server', tokenEndpoint, args]);
  return startPub(args: args, tokenEndpoint: tokenEndpoint);
}

/// Handles the beginning confirmation process for uploading a packages.
/// Ensures that the right output is shown and then enters "y" to confirm the
/// upload.
void confirmPublish(ScheduledProcess pub) {
  // TODO(rnystrom): This is overly specific and inflexible regarding different
  // test packages. Should validate this a little more loosely.
  expectLater(pub.nextLine(), equals('Publishing "test_pkg" 1.0.0:'));
  expectLater(pub.nextLine(), equals("|-- LICENSE"));
  expectLater(pub.nextLine(), equals("|-- lib"));
  expectLater(pub.nextLine(), equals("|   '-- test_pkg.dart"));
  expectLater(pub.nextLine(), equals("'-- pubspec.yaml"));
  expectLater(pub.nextLine(), equals(""));

  pub.writeLine("y");
}


/// Calls [fn] with appropriately modified arguments to run a pub process. [fn]
/// should have the same signature as [startProcess], except that the returned
/// [Future] may have a type other than [Process].
Future _doPub(Function fn, sandboxDir, List args, Future<Uri> tokenEndpoint) {
  String pathInSandbox(path) => join(getFullPath(sandboxDir), path);

  return Future.wait([
    ensureDir(pathInSandbox(appPath)),
    _awaitObject(args),
    tokenEndpoint == null ? new Future.immediate(null) : tokenEndpoint
  ]).then((results) {
    var args = results[1];
    var tokenEndpoint = results[2];
    // Find a Dart executable we can use to spawn. Use the same one that was
    // used to run this script itself.
    var dartBin = new Options().executable;

    // If the executable looks like a path, get its full path. That way we
    // can still find it when we spawn it with a different working directory.
    if (dartBin.contains(Platform.pathSeparator)) {
      dartBin = new File(dartBin).fullPathSync();
    }

    // Find the main pub entrypoint.
    var pubPath = fs.joinPaths(testDirectory, '../../pub/pub.dart');

    var dartArgs = ['--checked', pubPath, '--trace'];
    dartArgs.addAll(args);

    var environment = {
      'PUB_CACHE': pathInSandbox(cachePath),
      'DART_SDK': pathInSandbox(sdkPath)
    };

    if (tokenEndpoint != null) {
      environment['_PUB_TEST_TOKEN_ENDPOINT'] = tokenEndpoint.toString();
    }

    return fn(dartBin, dartArgs, workingDir: pathInSandbox(appPath),
        environment: environment);
  });
}

/// Skips the current test if Git is not installed. This validates that the
/// current test is running on a buildbot in which case we expect git to be
/// installed. If we are not running on the buildbot, we will instead see if
/// git is installed and skip the test if not. This way, users don't need to
/// have git installed to run the tests locally (unless they actually care
/// about the pub git tests).
void ensureGit() {
  _schedule((_) {
    return gitlib.isInstalled.then((installed) {
      if (!installed &&
          !Platform.environment.containsKey('BUILDBOT_BUILDERNAME')) {
        _abortScheduled = true;
      }
      return null;
    });
  });
}

/// Use [client] as the mock HTTP client for this test.
///
/// Note that this will only affect HTTP requests made via http.dart in the
/// parent process.
void useMockClient(MockClient client) {
  var oldInnerClient = httpClient.inner;
  httpClient.inner = client;
  _scheduleCleanup((_) {
    httpClient.inner = oldInnerClient;
  });
}

Future<Directory> _setUpSandbox() => createTempDir();

Future _runScheduled(Directory parentDir, List<_ScheduledEvent> scheduled) {
  if (scheduled == null) return new Future.immediate(null);
  var iterator = scheduled.iterator;

  Future runNextEvent(_) {
    if (_abortScheduled || !iterator.moveNext()) {
      _abortScheduled = false;
      scheduled.clear();
      return new Future.immediate(null);
    }

    var future = iterator.current(parentDir);
    if (future != null) {
      return future.then(runNextEvent);
    } else {
      return runNextEvent(null);
    }
  }

  return runNextEvent(null);
}

/// Compares the [actual] output from running pub with [expected]. For [String]
/// patterns, ignores leading and trailing whitespace differences and tries to
/// report the offending difference in a nice way. For other [Pattern]s, just
/// reports whether the output contained the pattern.
void _validateOutput(List<String> failures, String pipe, Pattern expected,
                     List<String> actual) {
  if (expected == null) return;

  if (expected is RegExp) {
    _validateOutputRegex(failures, pipe, expected, actual);
  } else {
    _validateOutputString(failures, pipe, expected, actual);
  }
}

void _validateOutputRegex(List<String> failures, String pipe,
                          RegExp expected, List<String> actual) {
  var actualText = Strings.join(actual, '\n');
  if (actualText.contains(expected)) return;

  if (actual.length == 0) {
    failures.add('Expected $pipe to match "${expected.pattern}" but got none.');
  } else {
    failures.add('Expected $pipe to match "${expected.pattern}" but got:');
    failures.addAll(actual.mappedBy((line) => '| $line'));
  }
}

void _validateOutputString(List<String> failures, String pipe,
                           String expectedText, List<String> actual) {
  final expected = expectedText.split('\n');

  // Strip off the last line. This lets us have expected multiline strings
  // where the closing ''' is on its own line. It also fixes '' expected output
  // to expect zero lines of output, not a single empty line.
  expected.removeLast();

  var results = [];
  var failed = false;

  // Compare them line by line to see which ones match.
  var length = max(expected.length, actual.length);
  for (var i = 0; i < length; i++) {
    if (i >= actual.length) {
      // Missing output.
      failed = true;
      results.add('? ${expected[i]}');
    } else if (i >= expected.length) {
      // Unexpected extra output.
      failed = true;
      results.add('X ${actual[i]}');
    } else {
      var expectedLine = expected[i].trim();
      var actualLine = actual[i].trim();

      if (expectedLine != actualLine) {
        // Mismatched lines.
        failed = true;
        results.add('X ${actual[i]}');
      } else {
        // Output is OK, but include it in case other lines are wrong.
        results.add('| ${actual[i]}');
      }
    }
  }

  // If any lines mismatched, show the expected and actual.
  if (failed) {
    failures.add('Expected $pipe:');
    failures.addAll(expected.mappedBy((line) => '| $line'));
    failures.add('Got:');
    failures.addAll(results);
  }
}

/// Base class for [FileDescriptor] and [DirectoryDescriptor] so that a
/// directory can contain a heterogeneous collection of files and
/// subdirectories.
abstract class Descriptor {
  /// The name of this file or directory. This must be a [String] if the file
  /// or directory is going to be created.
  final Pattern name;

  Descriptor(this.name);

  /// Creates the file or directory within [dir]. Returns a [Future] that is
  /// completed after the creation is done.
  Future create(dir);

  /// Validates that this descriptor correctly matches the corresponding file
  /// system entry within [dir]. Returns a [Future] that completes to `null` if
  /// the entry is valid, or throws an error if it failed.
  Future validate(String dir);

  /// Deletes the file or directory within [dir]. Returns a [Future] that is
  /// completed after the deletion is done.
  Future delete(String dir);

  /// Loads the file at [path] from within this descriptor. If [path] is empty,
  /// loads the contents of the descriptor itself.
  InputStream load(List<String> path);

  /// Schedules the directory to be created before Pub is run with
  /// [schedulePub]. The directory will be created relative to the sandbox
  /// directory.
  // TODO(nweiz): Use implicit closurization once issue 2984 is fixed.
  void scheduleCreate() => _schedule((dir) => this.create(dir));

  /// Schedules the file or directory to be deleted recursively.
  void scheduleDelete() => _schedule((dir) => this.delete(dir));

  /// Schedules the directory to be validated after Pub is run with
  /// [schedulePub]. The directory will be validated relative to the sandbox
  /// directory.
  void scheduleValidate() => _schedule((parentDir) => validate(parentDir.path));

  /// Asserts that the name of the descriptor is a [String] and returns it.
  String get _stringName {
    if (name is String) return name;
    throw 'Pattern $name must be a string.';
  }

  /// Validates that at least one file in [dir] matching [name] is valid
  /// according to [validate]. [validate] should complete to an exception if
  /// the input path is invalid.
  Future _validateOneMatch(String dir, Future validate(String path)) {
    // Special-case strings to support multi-level names like "myapp/packages".
    if (name is String) {
      var path = join(dir, name);
      return exists(path).then((exists) {
        if (!exists) Expect.fail('File $name in $dir not found.');
        return validate(path);
      });
    }

    // TODO(nweiz): remove this when issue 4061 is fixed.
    var stackTrace;
    try {
      throw "";
    } catch (_, localStackTrace) {
      stackTrace = localStackTrace;
    }

    return listDir(dir).then((files) {
      var matches = files.where((file) => endsWithPattern(file, name)).toList();
      if (matches.isEmpty) {
        Expect.fail('No files in $dir match pattern $name.');
      }
      if (matches.length == 1) return validate(matches[0]);

      var failures = [];
      var successes = 0;
      var completer = new Completer();
      checkComplete() {
        if (failures.length + successes != matches.length) return;
        if (successes > 0) {
          completer.complete();
          return;
        }

        var error = new StringBuffer();
        error.add("No files named $name in $dir were valid:\n");
        for (var failure in failures) {
          error.add("  $failure\n");
        }
        completer.completeError(
            new ExpectException(error.toString()), stackTrace);
      }

      for (var match in matches) {
        var future = validate(match).then((_) {
          successes++;
          checkComplete();
        }).catchError((e) {
          failures.add(e);
          checkComplete();
        });
      }
      return completer.future;
    });
  }
}

/// Describes a file. These are used both for setting up an expected directory
/// tree before running a test, and for validating that the file system matches
/// some expectations after running it.
class FileDescriptor extends Descriptor {
  /// The text contents of the file.
  final String contents;

  FileDescriptor(Pattern name, this.contents) : super(name);

  /// Creates the file within [dir]. Returns a [Future] that is completed after
  /// the creation is done.
  Future<File> create(dir) {
    return writeTextFile(join(dir, _stringName), contents);
  }

  /// Deletes the file within [dir]. Returns a [Future] that is completed after
  /// the deletion is done.
  Future delete(dir) {
    return deleteFile(join(dir, _stringName));
  }

  /// Validates that this file correctly matches the actual file at [path].
  Future validate(String path) {
    return _validateOneMatch(path, (file) {
      return readTextFile(file).then((text) {
        if (text == contents) return null;

        Expect.fail('File $file should contain:\n\n$contents\n\n'
                    'but contained:\n\n$text');
      });
    });
  }

  /// Loads the contents of the file.
  InputStream load(List<String> path) {
    if (!path.isEmpty) {
      var joinedPath = Strings.join(path, '/');
      throw "Can't load $joinedPath from within $name: not a directory.";
    }

    var stream = new ListInputStream();
    stream.write(contents.charCodes);
    stream.markEndOfStream();
    return stream;
  }
}

/// Describes a directory and its contents. These are used both for setting up
/// an expected directory tree before running a test, and for validating that
/// the file system matches some expectations after running it.
class DirectoryDescriptor extends Descriptor {
  /// The files and directories contained in this directory.
  final List<Descriptor> contents;

  DirectoryDescriptor(Pattern name, List<Descriptor> contents)
    : this.contents = contents == null ? <Descriptor>[] : contents,
      super(name);

  /// Creates the file within [dir]. Returns a [Future] that is completed after
  /// the creation is done.
  Future<Directory> create(parentDir) {
    // Create the directory.
    return ensureDir(join(parentDir, _stringName)).then((dir) {
      if (contents == null) return new Future<Directory>.immediate(dir);

      // Recursively create all of its children.
      final childFutures =
          contents.mappedBy((child) => child.create(dir)).toList();
      // Only complete once all of the children have been created too.
      return Future.wait(childFutures).then((_) => dir);
    });
  }

  /// Deletes the directory within [dir]. Returns a [Future] that is completed
  /// after the deletion is done.
  Future delete(dir) {
    return deleteDir(join(dir, _stringName));
  }

  /// Validates that the directory at [path] contains all of the expected
  /// contents in this descriptor. Note that this does *not* check that the
  /// directory doesn't contain other unexpected stuff, just that it *does*
  /// contain the stuff we do expect.
  Future validate(String path) {
    return _validateOneMatch(path, (dir) {
      // Validate each of the items in this directory.
      final entryFutures =
          contents.mappedBy((entry) => entry.validate(dir)).toList();

      // If they are all valid, the directory is valid.
      return Future.wait(entryFutures).then((entries) => null);
    });
  }

  /// Loads [path] from within this directory.
  InputStream load(List<String> path) {
    if (path.isEmpty) {
      throw "Can't load the contents of $name: is a directory.";
    }

    for (var descriptor in contents) {
      if (descriptor.name == path[0]) {
        return descriptor.load(path.getRange(1, path.length - 1));
      }
    }

    throw "Directory $name doesn't contain ${Strings.join(path, '/')}.";
  }
}

/// Wraps a [Future] that will complete to a [Descriptor] and makes it behave
/// like a concrete [Descriptor]. This is necessary when the contents of the
/// descriptor depends on information that's not available until part of the
/// test run is completed.
class FutureDescriptor extends Descriptor {
  Future<Descriptor> _future;

  FutureDescriptor(this._future) : super('<unknown>');

  Future create(dir) => _future.then((desc) => desc.create(dir));

  Future validate(dir) => _future.then((desc) => desc.validate(dir));

  Future delete(dir) => _future.then((desc) => desc.delete(dir));

  InputStream load(List<String> path) {
    var resultStream = new ListInputStream();
    _future.then((desc) => pipeInputToInput(desc.load(path), resultStream));
    return resultStream;
  }
}

/// Describes a Git repository and its contents.
class GitRepoDescriptor extends DirectoryDescriptor {
  GitRepoDescriptor(Pattern name, List<Descriptor> contents)
  : super(name, contents);

  /// Creates the Git repository and commits the contents.
  Future<Directory> create(parentDir) {
    return _runGitCommands(parentDir, [
      ['init'],
      ['add', '.'],
      ['commit', '-m', 'initial commit']
    ]);
  }

  /// Commits any changes to the Git repository.
  Future commit(parentDir) {
    return _runGitCommands(parentDir, [
      ['add', '.'],
      ['commit', '-m', 'update']
    ]);
  }

  /// Schedules changes to be committed to the Git repository.
  void scheduleCommit() => _schedule((dir) => this.commit(dir));

  /// Return a Future that completes to the commit in the git repository
  /// referred to by [ref] at the current point in the scheduled test run.
  Future<String> revParse(String ref) {
    return _scheduleValue((parentDir) {
      return super.create(parentDir).then((rootDir) {
        return _runGit(['rev-parse', ref], rootDir);
      }).then((output) => output[0]);
    });
  }

  /// Schedule a Git command to run in this repository.
  void scheduleGit(List<String> args) {
    _schedule((parentDir) {
      var gitDir = new Directory(join(parentDir, name));
      return _runGit(args, gitDir);
    });
  }

  Future _runGitCommands(parentDir, List<List<String>> commands) {
    var workingDir;

    Future runGitStep(_) {
      if (commands.isEmpty) return new Future.immediate(workingDir);
      var command = commands.removeAt(0);
      return _runGit(command, workingDir).then(runGitStep);
    }

    return super.create(parentDir).then((rootDir) {
      workingDir = rootDir;
      return runGitStep(null);
    });
  }

  Future<String> _runGit(List<String> args, Directory workingDir) {
    // Explicitly specify the committer information. Git needs this to commit
    // and we don't want to rely on the buildbots having this already set up.
    var environment = {
      'GIT_AUTHOR_NAME': 'Pub Test',
      'GIT_AUTHOR_EMAIL': 'pub@dartlang.org',
      'GIT_COMMITTER_NAME': 'Pub Test',
      'GIT_COMMITTER_EMAIL': 'pub@dartlang.org'
    };

    return gitlib.run(args, workingDir: workingDir.path,
        environment: environment);
  }
}

/// Describes a gzipped tar file and its contents.
class TarFileDescriptor extends Descriptor {
  final List<Descriptor> contents;

  TarFileDescriptor(Pattern name, this.contents)
  : super(name);

  /// Creates the files and directories within this tar file, then archives
  /// them, compresses them, and saves the result to [parentDir].
  Future<File> create(parentDir) {
    // TODO(rnystrom): Use withTempDir().
    var tempDir;
    return createTempDir().then((_tempDir) {
      tempDir = _tempDir;
      return Future.wait(contents.mappedBy((child) => child.create(tempDir)));
    }).then((createdContents) {
      return consumeInputStream(createTarGz(createdContents, baseDir: tempDir));
    }).then((bytes) {
      return new File(join(parentDir, _stringName)).writeAsBytes(bytes);
    }).then((file) {
      return deleteDir(tempDir).then((_) => file);
    });
  }

  /// Validates that the `.tar.gz` file at [path] contains the expected
  /// contents.
  Future validate(String path) {
    throw "TODO(nweiz): implement this";
  }

  Future delete(dir) {
    throw new UnsupportedError('');
  }

  /// Loads the contents of this tar file.
  InputStream load(List<String> path) {
    if (!path.isEmpty) {
      var joinedPath = Strings.join(path, '/');
      throw "Can't load $joinedPath from within $name: not a directory.";
    }

    var sinkStream = new ListInputStream();
    var tempDir;
    // TODO(rnystrom): Use withTempDir() here.
    // TODO(nweiz): propagate any errors to the return value. See issue 3657.
    createTempDir().then((_tempDir) {
      tempDir = _tempDir;
      return create(tempDir);
    }).then((tar) {
      var sourceStream = tar.openInputStream();
      return pipeInputToInput(sourceStream, sinkStream).then((_) {
        tempDir.delete(recursive: true);
      });
    });
    return sinkStream;
  }
}

/// A descriptor that validates that no file exists with the given name.
class NothingDescriptor extends Descriptor {
  NothingDescriptor(String name) : super(name);

  Future create(dir) => new Future.immediate(null);
  Future delete(dir) => new Future.immediate(null);

  Future validate(String dir) {
    return exists(join(dir, name)).then((exists) {
      if (exists) Expect.fail('File $name in $dir should not exist.');
    });
  }

  InputStream load(List<String> path) {
    if (path.isEmpty) {
      throw "Can't load the contents of $name: it doesn't exist.";
    } else {
      throw "Can't load ${Strings.join(path, '/')} from within $name: $name "
        "doesn't exist.";
    }
  }
}

/// A function that creates a [Validator] subclass.
typedef Validator ValidatorCreator(Entrypoint entrypoint);

/// Schedules a single [Validator] to run on the [appPath]. Returns a scheduled
/// Future that contains the erros and warnings produced by that validator.
Future<Pair<List<String>, List<String>>> schedulePackageValidation(
    ValidatorCreator fn) {
  return _scheduleValue((sandboxDir) {
    var cache = new SystemCache.withSources(
        join(sandboxDir, cachePath));

    return Entrypoint.load(join(sandboxDir, appPath), cache)
        .then((entrypoint) {
      var validator = fn(entrypoint);
      return validator.validate().then((_) {
        return new Pair(validator.errors, validator.warnings);
      });
    });
  });
}

/// A matcher that matches a Pair.
Matcher pairOf(Matcher firstMatcher, Matcher lastMatcher) =>
   new _PairMatcher(firstMatcher, lastMatcher);

class _PairMatcher extends BaseMatcher {
  final Matcher _firstMatcher;
  final Matcher _lastMatcher;

  _PairMatcher(this._firstMatcher, this._lastMatcher);

  bool matches(item, MatchState matchState) {
    if (item is! Pair) return false;
    return _firstMatcher.matches(item.first, matchState) &&
        _lastMatcher.matches(item.last, matchState);
  }

  Description describe(Description description) {
    description.addAll("(", ", ", ")", [_firstMatcher, _lastMatcher]);
  }
}

/// The time (in milliseconds) to wait for scheduled events that could run
/// forever.
const _SCHEDULE_TIMEOUT = 10000;

/// A class representing a [Process] that is scheduled to run in the course of
/// the test. This class allows actions on the process to be scheduled
/// synchronously. All operations on this class are scheduled.
///
/// Before running the test, either [shouldExit] or [kill] must be called on
/// this to ensure that the process terminates when expected.
///
/// If the test fails, this will automatically print out any remaining stdout
/// and stderr from the process to aid debugging.
class ScheduledProcess {
  /// The name of the process. Used for error reporting.
  final String name;

  /// The process future that's scheduled to run.
  Future<Process> _processFuture;

  /// The process that's scheduled to run. It may be null.
  Process _process;

  /// The exit code of the scheduled program. It may be null.
  int _exitCode;

  /// A [StringInputStream] wrapping the stdout of the process that's scheduled
  /// to run.
  final Future<StringInputStream> _stdoutFuture;

  /// A [StringInputStream] wrapping the stderr of the process that's scheduled
  /// to run.
  final Future<StringInputStream> _stderrFuture;

  /// The exit code of the process that's scheduled to run. This will naturally
  /// only complete once the process has terminated.
  Future<int> get _exitCodeFuture => _exitCodeCompleter.future;

  /// The completer for [_exitCode].
  final Completer<int> _exitCodeCompleter = new Completer();

  /// Whether the user has scheduled the end of this process by calling either
  /// [shouldExit] or [kill].
  bool _endScheduled = false;

  /// Whether the process is expected to terminate at this point.
  bool _endExpected = false;

  /// Wraps a [Process] [Future] in a scheduled process.
  ScheduledProcess(this.name, Future<Process> process)
    : _processFuture = process,
      _stdoutFuture = process.then((p) => new StringInputStream(p.stdout)),
      _stderrFuture = process.then((p) => new StringInputStream(p.stderr)) {
    process.then((p) {
      _process = p;
    });

    _schedule((_) {
      if (!_endScheduled) {
        throw new StateError("Scheduled process $name must have shouldExit() "
            "or kill() called before the test is run.");
      }

      return process.then((p) {
        p.onExit = (c) {
          if (_endExpected) {
            _exitCode = c;
            _exitCodeCompleter.complete(c);
            return;
          }

          // Sleep for half a second in case _endExpected is set in the next
          // scheduled event.
          sleep(500).then((_) {
            if (_endExpected) {
              _exitCodeCompleter.complete(c);
              return;
            }

            _printStreams().then((_) {
              registerException(new ExpectException("Process $name ended "
                  "earlier than scheduled with exit code $c"));
            });
          });
        };
      });
    });

    _scheduleOnException((_) {
      if (_process == null) return;

      if (_exitCode == null) {
        print("\nKilling process $name prematurely.");
        _endExpected = true;
        _process.kill();
      }

      return _printStreams();
    });

    _scheduleCleanup((_) {
      if (_process == null) return;
      // Ensure that the process is dead and we aren't waiting on any IO.
      _process.kill();
      _process.stdout.close();
      _process.stderr.close();
    });
  }

  /// Reads the next line of stdout from the process.
  Future<String> nextLine() {
    return _scheduleValue((_) {
      return timeout(_stdoutFuture.then((stream) => readLine(stream)),
          _SCHEDULE_TIMEOUT,
          "waiting for the next stdout line from process $name");
    });
  }

  /// Reads the next line of stderr from the process.
  Future<String> nextErrLine() {
    return _scheduleValue((_) {
      return timeout(_stderrFuture.then((stream) => readLine(stream)),
          _SCHEDULE_TIMEOUT,
          "waiting for the next stderr line from process $name");
    });
  }

  /// Reads the remaining stdout from the process. This should only be called
  /// after kill() or shouldExit().
  Future<String> remainingStdout() {
    if (!_endScheduled) {
      throw new StateError("remainingStdout() should only be called after "
          "kill() or shouldExit().");
    }

    return _scheduleValue((_) {
      return timeout(_stdoutFuture.then(consumeStringInputStream),
          _SCHEDULE_TIMEOUT,
          "waiting for the last stdout line from process $name");
    });
  }

  /// Reads the remaining stderr from the process. This should only be called
  /// after kill() or shouldExit().
  Future<String> remainingStderr() {
    if (!_endScheduled) {
      throw new StateError("remainingStderr() should only be called after "
          "kill() or shouldExit().");
    }

    return _scheduleValue((_) {
      return timeout(_stderrFuture.then(consumeStringInputStream),
          _SCHEDULE_TIMEOUT,
          "waiting for the last stderr line from process $name");
    });
  }

  /// Writes [line] to the process as stdin.
  void writeLine(String line) {
    _schedule((_) => _processFuture.then(
        (p) => p.stdin.writeString('$line\n')));
  }

  /// Kills the process, and waits until it's dead.
  void kill() {
    _endScheduled = true;
    _schedule((_) {
      _endExpected = true;
      _process.kill();
      timeout(_exitCodeCompleter.future, _SCHEDULE_TIMEOUT,
          "waiting for process $name to die");
    });
  }

  /// Waits for the process to exit, and verifies that the exit code matches
  /// [expectedExitCode] (if given).
  void shouldExit([int expectedExitCode]) {
    _endScheduled = true;
    _schedule((_) {
      _endExpected = true;
      return timeout(_exitCodeCompleter.future, _SCHEDULE_TIMEOUT,
          "waiting for process $name to exit").then((exitCode) {
        if (expectedExitCode != null) {
          expect(exitCode, equals(expectedExitCode));
        }
      });
    });
  }

  /// Prints the remaining data in the process's stdout and stderr streams.
  /// Prints nothing if the straems are empty.
  Future _printStreams() {
    Future printStream(String streamName, StringInputStream stream) {
      return consumeStringInputStream(stream).then((output) {
        if (output.isEmpty) return;

        print('\nProcess $name $streamName:');
        for (var line in output.trim().split("\n")) {
          print('| $line');
        }
        return;
      });
    }

    return _stdoutFuture.then((stdout) {
      return _stderrFuture.then((stderr) {
        return printStream('stdout', stdout)
            .then((_) => printStream('stderr', stderr));
      });
    });
  }
}

/// A class representing an [HttpServer] that's scheduled to run in the course
/// of the test. This class allows the server's request handling to be
/// scheduled synchronously. All operations on this class are scheduled.
class ScheduledServer {
  /// The wrapped server.
  final Future<HttpServer> _server;

  /// The queue of handlers to run for upcoming requests.
  final _handlers = new Queue<Future>();

  /// The requests to be ignored.
  final _ignored = new Set<Pair<String, String>>();

  ScheduledServer._(this._server);

  /// Creates a new server listening on an automatically-allocated port on
  /// localhost.
  factory ScheduledServer() {
    var scheduledServer;
    scheduledServer = new ScheduledServer._(_scheduleValue((_) {
      var server = new HttpServer();
      server.defaultRequestHandler = scheduledServer._awaitHandle;
      server.listen("127.0.0.1", 0);
      _scheduleCleanup((_) => server.close());
      return new Future.immediate(server);
    }));
    return scheduledServer;
  }

  /// The port on which the server is listening.
  Future<int> get port => _server.then((s) => s.port);

  /// The base URL of the server, including its port.
  Future<Uri> get url =>
    port.then((p) => Uri.parse("http://localhost:$p"));

  /// Assert that the next request has the given [method] and [path], and pass
  /// it to [handler] to handle. If [handler] returns a [Future], wait until
  /// it's completed to continue the schedule.
  void handle(String method, String path,
      Future handler(HttpRequest request, HttpResponse response)) {
    var handlerCompleter = new Completer<Function>();
    _scheduleValue((_) {
      var requestCompleteCompleter = new Completer();
      handlerCompleter.complete((request, response) {
        expect(request.method, equals(method));
        // TODO(nweiz): Use request.path once issue 7464 is fixed.
        expect(Uri.parse(request.uri).path, equals(path));

        var future = handler(request, response);
        if (future == null) future = new Future.immediate(null);
        chainToCompleter(future, requestCompleteCompleter);
      });
      return timeout(requestCompleteCompleter.future,
          _SCHEDULE_TIMEOUT, "waiting for $method $path");
    });
    _handlers.add(handlerCompleter.future);
  }

  /// Ignore all requests with the given [method] and [path]. If one is
  /// received, don't respond to it.
  void ignore(String method, String path) =>
    _ignored.add(new Pair(method, path));

  /// Raises an error complaining of an unexpected request.
  void _awaitHandle(HttpRequest request, HttpResponse response) {
    if (_ignored.contains(new Pair(request.method, request.path))) return;
    var future = timeout(new Future.immediate(null).then((_) {
      if (_handlers.isEmpty) {
        fail('Unexpected ${request.method} request to ${request.path}.');
      }
      return _handlers.removeFirst();
    }).then((handler) {
      handler(request, response);
    }), _SCHEDULE_TIMEOUT, "waiting for a handler for ${request.method} "
        "${request.path}");
    expect(future, completes);
  }
}

/// Takes a simple data structure (composed of [Map]s, [List]s, scalar objects,
/// and [Future]s) and recursively resolves all the [Future]s contained within.
/// Completes with the fully resolved structure.
Future _awaitObject(object) {
  // Unroll nested futures.
  if (object is Future) return object.then(_awaitObject);
  if (object is Collection) {
    return Future.wait(object.mappedBy(_awaitObject).toList());
  }
  if (object is! Map) return new Future.immediate(object);

  var pairs = <Future<Pair>>[];
  object.forEach((key, value) {
    pairs.add(_awaitObject(value)
        .then((resolved) => new Pair(key, resolved)));
  });
  return Future.wait(pairs).then((resolvedPairs) {
    var map = {};
    for (var pair in resolvedPairs) {
      map[pair.first] = pair.last;
    }
    return map;
  });
}

/// Schedules a callback to be called as part of the test case.
void _schedule(_ScheduledEvent event) {
  if (_scheduled == null) _scheduled = [];
  _scheduled.add(event);
}

/// Like [_schedule], but pipes the return value of [event] to a returned
/// [Future].
Future _scheduleValue(_ScheduledEvent event) {
  var completer = new Completer();
  _schedule((parentDir) {
    chainToCompleter(event(parentDir), completer);
    return completer.future;
  });
  return completer.future;
}

/// Schedules a callback to be called after the test case has completed, even
/// if it failed.
void _scheduleCleanup(_ScheduledEvent event) {
  if (_scheduledCleanup == null) _scheduledCleanup = [];
  _scheduledCleanup.add(event);
}

/// Schedules a callback to be called after the test case has completed, but
/// only if it failed.
void _scheduleOnException(_ScheduledEvent event) {
  if (_scheduledOnException == null) _scheduledOnException = [];
  _scheduledOnException.add(event);
}

/// Like [expect], but for [Future]s that complete as part of the scheduled
/// test. This is necessary to ensure that the exception thrown by the
/// expectation failing is handled by the scheduler.
///
/// Note that [matcher] matches against the completed value of [actual], so
/// calling [completion] is unnecessary.
void expectLater(Future actual, matcher, {String reason,
    FailureHandler failureHandler, bool verbose: false}) {
  _schedule((_) {
    return actual.then((value) {
      expect(value, matcher, reason: reason, failureHandler: failureHandler,
          verbose: false);
    });
  });
}
