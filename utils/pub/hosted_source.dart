// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library hosted_source;

import 'dart:async';
import 'dart:io' as io;
import 'dart:json' as json;
import 'dart:uri';

// TODO(nweiz): Make this import better.
import '../../pkg/http/lib/http.dart' as http;
import 'http.dart';
import 'io.dart';
import 'log.dart' as log;
import 'package.dart';
import 'pubspec.dart';
import 'source.dart';
import 'source_registry.dart';
import 'utils.dart';
import 'version.dart';

/// A package source that installs packages from a package hosting site that
/// uses the same API as pub.dartlang.org.
class HostedSource extends Source {
  final name = "hosted";
  final shouldCache = true;

  /// The URL of the default package repository.
  static final defaultUrl = "http://pub.dartlang.org";

  /// Downloads a list of all versions of a package that are available from the
  /// site.
  Future<List<Version>> getVersions(String name, description) {
    var parsed = _parseDescription(description);
    var fullUrl = "${parsed.last}/packages/${parsed.first}.json";

    return httpClient.read(fullUrl).then((body) {
      var doc = json.parse(body);
      return doc['versions']
          .mappedBy((version) => new Version.parse(version))
          .toList();
    }).catchError((ex) {
      _throwFriendlyError(ex, parsed.first, parsed.last);
    });
  }

  /// Downloads and parses the pubspec for a specific version of a package that
  /// is available from the site.
  Future<Pubspec> describe(PackageId id) {
    var parsed = _parseDescription(id.description);
    var fullUrl = "${parsed.last}/packages/${parsed.first}/versions/"
      "${id.version}.yaml";

    return httpClient.read(fullUrl).then((yaml) {
      return new Pubspec.parse(yaml, systemCache.sources);
    }).catchError((ex) {
      _throwFriendlyError(ex, id, parsed.last);
    });
  }

  /// Downloads a package from the site and unpacks it.
  Future<bool> install(PackageId id, String destPath) {
    var parsedDescription = _parseDescription(id.description);
    var name = parsedDescription.first;
    var url = parsedDescription.last;

    var fullUrl = "$url/packages/$name/versions/${id.version}.tar.gz";

    log.message('Downloading $id...');

    // Download and extract the archive to a temp directory.
    var tempDir;
    return Future.wait([
      httpClient.send(new http.Request("GET", Uri.parse(fullUrl)))
          .then((response) => response.stream),
      systemCache.createTempDir()
    ]).then((args) {
      var stream = streamToInputStream(args[0]);
      tempDir = args[1];
      return timeout(extractTarGz(stream, tempDir), HTTP_TIMEOUT,
          'fetching URL "$fullUrl"');
    }).then((_) {
      // Now that the install has succeeded, move it to the real location in
      // the cache. This ensures that we don't leave half-busted ghost
      // directories in the user's pub cache if an install fails.
      return renameDir(tempDir, destPath);
    }).then((_) => true);
  }

  /// The system cache directory for the hosted source contains subdirectories
  /// for each separate repository URL that's used on the system. Each of these
  /// subdirectories then contains a subdirectory for each package installed
  /// from that site.
  String systemCacheDirectory(PackageId id) {
    var parsed = _parseDescription(id.description);
    var url = parsed.last.replaceAll(new RegExp(r"^https?://"), "");
    var urlDir = replace(url, new RegExp(r'[<>:"\\/|?*%]'), (match) {
      return '%${match[0].charCodeAt(0)}';
    });
    return join(systemCacheRoot, urlDir, "${parsed.first}-${id.version}");
  }

  String packageName(description) => _parseDescription(description).first;

  bool descriptionsEqual(description1, description2) =>
      _parseDescription(description1) == _parseDescription(description2);

  /// Ensures that [description] is a valid hosted package description.
  ///
  /// There are two valid formats. A plain string refers to a package with the
  /// given name from the default host, while a map with keys "name" and "url"
  /// refers to a package with the given name from the host at the given URL.
  void validateDescription(description, {bool fromLockFile: false}) {
    _parseDescription(description);
  }

  /// When an error occurs trying to read something about [package] from [url],
  /// this tries to translate into a more user friendly error message. Always
  /// throws an error, either the original one or a better one.
  void _throwFriendlyError(AsyncError asyncError, package, url) {
    if (asyncError.error is PubHttpException &&
        asyncError.error.response.statusCode == 404) {
      throw 'Could not find package "$package" at $url.';
    }

    if (asyncError.error is TimeoutException) {
      throw 'Timed out trying to find package "$package" at $url.';
    }

    if (asyncError.error is io.SocketIOException) {
      throw 'Got socket error trying to find package "$package" at $url.\n'
          '${asyncError.error.osError}';
    }

    // Otherwise re-throw the original exception.
    throw asyncError;
  }

  /// Parses the description for a package.
  ///
  /// If the package parses correctly, this returns a (name, url) pair. If not,
  /// this throws a descriptive FormatException.
  Pair<String, String> _parseDescription(description) {
    if (description is String) {
      return new Pair<String, String>(description, defaultUrl);
    }

    if (description is! Map) {
      throw new FormatException(
          "The description must be a package name or map.");
    }

    if (!description.containsKey("name")) {
      throw new FormatException(
          "The description map must contain a 'name' key.");
    }

    var name = description["name"];
    if (name is! String) {
      throw new FormatException("The 'name' key must have a string value.");
    }

    var url = description.containsKey("url") ? description["url"] : defaultUrl;
    return new Pair<String, String>(name, url);
  }
}
