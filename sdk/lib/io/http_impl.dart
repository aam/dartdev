// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart.io;

// The close queue handles graceful closing of HTTP connections. When
// a connection is added to the queue it will enter a wait state
// waiting for all data written and possibly socket shutdown from
// peer.
class _CloseQueue {
  _CloseQueue() : _q = new Set<_HttpConnectionBase>();

  void add(_HttpConnectionBase connection) {
    void closeIfDone() {
      // When either the client has closed or all data has been
      // written to the client we close the underlying socket
      // completely.
      if (connection._isWriteClosed || connection._isReadClosed) {
        _q.remove(connection);
        connection._socket.close();
        if (connection.onClosed != null) connection.onClosed();
      }
    }

    // If the connection is already fully closed don't insert it into
    // the queue.
    if (connection._isFullyClosed) {
      connection._socket.close();
      if (connection.onClosed != null) connection.onClosed();
      return;
    }

    connection._state |= _HttpConnectionBase.CLOSING;
    _q.add(connection);

    // If the output stream is not closed for writing, close it now and
    // wait for callback when closed.
    if (!connection._isWriteClosed) {
      connection._socket.outputStream.close();
      connection._socket.outputStream.onClosed = () {
        connection._state |= _HttpConnectionBase.WRITE_CLOSED;
        closeIfDone();
      };
    } else {
      connection._socket.outputStream.onClosed = () { assert(false); };
    }

    // If the request is not already fully read wait for the socket to close.
    // As the _isReadClosed state from the HTTP request processing indicate
    // that the response has been parsed this does not necesarily mean tha
    // the socket is closed.
    if (!connection._isReadClosed) {
      connection._socket.onClosed = () {
        connection._state |= _HttpConnectionBase.READ_CLOSED;
        closeIfDone();
      };
    }

    // Ignore any data on a socket in the close queue.
    connection._socket.onData = connection._socket.read;

    // If an error occurs immediately close the socket.
    connection._socket.onError = (e) {
      connection._state |= _HttpConnectionBase.READ_CLOSED;
      connection._state |= _HttpConnectionBase.WRITE_CLOSED;
      closeIfDone();
    };
  }

  void shutdown() {
    _q.forEach((_HttpConnectionBase connection) {
      connection._socket.close();
    });
  }

  final Set<_HttpConnectionBase> _q;
}


class _HttpRequestResponseBase {
  static const int START = 0;
  static const int HEADER_SENT = 1;
  static const int DONE = 2;
  static const int UPGRADED = 3;

  _HttpRequestResponseBase(_HttpConnectionBase this._httpConnection)
      : _state = START, _headResponse = false;

  int get contentLength => _headers.contentLength;
  HttpHeaders get headers => _headers;

  bool get persistentConnection {
    List<String> connection = headers[HttpHeaders.CONNECTION];
    if (_protocolVersion == "1.1") {
      if (connection == null) return true;
      return !headers[HttpHeaders.CONNECTION].any(
          (value) => value.toLowerCase() == "close");
    } else {
      if (connection == null) return false;
      return headers[HttpHeaders.CONNECTION].any(
          (value) => value.toLowerCase() == "keep-alive");
    }
  }

  X509Certificate get certificate {
    var socket = _httpConnection._socket as SecureSocket;
    return socket == null ? socket : socket.peerCertificate;
  }

  void set persistentConnection(bool persistentConnection) {
    if (_outputStream != null) throw new HttpException("Header already sent");

    // Determine the value of the "Connection" header.
    headers.remove(HttpHeaders.CONNECTION, "close");
    headers.remove(HttpHeaders.CONNECTION, "keep-alive");
    if (_protocolVersion == "1.1" && !persistentConnection) {
      headers.add(HttpHeaders.CONNECTION, "close");
    } else if (_protocolVersion == "1.0" && persistentConnection) {
      headers.add(HttpHeaders.CONNECTION, "keep-alive");
    }
  }


  bool _write(List<int> data, bool copyBuffer) {
    if (_headResponse) return true;
    _ensureHeadersSent();
    bool allWritten = true;
    if (data.length > 0) {
      if (_headers.chunkedTransferEncoding) {
        // Write chunk size if transfer encoding is chunked.
        _writeHexString(data.length);
        _writeCRLF();
        _httpConnection._write(data, copyBuffer);
        allWritten = _writeCRLF();
      } else {
        _updateContentLength(data.length);
        allWritten = _httpConnection._write(data, copyBuffer);
      }
    }
    return allWritten;
  }

  bool _writeList(List<int> data, int offset, int count) {
    if (_headResponse) return true;
    _ensureHeadersSent();
    bool allWritten = true;
    if (count > 0) {
      if (_headers.chunkedTransferEncoding) {
        // Write chunk size if transfer encoding is chunked.
        _writeHexString(count);
        _writeCRLF();
        _httpConnection._writeFrom(data, offset, count);
        allWritten = _writeCRLF();
      } else {
        _updateContentLength(count);
        allWritten = _httpConnection._writeFrom(data, offset, count);
      }
    }
    return allWritten;
  }

  bool _writeDone() {
    bool allWritten = true;
    if (_headers.chunkedTransferEncoding) {
      // Terminate the content if transfer encoding is chunked.
      allWritten = _httpConnection._write(_Const.END_CHUNKED);
    } else {
      if (!_headResponse && _bodyBytesWritten < _headers.contentLength) {
        throw new HttpException("Sending less than specified content length");
      }
      assert(_headResponse || _bodyBytesWritten == _headers.contentLength);
    }
    return allWritten;
  }

  bool _writeHeaders() {
    _headers._write(_httpConnection);
    // Terminate header.
    return _writeCRLF();
  }

  bool _writeHexString(int x) {
    final List<int> hexDigits = [0x30, 0x31, 0x32, 0x33, 0x34,
                                 0x35, 0x36, 0x37, 0x38, 0x39,
                                 0x41, 0x42, 0x43, 0x44, 0x45, 0x46];
    List<int> hex = new Uint8List(10);
    int index = hex.length;
    while (x > 0) {
      index--;
      hex[index] = hexDigits[x % 16];
      x = x >> 4;
    }
    return _httpConnection._writeFrom(hex, index, hex.length - index);
  }

  bool _writeCRLF() {
    final CRLF = const [_CharCode.CR, _CharCode.LF];
    return _httpConnection._write(CRLF);
  }

  bool _writeSP() {
    final SP = const [_CharCode.SP];
    return _httpConnection._write(SP);
  }

  void _ensureHeadersSent() {
    // Ensure that headers are written.
    if (_state == START) {
      _writeHeader();
    }
  }

  void _updateContentLength(int bytes) {
    if (_bodyBytesWritten + bytes > _headers.contentLength) {
      throw new HttpException("Writing more than specified content length");
    }
    _bodyBytesWritten += bytes;
  }

  HttpConnectionInfo get connectionInfo => _httpConnection.connectionInfo;

  bool get _done => _state == DONE;

  int _state;
  bool _headResponse;

  _HttpConnectionBase _httpConnection;
  _HttpHeaders _headers;
  List<Cookie> _cookies;
  String _protocolVersion = "1.1";

  // Number of body bytes written. This is only actual body data not
  // including headers or chunk information of using chinked transfer
  // encoding.
  int _bodyBytesWritten = 0;
}


// Parsed HTTP request providing information on the HTTP headers.
class _HttpRequest extends _HttpRequestResponseBase implements HttpRequest {
  _HttpRequest(_HttpConnection connection) : super(connection);

  String get method => _method;
  String get uri => _uri;
  String get path => _path;
  String get queryString => _queryString;
  Map get queryParameters => _queryParameters;

  List<Cookie> get cookies {
    if (_cookies != null) return _cookies;

    // Parse a Cookie header value according to the rules in RFC 6265.
   void _parseCookieString(String s) {
      int index = 0;

      bool done() => index == s.length;

      void skipWS() {
        while (!done()) {
         if (s[index] != " " && s[index] != "\t") return;
         index++;
        }
      }

      String parseName() {
        int start = index;
        while (!done()) {
          if (s[index] == " " || s[index] == "\t" || s[index] == "=") break;
          index++;
        }
        return s.substring(start, index).toLowerCase();
      }

      String parseValue() {
        int start = index;
        while (!done()) {
          if (s[index] == " " || s[index] == "\t" || s[index] == ";") break;
          index++;
        }
        return s.substring(start, index).toLowerCase();
      }

      void expect(String expected) {
        if (done()) {
          throw new HttpException("Failed to parse header value [$s]");
        }
        if (s[index] != expected) {
          throw new HttpException("Failed to parse header value [$s]");
        }
        index++;
      }

      while (!done()) {
        skipWS();
        if (done()) return;
        String name = parseName();
        skipWS();
        expect("=");
        skipWS();
        String value = parseValue();
        _cookies.add(new _Cookie(name, value));
        skipWS();
        if (done()) return;
        expect(";");
      }
    }

    _cookies = new List<Cookie>();
    List<String> headerValues = headers["cookie"];
    if (headerValues != null) {
      headerValues.forEach((headerValue) => _parseCookieString(headerValue));
    }
    return _cookies;
  }

  InputStream get inputStream {
    if (_inputStream == null) {
      _inputStream = new _HttpInputStream(this);
    }
    return _inputStream;
  }

  String get protocolVersion => _protocolVersion;

  HttpSession session([init(HttpSession session)]) {
    if (_session != null) {
      // It's already mapped, use it.
      return _session;
    }
    // Create session, store it in connection, and return.
    var sessionManager = _httpConnection._server._sessionManager;
    return _session = sessionManager.createSession(init);
  }

  void _onRequestReceived(String method,
                          String uri,
                          String version,
                          _HttpHeaders headers) {
    _method = method;
    _uri = uri;
    _parseRequestUri(uri);
    _headers = headers;
    if (_httpConnection._server._sessionManagerInstance != null) {
      // Map to session if exists.
      var sessionId = cookies.reduce(null, (last, cookie) {
        if (last != null) return last;
        return cookie.name.toUpperCase() == _DART_SESSION_ID ?
            cookie.value : null;
      });
      if (sessionId != null) {
        var sessionManager = _httpConnection._server._sessionManager;
        _session = sessionManager.getSession(sessionId);
        if (_session != null) {
          _session._markSeen();
        }
      }
    }

    // Prepare for receiving data.
    _buffer = new _BufferList();
  }

  void _onDataReceived(List<int> data) {
    _buffer.add(data);
    if (_inputStream != null) _inputStream._dataReceived();
  }

  void _onDataEnd() {
    if (_inputStream != null) {
      _inputStream._closeReceived();
    } else {
      inputStream._streamMarkedClosed = true;
    }
  }

  // Escaped characters in uri are expected to have been parsed.
  void _parseRequestUri(String uri) {
    int position;
    position = uri.indexOf("?", 0);
    if (position == -1) {
      _path = _HttpUtils.decodeUrlEncodedString(_uri);
      _queryString = null;
      _queryParameters = new Map();
    } else {
      _path = _HttpUtils.decodeUrlEncodedString(_uri.substring(0, position));
      _queryString = _uri.substring(position + 1);
      _queryParameters = _HttpUtils.splitQueryString(_queryString);
    }
  }

  // Delegate functions for the HttpInputStream implementation.
  int _streamAvailable() {
    return _buffer.length;
  }

  List<int> _streamRead(int bytesToRead) {
    return _buffer.readBytes(bytesToRead);
  }

  int _streamReadInto(List<int> buffer, int offset, int len) {
    List<int> data = _buffer.readBytes(len);
    buffer.setRange(offset, data.length, data);
  }

  void _streamSetErrorHandler(callback(e)) {
    _streamErrorHandler = callback;
  }

  String _method;
  String _uri;
  String _path;
  String _queryString;
  Map<String, String> _queryParameters;
  _HttpInputStream _inputStream;
  _BufferList _buffer;
  Function _streamErrorHandler;
  _HttpSession _session;
}


// HTTP response object for sending a HTTP response.
class _HttpResponse extends _HttpRequestResponseBase implements HttpResponse {
  _HttpResponse(_HttpConnection httpConnection)
      : super(httpConnection),
        _statusCode = HttpStatus.OK {
    _headers = new _HttpHeaders();
  }

  void set contentLength(int contentLength) {
    if (_state >= _HttpRequestResponseBase.HEADER_SENT) {
      throw new HttpException("Header already sent");
    }
    _headers.contentLength = contentLength;
  }

  int get statusCode => _statusCode;
  void set statusCode(int statusCode) {
    if (_outputStream != null) throw new HttpException("Header already sent");
    _statusCode = statusCode;
  }

  String get reasonPhrase => _findReasonPhrase(_statusCode);
  void set reasonPhrase(String reasonPhrase) {
    if (_outputStream != null) throw new HttpException("Header already sent");
    _reasonPhrase = reasonPhrase;
  }

  List<Cookie> get cookies {
    if (_cookies == null) _cookies = new List<Cookie>();
    return _cookies;
  }

  OutputStream get outputStream {
    if (_state >= _HttpRequestResponseBase.DONE) {
      throw new HttpException("Response closed");
    }
    if (_outputStream == null) {
      _outputStream = new _HttpOutputStream(this);
    }
    return _outputStream;
  }

  DetachedSocket detachSocket() {
    if (_state >= _HttpRequestResponseBase.DONE) {
      throw new HttpException("Response closed");
    }
    // Ensure that headers are written.
    if (_state == _HttpRequestResponseBase.START) {
      _writeHeader();
    }
    _state = _HttpRequestResponseBase.UPGRADED;
    // Ensure that any trailing data is written.
    _writeDone();
    // Indicate to the connection that the response handling is done.
    return _httpConnection._detachSocket();
  }

  // Delegate functions for the HttpOutputStream implementation.
  bool _streamWrite(List<int> buffer, bool copyBuffer) {
    if (_done) throw new HttpException("Response closed");
    return _write(buffer, copyBuffer);
  }

  bool _streamWriteFrom(List<int> buffer, int offset, int len) {
    if (_done) throw new HttpException("Response closed");
    return _writeList(buffer, offset, len);
  }

  void _streamFlush() {
    _httpConnection._flush();
  }

  void _streamClose() {
    _ensureHeadersSent();
    _state = _HttpRequestResponseBase.DONE;
    // Stop tracking no pending write events.
    _httpConnection._onNoPendingWrites = null;
    // Ensure that any trailing data is written.
    _writeDone();
    // Indicate to the connection that the response handling is done.
    _httpConnection._responseClosed();
    if (_streamClosedHandler != null) {
      new Timer(0, (_) => _streamClosedHandler());
    }
  }

  void _streamSetNoPendingWriteHandler(callback()) {
    if (_state != _HttpRequestResponseBase.DONE) {
      _httpConnection._onNoPendingWrites = callback;
    }
  }

  void _streamSetClosedHandler(callback()) {
    _streamClosedHandler = callback;
  }

  void _streamSetErrorHandler(callback(e)) {
    _streamErrorHandler = callback;
  }

  String _findReasonPhrase(int statusCode) {
    if (_reasonPhrase != null) {
      return _reasonPhrase;
    }

    switch (statusCode) {
      case HttpStatus.CONTINUE: return "Continue";
      case HttpStatus.SWITCHING_PROTOCOLS: return "Switching Protocols";
      case HttpStatus.OK: return "OK";
      case HttpStatus.CREATED: return "Created";
      case HttpStatus.ACCEPTED: return "Accepted";
      case HttpStatus.NON_AUTHORITATIVE_INFORMATION:
        return "Non-Authoritative Information";
      case HttpStatus.NO_CONTENT: return "No Content";
      case HttpStatus.RESET_CONTENT: return "Reset Content";
      case HttpStatus.PARTIAL_CONTENT: return "Partial Content";
      case HttpStatus.MULTIPLE_CHOICES: return "Multiple Choices";
      case HttpStatus.MOVED_PERMANENTLY: return "Moved Permanently";
      case HttpStatus.FOUND: return "Found";
      case HttpStatus.SEE_OTHER: return "See Other";
      case HttpStatus.NOT_MODIFIED: return "Not Modified";
      case HttpStatus.USE_PROXY: return "Use Proxy";
      case HttpStatus.TEMPORARY_REDIRECT: return "Temporary Redirect";
      case HttpStatus.BAD_REQUEST: return "Bad Request";
      case HttpStatus.UNAUTHORIZED: return "Unauthorized";
      case HttpStatus.PAYMENT_REQUIRED: return "Payment Required";
      case HttpStatus.FORBIDDEN: return "Forbidden";
      case HttpStatus.NOT_FOUND: return "Not Found";
      case HttpStatus.METHOD_NOT_ALLOWED: return "Method Not Allowed";
      case HttpStatus.NOT_ACCEPTABLE: return "Not Acceptable";
      case HttpStatus.PROXY_AUTHENTICATION_REQUIRED:
        return "Proxy Authentication Required";
      case HttpStatus.REQUEST_TIMEOUT: return "Request Time-out";
      case HttpStatus.CONFLICT: return "Conflict";
      case HttpStatus.GONE: return "Gone";
      case HttpStatus.LENGTH_REQUIRED: return "Length Required";
      case HttpStatus.PRECONDITION_FAILED: return "Precondition Failed";
      case HttpStatus.REQUEST_ENTITY_TOO_LARGE:
        return "Request Entity Too Large";
      case HttpStatus.REQUEST_URI_TOO_LONG: return "Request-URI Too Large";
      case HttpStatus.UNSUPPORTED_MEDIA_TYPE: return "Unsupported Media Type";
      case HttpStatus.REQUESTED_RANGE_NOT_SATISFIABLE:
        return "Requested range not satisfiable";
      case HttpStatus.EXPECTATION_FAILED: return "Expectation Failed";
      case HttpStatus.INTERNAL_SERVER_ERROR: return "Internal Server Error";
      case HttpStatus.NOT_IMPLEMENTED: return "Not Implemented";
      case HttpStatus.BAD_GATEWAY: return "Bad Gateway";
      case HttpStatus.SERVICE_UNAVAILABLE: return "Service Unavailable";
      case HttpStatus.GATEWAY_TIMEOUT: return "Gateway Time-out";
      case HttpStatus.HTTP_VERSION_NOT_SUPPORTED:
        return "Http Version not supported";
      default: return "Status $statusCode";
    }
  }

  bool _writeHeader() {
    List<int> data;

    // Write status line.
    if (_protocolVersion == "1.1") {
      _httpConnection._write(_Const.HTTP11);
    } else {
      _httpConnection._write(_Const.HTTP10);
    }
    _writeSP();
    data = _statusCode.toString().charCodes;
    _httpConnection._write(data);
    _writeSP();
    data = reasonPhrase.charCodes;
    _httpConnection._write(data);
    _writeCRLF();

    var session = _httpConnection._request._session;
    if (session != null && !session._destroyed) {
      // Make sure we only send the current session id.
      bool found = false;
      for (int i = 0; i < cookies.length; i++) {
        if (cookies[i].name.toUpperCase() == _DART_SESSION_ID) {
          cookies[i].value = session.id;
          cookies[i].httpOnly = true;
          found = true;
          break;
        }
      }
      if (!found) {
        cookies.add(new Cookie(_DART_SESSION_ID, session.id)..httpOnly = true);
      }
    }
    // Add all the cookies set to the headers.
    if (_cookies != null) {
      _cookies.forEach((cookie) {
        _headers.add("set-cookie", cookie);
      });
    }

    // Write headers.
    _headers._finalize(_protocolVersion);
    bool allWritten = _writeHeaders();
    _state = _HttpRequestResponseBase.HEADER_SENT;
    return allWritten;
  }

  int _statusCode;  // Response status code.
  String _reasonPhrase;  // Response reason phrase.
  _HttpOutputStream _outputStream;
  Function _streamClosedHandler;
  Function _streamErrorHandler;
}


class _HttpInputStream extends _BaseDataInputStream implements InputStream {
  _HttpInputStream(_HttpRequestResponseBase this._requestOrResponse) {
    _checkScheduleCallbacks();
  }

  int available() {
    return _requestOrResponse._streamAvailable();
  }

  void pipe(OutputStream output, {bool close: true}) {
    _pipe(this, output, close: close);
  }

  List<int> _read(int bytesToRead) {
    List<int> result = _requestOrResponse._streamRead(bytesToRead);
    _checkScheduleCallbacks();
    return result;
  }

  void set onError(void callback(e)) {
    _requestOrResponse._streamSetErrorHandler(callback);
  }

  int _readInto(List<int> buffer, int offset, int len) {
    int result = _requestOrResponse._streamReadInto(buffer, offset, len);
    _checkScheduleCallbacks();
    return result;
  }

  void _close() {
    // TODO(sgjesse): Handle this.
  }

  void _dataReceived() {
    super._dataReceived();
  }

  _HttpRequestResponseBase _requestOrResponse;
}


class _HttpOutputStream extends _BaseOutputStream implements OutputStream {
  _HttpOutputStream(_HttpRequestResponseBase this._requestOrResponse);

  bool write(List<int> buffer, [bool copyBuffer = true]) {
    return _requestOrResponse._streamWrite(buffer, copyBuffer);
  }

  bool writeFrom(List<int> buffer, [int offset = 0, int len]) {
    if (offset < 0 || offset >= buffer.length) throw new ArgumentError();
    len = len != null ? len : buffer.length - offset;
    if (len < 0) throw new ArgumentError();
    return _requestOrResponse._streamWriteFrom(buffer, offset, len);
  }

  void flush() {
    _requestOrResponse._streamFlush();
  }

  void close() {
    _requestOrResponse._streamClose();
  }

  bool get closed => _requestOrResponse._done;

  void destroy() {
    throw "Not implemented";
  }

  void set onNoPendingWrites(void callback()) {
    _requestOrResponse._streamSetNoPendingWriteHandler(callback);
  }

  void set onClosed(void callback()) {
    _requestOrResponse._streamSetClosedHandler(callback);
  }

  void set onError(void callback(e)) {
    _requestOrResponse._streamSetErrorHandler(callback);
  }

  _HttpRequestResponseBase _requestOrResponse;
}


abstract class _HttpConnectionBase {
  static const int IDLE = 0;
  static const int ACTIVE = 1;
  static const int CLOSING = 2;
  static const int REQUEST_DONE = 4;
  static const int RESPONSE_DONE = 8;
  static const int ALL_DONE = REQUEST_DONE | RESPONSE_DONE;
  static const int READ_CLOSED = 16;
  static const int WRITE_CLOSED = 32;
  static const int FULLY_CLOSED = READ_CLOSED | WRITE_CLOSED;

  _HttpConnectionBase() : hashCode = _nextHashCode {
    _nextHashCode = (_nextHashCode + 1) & 0xFFFFFFF;
  }

  bool get _isIdle => (_state & ACTIVE) == 0;
  bool get _isActive => (_state & ACTIVE) == ACTIVE;
  bool get _isClosing => (_state & CLOSING) == CLOSING;
  bool get _isRequestDone => (_state & REQUEST_DONE) == REQUEST_DONE;
  bool get _isResponseDone => (_state & RESPONSE_DONE) == RESPONSE_DONE;
  bool get _isAllDone => (_state & ALL_DONE) == ALL_DONE;
  bool get _isReadClosed => (_state & READ_CLOSED) == READ_CLOSED;
  bool get _isWriteClosed => (_state & WRITE_CLOSED) == WRITE_CLOSED;
  bool get _isFullyClosed => (_state & FULLY_CLOSED) == FULLY_CLOSED;

  void _connectionEstablished(Socket socket) {
    _socket = socket;
    // Register handlers for socket events. All socket events are
    // passed to the HTTP parser.
    _socket.onData = () {
      List<int> buffer = _socket.read();
      if (buffer != null) {
        _httpParser.streamData(buffer);
      }
    };
    _socket.onClosed = _httpParser.streamDone;
    _socket.onError = _httpParser.streamError;
    _socket.outputStream.onError = _httpParser.streamError;
  }

  bool _write(List<int> data, [bool copyBuffer = false]);
  bool _writeFrom(List<int> buffer, [int offset, int len]);
  bool _flush();
  bool _close();
  bool _destroy();
  DetachedSocket _detachSocket();

  HttpConnectionInfo get connectionInfo {
    if (_socket == null) return null;
    try {
      _HttpConnectionInfo info = new _HttpConnectionInfo();
      info.remoteHost = _socket.remoteHost;
      info.remotePort = _socket.remotePort;
      info.localPort = _socket.port;
      return info;
    } catch (e) { }
    return null;
  }

  void set _onNoPendingWrites(void callback()) {
    _socket.outputStream.onNoPendingWrites = callback;
  }

  int _state = IDLE;

  Socket _socket;
  _HttpParser _httpParser;

  // Callbacks.
  Function onDetach;
  Function onClosed;

  // Hash code for HTTP connection. Currently this is just a counter.
  final int hashCode;
  static int _nextHashCode = 0;
}


// HTTP server connection over a socket.
class _HttpConnection extends _HttpConnectionBase {
  _HttpConnection(HttpServer this._server) {
    _httpParser = new _HttpParser.requestParser();
    // Register HTTP parser callbacks.
    _httpParser.requestStart = _onRequestReceived;
    _httpParser.dataReceived = _onDataReceived;
    _httpParser.dataEnd = _onDataEnd;
    _httpParser.error = _onError;
    _httpParser.closed = _onClosed;
    _httpParser.responseStart = (statusCode, reasonPhrase, version) {
      assert(false);
    };
  }

  void _bufferData(List<int> data, [bool copyBuffer = false]) {
    if (_buffer == null) _buffer = new _BufferList();
    if (copyBuffer) data = data.getRange(0, data.length);
    _buffer.add(data);
  }

  void _writeBufferedResponse() {
    if (_buffer != null) {
      while (!_buffer.isEmpty) {
        var data = _buffer.first;
        _socket.outputStream.write(data, false);
        _buffer.removeBytes(data.length);
      }
      _buffer = null;
    }
  }

  bool _write(List<int> data, [bool copyBuffer = false]) {
    if (_isRequestDone || !_hasBody || _httpParser.upgrade) {
      return _socket.outputStream.write(data, copyBuffer);
    } else {
      _bufferData(data, copyBuffer);
      return false;
    }
  }

  bool _writeFrom(List<int> data, [int offset, int len]) {
    if (_isRequestDone || !_hasBody || _httpParser.upgrade) {
      return _socket.outputStream.writeFrom(data, offset, len);
    } else {
      if (offset == null) offset = 0;
      if (len == null) len = buffer.length - offset;
      _bufferData(data.getRange(offset, len), false);
      return false;
    }
  }

  bool _flush() {
    _socket.outputStream.flush();
  }

  bool _close() {
    _socket.outputStream.close();
  }

  bool _destroy() {
    _socket.close();
  }

  void _onClosed() {
    _state |= _HttpConnectionBase.READ_CLOSED;
    _checkDone();
  }

  DetachedSocket _detachSocket() {
    _socket.onData = null;
    _socket.onClosed = null;
    _socket.onError = null;
    _socket.outputStream.onNoPendingWrites = null;
    _writeBufferedResponse();
    Socket socket = _socket;
    _socket = null;
    if (onDetach != null) onDetach();
    return new _DetachedSocket(socket, _httpParser.readUnparsedData());
  }

  void _onError(e) {
    // Don't report errors for a request parser when HTTP parser is in
    // idle state. Clients can close the connection and cause a
    // connection reset by peer error which is OK.
    _onClosed();
    if (_state == _HttpConnectionBase.IDLE) return;

    // Propagate the error to the streams.
    if (_request != null &&
        !_isRequestDone &&
        _request._streamErrorHandler != null) {
      _request._streamErrorHandler(e);
    } else if (_response != null &&
        !_isResponseDone &&
        _response._streamErrorHandler != null) {
      _response._streamErrorHandler(e);
    } else {
      onError(e);
    }
    if (_socket != null) _socket.close();
  }

  void _onRequestReceived(String method,
                          String uri,
                          String version,
                          _HttpHeaders headers,
                          bool hasBody) {
    _state = _HttpConnectionBase.ACTIVE;
    // Create new request and response objects for this request.
    _request = new _HttpRequest(this);
    _response = new _HttpResponse(this);
    _request._onRequestReceived(method, uri, version, headers);
    _request._protocolVersion = version;
    _response._protocolVersion = version;
    _response._headResponse = method == "HEAD";
    _response.persistentConnection = _httpParser.persistentConnection;
    _hasBody = hasBody;
    if (onRequestReceived != null) {
      onRequestReceived(_request, _response);
    }
    _checkDone();
  }

  void _onDataReceived(List<int> data) {
    _request._onDataReceived(data);
    _checkDone();
  }

  void _checkDone() {
    if (_isReadClosed) {
      // If the client closes the conversation is ended.
      _server._closeQueue.add(this);
    } else if (_isAllDone) {
      // If we are done writing the response, and the connection is
      // not persistent, we must close. Also if using HTTP 1.0 and the
      // content length was not known we must close to indicate end of
      // body.
      bool close =
          !_response.persistentConnection ||
          (_response._protocolVersion == "1.0" && _response.contentLength < 0);
      _request = null;
      _response = null;
      if (close) {
        _httpParser.cancel();
        _server._closeQueue.add(this);
      } else {
        _state = _HttpConnectionBase.IDLE;
      }
    } else if (_isResponseDone && _hasBody) {
      // If the response is closed before the request is fully read
      // close this connection. If there is buffered output
      // (e.g. error response for invalid request where the server did
      // not care to read the request body) this is send.
      assert(!_isRequestDone);
      _writeBufferedResponse();
      _httpParser.cancel();
      _server._closeQueue.add(this);
    }
  }

  void _onDataEnd(bool close) {
    // Start sending queued response if any.
    _state |= _HttpConnectionBase.REQUEST_DONE;
    _writeBufferedResponse();
    _request._onDataEnd();
    _checkDone();
  }

  void _responseClosed() {
    _state |= _HttpConnectionBase.RESPONSE_DONE;
  }

  HttpServer _server;
  HttpRequest _request;
  HttpResponse _response;
  bool _hasBody = false;

  // Buffer for data written before full response has been processed.
  _BufferList _buffer;

  // Callbacks.
  Function onRequestReceived;
  Function onError;
}


class _RequestHandlerRegistration {
  _RequestHandlerRegistration(Function this._matcher, Function this._handler);
  Function _matcher;
  Function _handler;
}

// HTTP server waiting for socket connections. The connections are
// managed by the server and as requests are received the request.
// HTTPS connections are also supported, if the _HttpServer.httpsServer
// constructor is used and a certificate name is provided in listen,
// or a SecureServerSocket is provided to listenOn.
class _HttpServer implements HttpServer, HttpsServer {
  _HttpServer() : this._internal(isSecure: false);

  _HttpServer.httpsServer() : this._internal(isSecure: true);

  _HttpServer._internal({ bool isSecure: false })
      : _secure = isSecure,
        _connections = new Set<_HttpConnection>(),
        _handlers = new List<_RequestHandlerRegistration>(),
        _closeQueue = new _CloseQueue();

  void listen(String host,
              int port,
              {int backlog: 128,
               String certificate_name,
               bool requestClientCertificate: false}) {
    if (_secure) {
      listenOn(new SecureServerSocket(
          host,
          port,
          backlog,
          certificate_name,
          requestClientCertificate: requestClientCertificate));
    } else {
      listenOn(new ServerSocket(host, port, backlog));
    }
    _closeServer = true;
  }

  void listenOn(ServerSocket serverSocket) {
    if (_secure && serverSocket is! SecureServerSocket) {
        throw new HttpException(
            'HttpsServer.listenOn was called with non-secure server socket');
    } else if (!_secure && serverSocket is SecureServerSocket) {
        throw new HttpException(
            'HttpServer.listenOn was called with a secure server socket');
    }
    void onConnection(Socket socket) {
      // Accept the client connection.
      _HttpConnection connection = new _HttpConnection(this);
      connection._connectionEstablished(socket);
      _connections.add(connection);
      connection.onRequestReceived = _handleRequest;
      connection.onClosed = () => _connections.remove(connection);
      connection.onDetach = () => _connections.remove(connection);
      connection.onError = (e) {
        _connections.remove(connection);
        if (_onError != null) {
          _onError(e);
        } else {
          throw(e);
        }
      };
    }
    serverSocket.onConnection = onConnection;
    _server = serverSocket;
    _closeServer = false;
  }

  addRequestHandler(bool matcher(HttpRequest request),
                    void handler(HttpRequest request, HttpResponse response)) {
    _handlers.add(new _RequestHandlerRegistration(matcher, handler));
  }

  void set defaultRequestHandler(
      void handler(HttpRequest request, HttpResponse response)) {
    _defaultHandler = handler;
  }

  void close() {
    _closeQueue.shutdown();
    if (_sessionManagerInstance != null) {
      _sessionManagerInstance.close();
      _sessionManagerInstance = null;
    }
    if (_server != null && _closeServer) {
      _server.close();
    }
    _server = null;
    for (_HttpConnection connection in _connections) {
      connection._destroy();
    }
    _connections.clear();
  }

  int get port {
    if (_server == null) {
      throw new HttpException("The HttpServer is not listening on a port.");
    }
    return _server.port;
  }

  void set onError(void callback(e)) {
    _onError = callback;
  }

  set sessionTimeout(int timeout) {
    _sessionManager.sessionTimeout = timeout;
  }

  void _handleRequest(HttpRequest request, HttpResponse response) {
    for (int i = 0; i < _handlers.length; i++) {
      if (_handlers[i]._matcher(request)) {
        Function handler = _handlers[i]._handler;
        try {
          handler(request, response);
        } catch (e) {
          if (_onError != null) {
            _onError(e);
          } else {
            throw e;
          }
        }
        return;
      }
    }

    if (_defaultHandler != null) {
      _defaultHandler(request, response);
    } else {
      response.statusCode = HttpStatus.NOT_FOUND;
      response.contentLength = 0;
      response.outputStream.close();
    }
  }

  _HttpSessionManager get _sessionManager {
    // Lazy init.
    if (_sessionManagerInstance == null) {
      _sessionManagerInstance = new _HttpSessionManager();
    }
    return _sessionManagerInstance;
  }

  HttpConnectionsInfo connectionsInfo() {
    HttpConnectionsInfo result = new HttpConnectionsInfo();
    result.total = _connections.length;
    _connections.forEach((_HttpConnection conn) {
      if (conn._isActive) {
        result.active++;
      } else if (conn._isIdle) {
        result.idle++;
      } else {
        assert(result._isClosing);
        result.closing++;
      }
    });
    return result;
  }

  ServerSocket _server;  // The server listen socket.
  bool _closeServer = false;
  bool _secure;
  Set<_HttpConnection> _connections;  // Set of currently connected clients.
  List<_RequestHandlerRegistration> _handlers;
  Object _defaultHandler;
  Function _onError;
  _CloseQueue _closeQueue;
  _HttpSessionManager _sessionManagerInstance;
}


class _HttpClientRequest
    extends _HttpRequestResponseBase implements HttpClientRequest {
  _HttpClientRequest(String this._method,
                     Uri this._uri,
                     _HttpClientConnection connection)
      : super(connection) {
    _headers = new _HttpHeaders();
    _connection = connection;
    // Default GET and HEAD requests to have no content.
    if (_method == "GET" || _method == "HEAD") {
      contentLength = 0;
    }
  }

  void set contentLength(int contentLength) {
    if (_state >= _HttpRequestResponseBase.HEADER_SENT) {
      throw new HttpException("Header already sent");
    }
    _headers.contentLength = contentLength;
  }

  List<Cookie> get cookies {
    if (_cookies == null) _cookies = new List<Cookie>();
    return _cookies;
  }

  OutputStream get outputStream {
    if (_done) throw new HttpException("Request closed");
    if (_outputStream == null) {
      _outputStream = new _HttpOutputStream(this);
    }
    return _outputStream;
  }

  // Delegate functions for the HttpOutputStream implementation.
  bool _streamWrite(List<int> buffer, bool copyBuffer) {
    if (_done) throw new HttpException("Request closed");
    _emptyBody = _emptyBody && buffer.length == 0;
    return _write(buffer, copyBuffer);
  }

  bool _streamWriteFrom(List<int> buffer, int offset, int len) {
    if (_done) throw new HttpException("Request closed");
    _emptyBody = _emptyBody && buffer.length == 0;
    return _writeList(buffer, offset, len);
  }

  void _streamFlush() {
    _httpConnection._flush();
  }

  void _streamClose() {
    _ensureHeadersSent();
    _state = _HttpRequestResponseBase.DONE;
    // Stop tracking no pending write events.
    _httpConnection._onNoPendingWrites = null;
    // Ensure that any trailing data is written.
    _writeDone();
    _connection._requestClosed();
    if (_streamClosedHandler != null) {
      new Timer(0, (_) => _streamClosedHandler());
    }
  }

  void _streamSetNoPendingWriteHandler(callback()) {
    if (_state != _HttpRequestResponseBase.DONE) {
      _httpConnection._onNoPendingWrites = callback;
    }
  }

  void _streamSetClosedHandler(callback()) {
    _streamClosedHandler = callback;
  }

  void _streamSetErrorHandler(callback(e)) {
    _streamErrorHandler = callback;
  }

  void _writeHeader() {
    List<int> data;

    // Write request line.
    data = _method.toString().charCodes;
    _httpConnection._write(data);
    _writeSP();
    // Send the path for direct connections and the whole URL for
    // proxy connections.
    if (!_connection._usingProxy) {
      String path = _uri.path;
      if (path.length == 0) path = "/";
      if (_uri.query != "") {
        if (_uri.fragment != "") {
          path = "${path}?${_uri.query}#${_uri.fragment}";
        } else {
          path = "${path}?${_uri.query}";
        }
      }
      data = path.charCodes;
    } else {
      data = _uri.toString().charCodes;
    }
    _httpConnection._write(data);
    _writeSP();
    _httpConnection._write(_Const.HTTP11);
    _writeCRLF();

    // Add the cookies to the headers.
    if (_cookies != null) {
      StringBuffer sb = new StringBuffer();
      for (int i = 0; i < _cookies.length; i++) {
        if (i > 0) sb.add("; ");
        sb.add(_cookies[i].name);
        sb.add("=");
        sb.add(_cookies[i].value);
      }
      _headers.add("cookie", sb.toString());
    }

    // Write headers.
    _headers._finalize("1.1");
    _writeHeaders();
    _state = _HttpRequestResponseBase.HEADER_SENT;
  }

  String _method;
  Uri _uri;
  _HttpClientConnection _connection;
  _HttpOutputStream _outputStream;
  Function _streamClosedHandler;
  Function _streamErrorHandler;
  bool _emptyBody = true;
}

class _HttpClientResponse
    extends _HttpRequestResponseBase
    implements HttpClientResponse {
  _HttpClientResponse(_HttpClientConnection connection)
      : super(connection) {
    _connection = connection;
  }

  int get statusCode => _statusCode;
  String get reasonPhrase => _reasonPhrase;

  bool get isRedirect {
    var method = _connection._request._method;
    if (method == "GET" || method == "HEAD") {
      return statusCode == HttpStatus.MOVED_PERMANENTLY ||
             statusCode == HttpStatus.FOUND ||
             statusCode == HttpStatus.SEE_OTHER ||
             statusCode == HttpStatus.TEMPORARY_REDIRECT;
    } else if (method == "POST") {
      return statusCode == HttpStatus.SEE_OTHER;
    }
    return false;
  }

  List<Cookie> get cookies {
    if (_cookies != null) return _cookies;
    _cookies = new List<Cookie>();
    List<String> values = _headers["set-cookie"];
    if (values != null) {
      values.forEach((value) {
        _cookies.add(new Cookie.fromSetCookieValue(value));
      });
    }
    return _cookies;
  }

  InputStream get inputStream {
    if (_inputStream == null) {
      _inputStream = new _HttpInputStream(this);
    }
    return _inputStream;
  }

  void _onResponseReceived(int statusCode,
                           String reasonPhrase,
                           String version,
                           _HttpHeaders headers,
                           bool hasBody) {
    _statusCode = statusCode;
    _reasonPhrase = reasonPhrase;
    _headers = headers;

    // Prepare for receiving data.
    _buffer = new _BufferList();
    if (isRedirect && _connection.followRedirects) {
      if (_connection._redirects == null ||
          _connection._redirects.length < _connection.maxRedirects) {
        // Check the location header.
        List<String> location = headers[HttpHeaders.LOCATION];
        if (location == null || location.length > 1) {
           throw new RedirectException("Invalid redirect",
                                       _connection._redirects);
        }
        // Check for redirect loop
        if (_connection._redirects != null) {
          Uri redirectUrl = Uri.parse(location[0]);
          for (int i = 0; i < _connection._redirects.length; i++) {
            if (_connection._redirects[i].location.toString() ==
                redirectUrl.toString()) {
              throw new RedirectLoopException(_connection._redirects);
            }
          }
        }
        // Drain body and redirect.
        inputStream.onData = inputStream.read;
        if (_statusCode == HttpStatus.SEE_OTHER &&
            _connection._method == "POST") {
          _connection.redirect("GET");
        } else {
          _connection.redirect();
        }
      } else {
        throw new RedirectLimitExceededException(_connection._redirects);
      }
    } else if (statusCode == HttpStatus.UNAUTHORIZED) {
      _handleUnauthorized();
    } else if (_connection._onResponse != null) {
      _connection._onResponse(this);
    }
  }

  void _handleUnauthorized() {

    void retryRequest(_Credentials cr) {
      if (cr != null) {
        // Drain body and retry.
        // TODO(sgjesse): Support digest.
        if (cr.scheme == _AuthenticationScheme.BASIC) {
          inputStream.onData = inputStream.read;
          _connection._retry();
          return;
        }
      }

      // Fall through to here to perform normal response handling if
      // there is no sensible authorization handling.
      if (_connection._onResponse != null) {
        _connection._onResponse(this);
      }
    }

    // Only try to authenticate if there is a challenge in the response.
    List<String> challenge = _headers[HttpHeaders.WWW_AUTHENTICATE];
    if (challenge != null && challenge.length == 1) {
      _HeaderValue header =
          new _HeaderValue.fromString(challenge[0], parameterSeparator: ",");
      _AuthenticationScheme scheme =
          new _AuthenticationScheme.fromString(header.value);
      String realm = header.parameters["realm"];

      // See if any credentials are available.
      _Credentials cr =
          _connection._client._findCredentials(
              _connection._request._uri, scheme);

      // Ask for more credentials if none found or the one found has
      // already been used. If it has already been used it must now be
      // invalid and is removed.
      if (cr == null || cr.used) {
        if (cr != null) {
          _connection._client._removeCredentials(cr);
        }
        cr = null;
        if (_connection._client._authenticate != null) {
          Future authComplete =
              _connection._client._authenticate(
                  _connection._request._uri, scheme.toString(), realm);
          authComplete.then((credsAvailable) {
            if (credsAvailable) {
              cr = _connection._client._findCredentials(
                  _connection._request._uri, scheme);
              retryRequest(cr);
            } else {
              if (_connection._onResponse != null) {
                _connection._onResponse(this);
              }
            }
          });
          return;
        }
      } else {
        // If credentials found prepare for retrying the request.
        retryRequest(cr);
        return;
      }
    }

    // Fall through to here to perform normal response handling if
    // there is no sensible authorization handling.
    if (_connection._onResponse != null) {
      _connection._onResponse(this);
    }
  }

  void _onDataReceived(List<int> data) {
    _buffer.add(data);
    if (_inputStream != null) _inputStream._dataReceived();
  }

  void _onDataEnd() {
    if (_inputStream != null) {
      _inputStream._closeReceived();
    } else {
      inputStream._streamMarkedClosed = true;
    }
  }

  // Delegate functions for the HttpInputStream implementation.
  int _streamAvailable() {
    return _buffer.length;
  }

  List<int> _streamRead(int bytesToRead) {
    return _buffer.readBytes(bytesToRead);
  }

  int _streamReadInto(List<int> buffer, int offset, int len) {
    List<int> data = _buffer.readBytes(len);
    buffer.setRange(offset, data.length, data);
    return data.length;
  }

  void _streamSetErrorHandler(callback(e)) {
    _streamErrorHandler = callback;
  }

  int _statusCode;
  String _reasonPhrase;

  _HttpClientConnection _connection;
  _HttpInputStream _inputStream;
  _BufferList _buffer;

  Function _streamErrorHandler;
}


class _HttpClientConnection
    extends _HttpConnectionBase implements HttpClientConnection {

  _HttpClientConnection(_HttpClient this._client) {
    _httpParser = new _HttpParser.responseParser();
  }

  bool _write(List<int> data, [bool copyBuffer = false]) {
    return _socket.outputStream.write(data, copyBuffer);
  }

  bool _writeFrom(List<int> data, [int offset, int len]) {
    return _socket.outputStream.writeFrom(data, offset, len);
  }

  bool _flush() {
    _socket.outputStream.flush();
  }

  bool _close() {
    _socket.outputStream.close();
  }

  bool _destroy() {
    _socket.close();
  }

  DetachedSocket _detachSocket() {
    _socket.onData = null;
    _socket.onClosed = null;
    _socket.onError = null;
    _socket.outputStream.onNoPendingWrites = null;
    Socket socket = _socket;
    _socket = null;
    if (onDetach != null) onDetach();
    return new _DetachedSocket(socket, _httpParser.readUnparsedData());
  }

  void _connectionEstablished(_SocketConnection socketConn) {
    super._connectionEstablished(socketConn._socket);
    _socketConn = socketConn;
    // Register HTTP parser callbacks.
    _httpParser.responseStart = _onResponseReceived;
    _httpParser.dataReceived = _onDataReceived;
    _httpParser.dataEnd = _onDataEnd;
    _httpParser.error = _onError;
    _httpParser.closed = _onClosed;
    _httpParser.requestStart = (method, uri, version) { assert(false); };
    _state = _HttpConnectionBase.ACTIVE;
  }

  void _checkSocketDone() {
    if (_isAllDone) {
      // If we are done writing the response, and either the server
      // has closed or the connection is not persistent, we must
      // close.
      if (_isReadClosed || !_response.persistentConnection) {
        this.onClosed = () {
          _client._closedSocketConnection(_socketConn);
        };
        _client._closeQueue.add(this);
      } else if (_socket != null) {
        _client._returnSocketConnection(_socketConn);
        _socket = null;
        _socketConn = null;
        assert(_pendingRedirect == null || _pendingRetry == null);
        if (_pendingRedirect != null) {
          _doRedirect(_pendingRedirect);
          _pendingRedirect = null;
        } else if (_pendingRetry != null) {
          _doRetry(_pendingRetry);
          _pendingRetry = null;
        }
      }
    }
  }

  void _requestClosed() {
    _state |= _HttpConnectionBase.REQUEST_DONE;
    _checkSocketDone();
  }

  HttpClientRequest open(String method, Uri uri) {
    _method = method;
    // Tell the HTTP parser the method it is expecting a response to.
    _httpParser.responseToMethod = method;
    // If the connection already have a request this is a retry of a
    // request. In this case the request object is reused to ensure
    // that the same headers are send.
    if (_request != null) {
      _request._method = method;
      _request._uri = uri;
      _request._headers._mutable = true;
      _request._state = _HttpRequestResponseBase.START;
    } else {
      _request = new _HttpClientRequest(method, uri, this);
    }
    _response = new _HttpClientResponse(this);
    return _request;
  }

  DetachedSocket detachSocket() {
    return _detachSocket();
  }

  void _onClosed() {
    _state |= _HttpConnectionBase.READ_CLOSED;
    _checkSocketDone();
  }

  void _onError(e) {
    // Cancel any pending data in the HTTP parser.
    _httpParser.cancel();
    if (_socketConn != null) {
      _client._closeSocketConnection(_socketConn);
    }

    // If it looks as if we got a bad connection from the connection
    // pool and the request can be retried do a retry.
    if (_socketConn != null && _socketConn._fromPool && _request._emptyBody) {
      String method = _request._method;
      Uri uri = _request._uri;
      _socketConn = null;

      // Retry the URL using the same connection instance.
      _httpParser.restart();
      _client._openUrl(method, uri, this);
    } else {
      // Report the error.
      if (_response != null && _response._streamErrorHandler != null) {
         _response._streamErrorHandler(e);
      } else if (_onErrorCallback != null) {
        _onErrorCallback(e);
      } else {
        throw e;
      }
    }
  }

  void _onResponseReceived(int statusCode,
                           String reasonPhrase,
                           String version,
                           _HttpHeaders headers,
                           bool hasBody) {
    _response._onResponseReceived(
        statusCode, reasonPhrase, version, headers, hasBody);
  }

  void _onDataReceived(List<int> data) {
    _response._onDataReceived(data);
  }

  void _onDataEnd(bool close) {
    _state |= _HttpConnectionBase.RESPONSE_DONE;
    _response._onDataEnd();
    _checkSocketDone();
  }

  void _onClientShutdown() {
    if (!_isResponseDone) {
      _onError(new HttpException("Client shutdown"));
    }
  }

  void set onRequest(void handler(HttpClientRequest request)) {
    _onRequest = handler;
  }

  void set onResponse(void handler(HttpClientResponse response)) {
    _onResponse = handler;
  }

  void set onError(void callback(e)) {
    _onErrorCallback = callback;
  }

  void _doRetry(_RedirectInfo retry) {
    assert(_socketConn == null);

    // Retry the URL using the same connection instance.
    _state = _HttpConnectionBase.IDLE;
    _client._openUrl(retry.method, retry.location, this);
  }

  void _retry() {
    var retry = new _RedirectInfo(_response.statusCode, _method, _request._uri);
    // The actual retry is postponed until both response and request
    // are done.
    if (_isAllDone) {
      _doRetry(retry);
    } else {
      // Prepare for retry.
      assert(_pendingRedirect == null);
      _pendingRetry = retry;
    }
  }

  void _doRedirect(_RedirectInfo redirect) {
    assert(_socketConn == null);

    if (_redirects == null) {
      _redirects = new List<_RedirectInfo>();
    }
    _redirects.add(redirect);
    _doRetry(redirect);
  }

  void redirect([String method, Uri url]) {
    if (method == null) method = _method;
    if (url == null) {
      url = Uri.parse(_response.headers.value(HttpHeaders.LOCATION));
    }
    // Always set the content length to 0 for redirects.
    var mutable = _request._headers._mutable;
    _request._headers._mutable = true;
    _request._headers.contentLength = 0;
    _request._headers._mutable = mutable;
    _request._bodyBytesWritten = 0;
    var redirect = new _RedirectInfo(_response.statusCode, method, url);
    // The actual redirect is postponed until both response and
    // request are done.
    assert(_pendingRetry == null);
    _pendingRedirect = redirect;
  }

  List<RedirectInfo> get redirects => _redirects;

  Function _onRequest;
  Function _onResponse;
  Function _onErrorCallback;

  _HttpClient _client;
  _SocketConnection _socketConn;
  HttpClientRequest _request;
  HttpClientResponse _response;
  String _method;
  bool _usingProxy;

  // Redirect handling
  bool followRedirects = true;
  int maxRedirects = 5;
  List<_RedirectInfo> _redirects;
  _RedirectInfo _pendingRedirect;
  _RedirectInfo _pendingRetry;

  // Callbacks.
  var requestReceived;
}


// Class for holding keep-alive sockets in the cache for the HTTP
// client together with the connection information.
class _SocketConnection {
  _SocketConnection(String this._host,
                    int this._port,
                    Socket this._socket);

  void _markReturned() {
    // Any activity on the socket while waiting in the pool will
    // invalidate the connection os that it is not reused.
    _socket.onData = _invalidate;
    _socket.onClosed = _invalidate;
    _socket.onError = (_) => _invalidate();
    _returnTime = new DateTime.now();
    _httpClientConnection = null;
  }

  void _markRetrieved() {
    _socket.onData = null;
    _socket.onClosed = null;
    _socket.onError = null;
    _httpClientConnection = null;
  }

  void _close() {
    _socket.onData = null;
    _socket.onClosed = null;
    _socket.onError = null;
    _httpClientConnection = null;
    _socket.close();
  }

  Duration _idleTime(DateTime now) => now.difference(_returnTime);

  bool get _fromPool => _returnTime != null;

  void _invalidate() {
    _valid = false;
    _close();
  }

  int get hashCode => _socket.hashCode;

  String _host;
  int _port;
  Socket _socket;
  DateTime _returnTime;
  bool _valid = true;
  HttpClientConnection _httpClientConnection;
}

class _ProxyConfiguration {
  static const String PROXY_PREFIX = "PROXY ";
  static const String DIRECT_PREFIX = "DIRECT";

  _ProxyConfiguration(String configuration) : proxies = new List<_Proxy>() {
    if (configuration == null) {
      throw new HttpException("Invalid proxy configuration $configuration");
    }
    List<String> list = configuration.split(";");
    list.forEach((String proxy) {
      proxy = proxy.trim();
      if (!proxy.isEmpty) {
        if (proxy.startsWith(PROXY_PREFIX)) {
          int colon = proxy.indexOf(":");
          if (colon == -1 || colon == 0 || colon == proxy.length - 1) {
            throw new HttpException(
                "Invalid proxy configuration $configuration");
          }
          // Skip the "PROXY " prefix.
          String host = proxy.substring(PROXY_PREFIX.length, colon).trim();
          String portString = proxy.substring(colon + 1).trim();
          int port;
          try {
            port = int.parse(portString);
          } on FormatException catch (e) {
            throw new HttpException(
                "Invalid proxy configuration $configuration, "
                "invalid port '$portString'");
          }
          proxies.add(new _Proxy(host, port));
        } else if (proxy.trim() == DIRECT_PREFIX) {
          proxies.add(new _Proxy.direct());
        } else {
          throw new HttpException("Invalid proxy configuration $configuration");
        }
      }
    });
  }

  const _ProxyConfiguration.direct()
      : proxies = const [const _Proxy.direct()];

  final List<_Proxy> proxies;
}

class _Proxy {
  const _Proxy(this.host, this.port) : isDirect = false;
  const _Proxy.direct() : host = null, port = null, isDirect = true;

  final String host;
  final int port;
  final bool isDirect;
}

class _HttpClient implements HttpClient {
  static const int DEFAULT_EVICTION_TIMEOUT = 60000;

  _HttpClient() : _openSockets = new Map(),
                  _activeSockets = new Set(),
                  _closeQueue = new _CloseQueue(),
                  credentials = new List<_Credentials>(),
                  _shutdown = false;

  HttpClientConnection open(
      String method, String host, int port, String path) {
    // TODO(sgjesse): The path set here can contain both query and
    // fragment. They should be cracked and set correctly.
    return _open(method, new Uri.fromComponents(
        scheme: "http", domain: host, port: port, path: path));
  }

  HttpClientConnection _open(String method,
                             Uri uri,
                             [_HttpClientConnection connection]) {
    if (_shutdown) throw new HttpException("HttpClient shutdown");
    if (method == null || uri.domain.isEmpty) {
      throw new ArgumentError(null);
    }
    return _prepareHttpClientConnection(method, uri, connection);
  }

  HttpClientConnection openUrl(String method, Uri url) {
    return _openUrl(method, url);
  }

  HttpClientConnection _openUrl(String method,
                                Uri url,
                                [_HttpClientConnection connection]) {
    if (url.scheme != "http" && url.scheme != "https") {
      throw new HttpException("Unsupported URL scheme ${url.scheme}");
    }
    return _open(method, url, connection);
  }

  HttpClientConnection get(String host, int port, String path) {
    return open("GET", host, port, path);
  }

  HttpClientConnection getUrl(Uri url) => _openUrl("GET", url);

  HttpClientConnection post(String host, int port, String path) {
    return open("POST", host, port, path);
  }

  HttpClientConnection postUrl(Uri url) => _openUrl("POST", url);

  set authenticate(Future<bool> f(Uri url, String scheme, String realm)) {
    _authenticate = f;
  }

  void addCredentials(
      Uri url, String realm, HttpClientCredentials cr) {
    credentials.add(new _Credentials(url, realm, cr));
  }

  set sendClientCertificate(bool send) => _sendClientCertificate = send;

  set clientCertificate(String nickname) => _clientCertificate = nickname;

  set findProxy(String f(Uri uri)) => _findProxy = f;

  void shutdown({bool force: false}) {
    if (force) _closeQueue.shutdown();
    _openSockets.forEach((String key, Queue<_SocketConnection> connections) {
      while (!connections.isEmpty) {
        _SocketConnection socketConn = connections.removeFirst();
        socketConn._socket.close();
      }
    });
    if (force) {
      _activeSockets.forEach((_SocketConnection socketConn) {
        socketConn._httpClientConnection._onClientShutdown();
        socketConn._close();
      });
    }
    if (_evictionTimer != null) _cancelEvictionTimer();
    _shutdown = true;
  }

  void _cancelEvictionTimer() {
    _evictionTimer.cancel();
    _evictionTimer = null;
  }

  String _connectionKey(String host, int port) {
    return "$host:$port";
  }

  HttpClientConnection _prepareHttpClientConnection(
      String method,
      Uri url,
      [_HttpClientConnection connection]) {

    void _establishConnection(String host,
                              int port,
                              _ProxyConfiguration proxyConfiguration,
                              int proxyIndex,
                              bool reusedConnection,
                              bool secure) {

      void _connectionOpened(_SocketConnection socketConn,
                             _HttpClientConnection connection,
                             bool usingProxy) {
        socketConn._httpClientConnection = connection;
        connection._usingProxy = usingProxy;
        connection._connectionEstablished(socketConn);
        HttpClientRequest request = connection.open(method, url);
        request.headers.host = host;
        request.headers.port = port;
        if (url.userInfo != null && !url.userInfo.isEmpty) {
          // If the URL contains user information use that for basic
          // authorization
          _UTF8Encoder encoder = new _UTF8Encoder();
          String auth =
              CryptoUtils.bytesToBase64(encoder.encodeString(url.userInfo));
          request.headers.set(HttpHeaders.AUTHORIZATION, "Basic $auth");
        } else {
          // Look for credentials.
          _Credentials cr = _findCredentials(url);
          if (cr != null) {
            cr.authorize(request);
          }
        }
        // A reused connection is indicating either redirect or retry
        // where the onRequest callback should not be issued again.
        if (connection._onRequest != null && !reusedConnection) {
          connection._onRequest(request);
        } else {
          request.outputStream.close();
        }
      }

      assert(proxyIndex < proxyConfiguration.proxies.length);

      // Determine the actual host to connect to.
      String connectHost;
      int connectPort;
      _Proxy proxy = proxyConfiguration.proxies[proxyIndex];
      if (proxy.isDirect) {
        connectHost = host;
        connectPort = port;
      } else {
        connectHost = proxy.host;
        connectPort = proxy.port;
      }

      // If there are active connections for this key get the first one
      // otherwise create a new one.
      String key = _connectionKey(connectHost, connectPort);
      Queue socketConnections = _openSockets[key];
      // Remove active connections that are not valid any more or of
      // the wrong type (HTTP or HTTPS).
      if (socketConnections != null) {
        while (!socketConnections.isEmpty) {
          if (socketConnections.first._valid) {
            // If socket has the same properties, exit loop with found socket.
            var socket = socketConnections.first._socket;
            if (!secure && socket is! SecureSocket) break;
            if (secure && socket is SecureSocket &&
                 _sendClientCertificate == socket.sendClientCertificate &&
                 _clientCertificate == socket.certificateName) break;
          }
          socketConnections.removeFirst()._close();
        }
      }
      if (socketConnections == null || socketConnections.isEmpty) {
        Socket socket = secure ?
            new SecureSocket(connectHost,
                             connectPort,
                             sendClientCertificate: _sendClientCertificate,
                             certificateName: _clientCertificate) :
            new Socket(connectHost, connectPort);
        // Until the connection is established handle connection errors
        // here as the HttpClientConnection object is not yet associated
        // with the socket.
        socket.onError = (e) {
          proxyIndex++;
          if (proxyIndex < proxyConfiguration.proxies.length) {
            // Try the next proxy in the list.
            _establishConnection(
                host, port, proxyConfiguration, proxyIndex, false, secure);
          } else {
            // Report the error through the HttpClientConnection object to
            // the client.
            connection._onError(e);
          }
        };
        socket.onConnect = () {
          // When the connection is established, clear the error
          // callback as it will now be handled by the
          // HttpClientConnection object which will be associated with
          // the connected socket.
          socket.onError = null;
          _SocketConnection socketConn =
              new _SocketConnection(connectHost, connectPort, socket);
          _activeSockets.add(socketConn);
          _connectionOpened(socketConn, connection, !proxy.isDirect);
        };
      } else {
        _SocketConnection socketConn = socketConnections.removeFirst();
        socketConn._markRetrieved();
        _activeSockets.add(socketConn);
        new Timer(0, (ignored) =>
                  _connectionOpened(socketConn, connection, !proxy.isDirect));

        // Get rid of eviction timer if there are no more active connections.
        if (socketConnections.isEmpty) _openSockets.remove(key);
        if (_openSockets.isEmpty) _cancelEvictionTimer();
      }
    }

    // Find out if we want a secure socket.
    bool is_secure = (url.scheme == "https");

    // Find the TCP host and port.
    String host = url.domain;
    int port = url.port;
    if (port == 0) {
      port = is_secure ?
          HttpClient.DEFAULT_HTTPS_PORT :
          HttpClient.DEFAULT_HTTP_PORT;
    }
    // Create a new connection object if we are not re-using an existing one.
    var reusedConnection = false;
    if (connection == null) {
      connection = new _HttpClientConnection(this);
    } else {
      reusedConnection = true;
    }
    connection.onDetach = () => _activeSockets.remove(connection._socketConn);

    // Check to see if a proxy server should be used for this connection.
    _ProxyConfiguration proxyConfiguration = const _ProxyConfiguration.direct();
    if (_findProxy != null) {
      // TODO(sgjesse): Keep a map of these as normally only a few
      // configuration strings will be used.
      proxyConfiguration = new _ProxyConfiguration(_findProxy(url));
    }

    // Establish the connection starting with the first proxy configured.
    _establishConnection(host,
                         port,
                         proxyConfiguration,
                         0,
                         reusedConnection,
                         is_secure);

    return connection;
  }

  void _returnSocketConnection(_SocketConnection socketConn) {
    // If the HTTP client is being shutdown don't return the connection.
    if (_shutdown) {
      socketConn._close();
      return;
    };

    // Mark socket as returned to unregister from the old connection.
    socketConn._markReturned();

    String key = _connectionKey(socketConn._host, socketConn._port);

    // Get or create the connection list for this key.
    Queue sockets = _openSockets[key];
    if (sockets == null) {
      sockets = new Queue();
      _openSockets[key] = sockets;
    }

    // If there is currently no eviction timer start one.
    if (_evictionTimer == null) {
      void _handleEviction(Timer timer) {
        DateTime now = new DateTime.now();
        List<String> emptyKeys = new List<String>();
        _openSockets.forEach(
            (String key, Queue<_SocketConnection> connections) {
              // As returned connections are added at the head of the
              // list remove from the tail.
              while (!connections.isEmpty) {
                _SocketConnection socketConn = connections.last;
                if (socketConn._idleTime(now).inMilliseconds >
                    DEFAULT_EVICTION_TIMEOUT) {
                  connections.removeLast();
                  socketConn._socket.close();
                  if (connections.isEmpty) emptyKeys.add(key);
                } else {
                  break;
                }
              }
            });

        // Remove the keys for which here are no more open connections.
        emptyKeys.forEach((String key) => _openSockets.remove(key));

        // If all connections where evicted cancel the eviction timer.
        if (_openSockets.isEmpty) _cancelEvictionTimer();
      }
      _evictionTimer = new Timer.repeating(10000, _handleEviction);
    }

    // Return connection.
    _activeSockets.remove(socketConn);
    sockets.addFirst(socketConn);
  }

  void _closeSocketConnection(_SocketConnection socketConn) {
    socketConn._close();
    _activeSockets.remove(socketConn);
  }

  void _closedSocketConnection(_SocketConnection socketConn) {
    _activeSockets.remove(socketConn);
  }

  _Credentials _findCredentials(Uri url, [_AuthenticationScheme scheme]) {
    // Look for credentials.
    _Credentials cr =
        credentials.reduce(null, (_Credentials prev, _Credentials value) {
          if (value.applies(url, scheme)) {
            if (prev == null) return value;
            return value.uri.path.length > prev.uri.path.length ? value : prev;
          } else {
            return prev;
          }
        });
    return cr;
  }

  void _removeCredentials(_Credentials cr) {
    int index = credentials.indexOf(cr);
    if (index != -1) {
      credentials.removeAt(index);
    }
  }

  Function _onOpen;
  Map<String, Queue<_SocketConnection>> _openSockets;
  Set<_SocketConnection> _activeSockets;
  _CloseQueue _closeQueue;
  List<_Credentials> credentials;
  Timer _evictionTimer;
  Function _findProxy;
  Function _authenticate;
  bool _sendClientCertificate = false;
  String _clientCertificate;
  bool _shutdown;  // Has this HTTP client been shutdown?
}


class _HttpConnectionInfo implements HttpConnectionInfo {
  String remoteHost;
  int remotePort;
  int localPort;
}


class _DetachedSocket implements DetachedSocket {
  _DetachedSocket(this._socket, this._unparsedData);
  Socket get socket => _socket;
  List<int> get unparsedData => _unparsedData;
  Socket _socket;
  List<int> _unparsedData;
}


class _AuthenticationScheme {
  static const UNKNOWN = const _AuthenticationScheme(-1);
  static const BASIC = const _AuthenticationScheme(0);
  static const DIGEST = const _AuthenticationScheme(1);

  const _AuthenticationScheme(this._scheme);

  factory _AuthenticationScheme.fromString(String scheme) {
    if (scheme.toLowerCase() == "basic") return BASIC;
    if (scheme.toLowerCase() == "digest") return DIGEST;
    return UNKNOWN;
  }

  String toString() {
    if (this == BASIC) return "Basic";
    if (this == DIGEST) return "Digest";
    return "Unknown";
  }

  final int _scheme;
}


class _Credentials {
  _Credentials(this.uri, this.realm, this.credentials);

  _AuthenticationScheme get scheme => credentials.scheme;

  bool applies(Uri uri, _AuthenticationScheme scheme) {
    if (scheme != null && credentials.scheme != scheme) return false;
    if (uri.domain != this.uri.domain) return false;
    int thisPort =
        this.uri.port == 0 ? HttpClient.DEFAULT_HTTP_PORT : this.uri.port;
    int otherPort = uri.port == 0 ? HttpClient.DEFAULT_HTTP_PORT : uri.port;
    if (otherPort != thisPort) return false;
    return uri.path.startsWith(this.uri.path);
  }

  void authorize(HttpClientRequest request) {
    credentials.authorize(this, request);
    used = true;
  }

  bool used = false;
  Uri uri;
  String realm;
  HttpClientCredentials credentials;

  // Digest specific fields.
  String nonce;
  String algorithm;
  String qop;
}


abstract class _HttpClientCredentials implements HttpClientCredentials {
  _AuthenticationScheme get scheme;
  void authorize(HttpClientRequest request);
}


class _HttpClientBasicCredentials implements HttpClientBasicCredentials {
  _HttpClientBasicCredentials(this.username,
                              this.password);

  _AuthenticationScheme get scheme => _AuthenticationScheme.BASIC;

  void authorize(_Credentials _, HttpClientRequest request) {
    // There is no mentioning of username/password encoding in RFC
    // 2617. However there is an open draft for adding an additional
    // accept-charset parameter to the WWW-Authenticate and
    // Proxy-Authenticate headers, see
    // http://tools.ietf.org/html/draft-reschke-basicauth-enc-06. For
    // now always use UTF-8 encoding.
    _UTF8Encoder encoder = new _UTF8Encoder();
    String auth =
        CryptoUtils.bytesToBase64(encoder.encodeString(
            "$username:$password"));
    request.headers.set(HttpHeaders.AUTHORIZATION, "Basic $auth");
  }

  String username;
  String password;
}


class _HttpClientDigestCredentials implements HttpClientDigestCredentials {
  _HttpClientDigestCredentials(this.username,
                               this.password);

  _AuthenticationScheme get scheme => _AuthenticationScheme.DIGEST;

  void authorize(_Credentials credentials, HttpClientRequest request) {
    // TODO(sgjesse): Implement!!!
    throw new UnsupportedError("Digest authentication not yet supported");
  }

  String username;
  String password;
}



class _RedirectInfo implements RedirectInfo {
  const _RedirectInfo(int this.statusCode,
                      String this.method,
                      Uri this.location);
  final int statusCode;
  final String method;
  final Uri location;
}
