// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart.io;

class _Directory implements Directory {
  static const CREATE_REQUEST = 0;
  static const DELETE_REQUEST = 1;
  static const EXISTS_REQUEST = 2;
  static const CREATE_TEMP_REQUEST = 3;
  static const LIST_REQUEST = 4;
  static const RENAME_REQUEST = 5;

  static const SUCCESS_RESPONSE = 0;
  static const ILLEGAL_ARGUMENT_RESPONSE = 1;
  static const OSERROR_RESPONSE = 2;

  _Directory(String this._path);
  _Directory.fromPath(Path path) : this(path.toNativePath());
  _Directory.current() : this(_current());

  external static String _current();
  external static _createTemp(String template);
  external static int _exists(String path);
  external static _create(String path);
  external static _delete(String path, bool recursive);
  external static _rename(String path, String newPath);
  external static List _list(String path, bool recursive);
  external static SendPort _newServicePort();

  Future<bool> exists() {
    _ensureDirectoryService();
    List request = new List.fixedLength(2);
    request[0] = EXISTS_REQUEST;
    request[1] = _path;
    return _directoryService.call(request).then((response) {
      if (_isErrorResponse(response)) {
        throw _exceptionOrErrorFromResponse(response, "Exists failed");
      }
      return response == 1;
    });
  }

  bool existsSync() {
    if (_path is !String) {
      throw new ArgumentError();
    }
    var result = _exists(_path);
    if (result is OSError) {
      throw new DirectoryIOException("Exists failed", _path, result);
    }
    return (result == 1);
  }

  // Compute the index of the first directory in the list that exists. If
  // none of the directories exist dirsToCreate.length is returned.
  Future<int> _computeExistingIndex(List dirsToCreate) {
    var future;
    var notFound = dirsToCreate.length;
    for (var i = 0; i < dirsToCreate.length; i++) {
      if (future == null) {
        future = dirsToCreate[i].exists().then((e) => e ? i : notFound);
      } else {
        future = future.then((index) {
          if (index != notFound) {
            return new Future.immediate(index);
          }
          return dirsToCreate[i].exists().then((e) => e ? i : notFound);
        });
      }
    }
    if (future == null) {
      return new Future.immediate(notFound);
    } else {
      return future;
    }
  }

  Future<Directory> createRecursively() {
    if (_path is !String) {
      throw new ArgumentError();
    }
    var path = new Path(_path);
    var dirsToCreate = [];
    var terminator = path.isAbsolute ? '/' : '';
    while (path.toString() != terminator) {
      dirsToCreate.add(new Directory.fromPath(path));
      path = path.directoryPath;
    }
    return _computeExistingIndex(dirsToCreate).then((index) {
      var future;
      for (var i = index - 1; i >= 0 ; i--) {
        if (future == null) {
          future = dirsToCreate[i].create();
        } else {
          future = future.then((_) {
            return dirsToCreate[i].create();
          });
        }
      }
      if (future == null) {
        return new Future.immediate(this);
      } else {
        return future.then((_) => this);
      }
    });
  }

  Future<Directory> create({recursive: false}) {
    if (recursive) return createRecursively();
    _ensureDirectoryService();
    List request = new List.fixedLength(2);
    request[0] = CREATE_REQUEST;
    request[1] = _path;
    return _directoryService.call(request).then((response) {
      if (_isErrorResponse(response)) {
        throw _exceptionOrErrorFromResponse(response, "Creation failed");
      }
      return this;
    });
  }

  void createRecursivelySync() {
    var path = new Path(_path);
    var dirsToCreate = [];
    var terminator = path.isAbsolute ? '/' : '';
    while (path.toString() != terminator) {
      var dir = new Directory.fromPath(path);
      if (dir.existsSync()) break;
      dirsToCreate.add(dir);
      path = path.directoryPath;
    }
    for (var i = dirsToCreate.length - 1; i >= 0; i--) {
      dirsToCreate[i].createSync();
    }
  }

  void createSync({recursive: false}) {
    if (_path is !String) {
      throw new ArgumentError();
    }
    if (recursive) return createRecursivelySync();
    var result = _create(_path);
    if (result is OSError) {
      throw new DirectoryIOException("Creation failed", _path, result);
    }
  }

  Future<Directory> createTemp() {
    _ensureDirectoryService();
    List request = new List.fixedLength(2);
    request[0] = CREATE_TEMP_REQUEST;
    request[1] = _path;
    return _directoryService.call(request).then((response) {
      if (_isErrorResponse(response)) {
        throw _exceptionOrErrorFromResponse(response,
                                      "Creation of temporary directory failed");
      }
      return new Directory(response);
    });
  }

  Directory createTempSync() {
    if (_path is !String) {
      throw new ArgumentError();
    }
    var result = _createTemp(path);
    if (result is OSError) {
      throw new DirectoryIOException("Creation of temporary directory failed",
                                     _path,
                                     result);
    }
    return new Directory(result);
  }

  Future<Directory> _deleteHelper(bool recursive, String errorMsg) {
  }

  Future<Directory> delete({recursive: false}) {
    _ensureDirectoryService();
    List request = new List.fixedLength(3);
    request[0] = DELETE_REQUEST;
    request[1] = _path;
    request[2] = recursive;
    return _directoryService.call(request).then((response) {
      if (_isErrorResponse(response)) {
        throw _exceptionOrErrorFromResponse(response, "Deletion failed");
      }
      return this;
    });
  }

  void deleteSync({recursive: false}) {
    if (_path is !String) {
      throw new ArgumentError();
    }
    var result = _delete(_path, recursive);
    if (result is OSError) {
      throw new DirectoryIOException("Deletion failed", _path, result);
    }
  }

  Future<Directory> rename(String newPath) {
    _ensureDirectoryService();
    List request = new List.fixedLength(3);
    request[0] = RENAME_REQUEST;
    request[1] = _path;
    request[2] = newPath;
    return _directoryService.call(request).then((response) {
      if (_isErrorResponse(response)) {
        throw _exceptionOrErrorFromResponse(response, "Rename failed");
      }
      return new Directory(newPath);
    });
  }

  Directory renameSync(String newPath) {
    if (_path is !String || newPath is !String) {
      throw new ArgumentError();
    }
    var result = _rename(_path, newPath);
    if (result is OSError) {
      throw new DirectoryIOException("Rename failed", _path, result);
    }
    return new Directory(newPath);
  }

  DirectoryLister list({bool recursive: false}) {
    return new _DirectoryLister(_path, recursive);
  }

  List listSync({bool recursive: false}) {
    if (_path is! String || recursive is! bool) {
      throw new ArgumentError();
    }
    return _list(_path, recursive);
  }

  String get path => _path;

  String toString() => "Directory: '$path'";

  bool _isErrorResponse(response) {
    return response is List && response[0] != _SUCCESS_RESPONSE;
  }

  _exceptionOrErrorFromResponse(response, String message) {
    assert(_isErrorResponse(response));
    switch (response[_ERROR_RESPONSE_ERROR_TYPE]) {
      case _ILLEGAL_ARGUMENT_RESPONSE:
        return new ArgumentError();
      case _OSERROR_RESPONSE:
        var err = new OSError(response[_OSERROR_RESPONSE_MESSAGE],
                              response[_OSERROR_RESPONSE_ERROR_CODE]);
        return new DirectoryIOException(message, _path, err);
      default:
        return new Exception("Unknown error");
    }
  }

  void _ensureDirectoryService() {
    if (_directoryService == null) {
      _directoryService = _newServicePort();
    }
  }

  final String _path;
  SendPort _directoryService;
}

class _DirectoryLister implements DirectoryLister {
  _DirectoryLister(String path, bool recursive) {
    const int LIST_DIRECTORY = 0;
    const int LIST_FILE = 1;
    const int LIST_ERROR = 2;
    const int LIST_DONE = 3;

    final int RESPONSE_TYPE = 0;
    final int RESPONSE_PATH = 1;
    final int RESPONSE_COMPLETE = 1;
    final int RESPONSE_ERROR = 2;

    List request = new List.fixedLength(3);
    request[0] = _Directory.LIST_REQUEST;
    request[1] = path;
    request[2] = recursive;
    ReceivePort responsePort = new ReceivePort();
    // Use a separate directory service port for each listing as
    // listing operations on the same directory can run in parallel.
    _Directory._newServicePort().send(request, responsePort.toSendPort());
    responsePort.receive((message, replyTo) {
      if (message is !List || message[RESPONSE_TYPE] is !int) {
        responsePort.close();
        _reportError(new DirectoryIOException("Internal error"));
        return;
      }
      switch (message[RESPONSE_TYPE]) {
        case LIST_DIRECTORY:
          if (_onDir != null) _onDir(message[RESPONSE_PATH]);
          break;
        case LIST_FILE:
          if (_onFile != null) _onFile(message[RESPONSE_PATH]);
          break;
        case LIST_ERROR:
          var errorType =
              message[RESPONSE_ERROR][_ERROR_RESPONSE_ERROR_TYPE];
          if (errorType == _ILLEGAL_ARGUMENT_RESPONSE) {
            _reportError(new ArgumentError());
          } else if (errorType == _OSERROR_RESPONSE) {
            var responseError = message[RESPONSE_ERROR];
            var err = new OSError(
                responseError[_OSERROR_RESPONSE_MESSAGE],
                responseError[_OSERROR_RESPONSE_ERROR_CODE]);
            var errorPath = message[RESPONSE_PATH];
            if (errorPath == null) errorPath = path;
            _reportError(new DirectoryIOException("Directory listing failed",
                                                  errorPath,
                                                  err));
          } else {
            _reportError(new DirectoryIOException("Internal error"));
          }
          break;
        case LIST_DONE:
          responsePort.close();
          if (_onDone != null) _onDone(message[RESPONSE_COMPLETE]);
          break;
      }
    });
  }

  void set onDir(void onDir(String dir)) {
    _onDir = onDir;
  }

  void set onFile(void onFile(String file)) {
    _onFile = onFile;
  }

  void set onDone(void onDone(bool completed)) {
    _onDone = onDone;
  }

  void set onError(void onError(e)) {
    _onError = onError;
  }

  void _reportError(e) {
    if (_onError != null) {
      _onError(e);
    } else {
      throw e;
    }
  }

  Function _onDir;
  Function _onFile;
  Function _onDone;
  Function _onError;
}
