// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart.io;

// Interface for decoders decoding binary data into string data. The
// decoder keeps track of line breaks during decoding.
abstract class _StringDecoder {
  // Add more binary data to be decoded. The ownership of the buffer
  // is transfered to the decoder and the caller most not modify it any more.
  int write(List<int> buffer);

  void done();

  // Returns whether any decoded data is available.
  bool get isEmpty;

  // Returns the number of available decoded characters.
  int available();

  // Get the number of line breaks present in the current decoded
  // data.
  int get lineBreaks;

  // Get up to [len] characters of string data decoded since the last
  // call to [decode] or [decodeLine]. Returns null if no decoded data
  // is available. If [len] is not specified all decoded characters
  // are returned.
  String decoded([int len]);

  // Get the string data decoded since the last call to [decode] or
  // [decodeLine] up to the next line break present. Returns null if
  // no line break is present. The line break character sequence is
  // discarded.
  String get decodedLine;

  // Set the handler that will be called if an error ocurs while decoding.
  void set onError(Function callback);
}


class _StringDecoders {
  static _StringDecoder decoder(Encoding encoding) {
    if (encoding == Encoding.UTF_8) {
      return new _UTF8Decoder();
    } else if (encoding == Encoding.ISO_8859_1) {
      return new _Latin1Decoder();
    } else if (encoding == Encoding.ASCII) {
      return new _AsciiDecoder();
    } else if (encoding == Encoding.SYSTEM) {
      if (Platform.operatingSystem == 'windows') {
        return new _WindowsCodePageDecoder();
      }
      return new _UTF8Decoder();
    } else {
      if (encoding is Encoding) {
        throw new StreamException("Unsupported encoding ${encoding.name}");
      } else {
        throw new StreamException("Unsupported encoding ${encoding}");
      }
    }
  }
}


class DecoderException implements Exception {
  const DecoderException([String this.message]);
  String toString() => "DecoderException: $message";
  final String message;
}


// Utility class for decoding UTF-8 from data delivered as a stream of
// bytes.
abstract class _StringDecoderBase implements _StringDecoder {
  _StringDecoderBase()
      : _bufferList = new _BufferList(),
        _result = new List<int>(),
        _lineBreakEnds = new Queue<int>();

  int write(List<int> buffer) {
    _bufferList.add(buffer);
    // Decode as many bytes into characters as possible.
    while (_bufferList.length > 0) {
      if (!_processNext()) {
        break;
      }
    }
    return buffer.length;
  }

  void done() { }

  bool get isEmpty => _result.isEmpty;

  int get lineBreaks => _lineBreaks;

  String decoded([int length]) {
    if (isEmpty) return null;

    List<int> result;
    if (length != null && length < available()) {
      result = _result.getRange(_resultOffset, length);
    } else if (_resultOffset == 0) {
      result = _result;
    } else {
      result = _result.getRange(_resultOffset, _result.length - _resultOffset);
    }

    _resultOffset += result.length;
    while (!_lineBreakEnds.isEmpty &&
           _lineBreakEnds.first < _charOffset + _resultOffset) {
      _lineBreakEnds.removeFirst();
      _lineBreaks--;
    }
    if (_result.length == _resultOffset) _resetResult();
    return new String.fromCharCodes(result);
  }

  String get decodedLine {
    if (_lineBreakEnds.isEmpty) return null;
    int lineEnd = _lineBreakEnds.removeFirst();
    int terminationSequenceLength = 1;
    if (_result[lineEnd - _charOffset] == LF &&
        lineEnd > _charOffset &&
        _resultOffset < lineEnd &&
        _result[lineEnd - _charOffset - 1] == CR) {
      terminationSequenceLength = 2;
    }
    var lineLength =
        lineEnd - _charOffset - _resultOffset - terminationSequenceLength + 1;
    String result =
        new String.fromCharCodes(_result.getRange(_resultOffset, lineLength));
    _lineBreaks--;
    _resultOffset += (lineLength + terminationSequenceLength);
    if (_result.length == _resultOffset) _resetResult();
    return result;
  }

  // Add another decoded character.
  void addChar(int charCode) {
    _result.add(charCode);
    _charCount++;
    // Check for line ends (\r, \n and \r\n).
    if (charCode == LF) {
      _recordLineBreakEnd(_charCount - 1);
    } else if (_lastCharCode == CR) {
      _recordLineBreakEnd(_charCount - 2);
    }
    _lastCharCode = charCode;
  }

  int available() => _result.length - _resultOffset;

  void _recordLineBreakEnd(int charPos) {
    _lineBreakEnds.add(charPos);
    _lineBreaks++;
  }

  void _resetResult() {
    _charOffset += _result.length;
    _result = new List<int>();
    _resultOffset = 0;
  }

  bool _processNext();

  _BufferList _bufferList;
  int _resultOffset = 0;
  List<int> _result;
  int _lineBreaks = 0;  // Number of line breaks in the current list.
  // The positions of the line breaks are tracked in terms of absolute
  // character positions from the begining of the decoded data.
  Queue<int> _lineBreakEnds;  // Character position of known line breaks.
  int _charOffset = 0;  // Character number of the first character in the list.
  int _charCount = 0;  // Total number of characters decoded.
  int _lastCharCode = -1;
  Function onError;

  final int LF = 10;
  final int CR = 13;
}


// Utility class for decoding UTF-8 from data delivered as a stream of
// bytes.
class _UTF8Decoder extends _StringDecoderBase {
  static const kMaxCodePoint = 0x10FFFF;
  static const kReplacementCodePoint = 0x3f;

  void done() {
    if (!_bufferList.isEmpty) {
      _reportError(new DecoderException("Illegal UTF-8"));
    }
  }

  bool _reportError(error) {
    if (onError != null) {
      onError(error);
      return false;
    } else {
      throw error;
    }
  }

  // Process the next UTF-8 encoded character.
  bool _processNext() {
    // Peek the next byte to calculate the number of bytes required for
    // the next character.
    int value = _bufferList.peek() & 0xFF;
    if ((value & 0x80) == 0x80) {
      int additionalBytes;
      if ((value & 0xe0) == 0xc0) {  // 110xxxxx
        value = value & 0x1F;
        additionalBytes = 1;
      } else if ((value & 0xf0) == 0xe0) {  // 1110xxxx
        value = value & 0x0F;
        additionalBytes = 2;
      } else if ((value & 0xf8) == 0xf0) {  // 11110xxx
        value = value & 0x07;
        additionalBytes = 3;
      } else if ((value & 0xfc) == 0xf8) {  // 111110xx
        value = value & 0x03;
        additionalBytes = 4;
      } else if ((value & 0xfe) == 0xfc) {  // 1111110x
        value = value & 0x01;
        additionalBytes = 5;
      } else {
        return _reportError(new DecoderException("Illegal UTF-8"));
      }
      // Check if there are enough bytes to decode the character. Otherwise
      // return false.
      if (_bufferList.length < additionalBytes + 1) {
        return false;
      }
      // Remove the value peeked from the buffer list.
      _bufferList.next();
      for (int i = 0; i < additionalBytes; i++) {
        int byte = _bufferList.next();
        if ((byte & 0xc0) != 0x80) {
          return _reportError(new DecoderException("Illegal UTF-8"));
        }
        value = value << 6 | (byte & 0x3F);
      }
    } else {
      // Remove the value peeked from the buffer list.
      _bufferList.next();
    }
    if (value > kMaxCodePoint) {
      addChar(kReplacementCodePoint);
    } else {
      addChar(value);
    }
    return true;
  }
}


// Utility class for decoding ascii data delivered as a stream of
// bytes.
class _AsciiDecoder extends _StringDecoderBase {
  // Process the next ascii encoded character.
  bool _processNext() {
    while (_bufferList.length > 0) {
      int byte = _bufferList.next();
      if (byte > 127) {
        var error = new DecoderException("Illegal ASCII character $byte");
        if (onError != null) {
          onError(error);
          return false;
        } else {
          throw error;
        }
      }
      addChar(byte);
    }
    return true;
  }
}


// Utility class for decoding Latin-1 data delivered as a stream of
// bytes.
class _Latin1Decoder extends _StringDecoderBase {
  // Process the next Latin-1 encoded character.
  bool _processNext() {
    while (_bufferList.length > 0) {
      int byte = _bufferList.next();
      addChar(byte);
    }
    return true;
  }
}


// Utility class for decoding Windows current code page data delivered
// as a stream of bytes.
class _WindowsCodePageDecoder extends _StringDecoderBase {
  // Process the next chunk of data.
  bool _processNext() {
    List<int> bytes = _bufferList.readBytes(_bufferList.length);
    for (var charCode in _decodeBytes(bytes).charCodes) {
      addChar(charCode);
    }
    return true;
  }

  external static String _decodeBytes(List<int> bytes);
}


// Interface for encoders encoding string data into binary data.
abstract class _StringEncoder {
  List<int> encodeString(String string);
}


// Utility class for encoding a string into UTF-8 byte stream.
class _UTF8Encoder implements _StringEncoder {
  List<int> encodeString(String string) {
    int size = _encodingSize(string);
    List<int> result = new Uint8List(size);
    _encodeString(string, result);
    return result;
  }

  static int _encodingSize(String string) => _encodeString(string, null);

  static int _encodeString(String string, List<int> buffer) {
    List<int> utf8CodeUnits = encodeUtf8(string);
    if (buffer != null) {
      for (int i = 0; i < utf8CodeUnits.length; i++) {
        buffer[i] = utf8CodeUnits[i];
      }
    }
    return utf8CodeUnits.length;
  }
}


// Utility class for encoding a string into a Latin1 byte stream.
class _Latin1Encoder implements _StringEncoder {
  List<int> encodeString(String string) {
    List<int> result = new Uint8List(string.length);
    for (int i = 0; i < string.length; i++) {
      int charCode = string.charCodeAt(i);
      if (charCode > 255) {
        throw new EncoderException(
            "No ISO_8859_1 encoding for code point $charCode");
      }
      result[i] = charCode;
    }
    return result;
  }
}


// Utility class for encoding a string into an ASCII byte stream.
class _AsciiEncoder implements _StringEncoder {
  List<int> encodeString(String string) {
    List<int> result = new Uint8List(string.length);
    for (int i = 0; i < string.length; i++) {
      int charCode = string.charCodeAt(i);
      if (charCode > 127) {
        throw new EncoderException(
            "No ASCII encoding for code point $charCode");
      }
      result[i] = charCode;
    }
    return result;
  }
}


// Utility class for encoding a string into a current windows
// code page byte list.
class _WindowsCodePageEncoder implements _StringEncoder {
  List<int> encodeString(String string) {
    return _encodeString(string);
  }

  external static List<int> _encodeString(String string);
}


class _StringEncoders {
  static _StringEncoder encoder(Encoding encoding) {
    if (encoding == Encoding.UTF_8) {
      return new _UTF8Encoder();
    } else if (encoding == Encoding.ISO_8859_1) {
      return new _Latin1Encoder();
    } else if (encoding == Encoding.ASCII) {
      return new _AsciiEncoder();
    } else if (encoding == Encoding.SYSTEM) {
      if (Platform.operatingSystem == 'windows') {
        return new _WindowsCodePageEncoder();
      }
      return new _UTF8Encoder();
    } else {
      throw new StreamException("Unsupported encoding ${encoding.name}");
    }
  }
}


class EncoderException implements Exception {
  const EncoderException([String this.message]);
  String toString() => "EncoderException: $message";
  final String message;
}


class _StringInputStream implements StringInputStream {
  _StringInputStream(InputStream this._input, Encoding this._encoding) {
    _decoder = _StringDecoders.decoder(encoding);
    _input.onData = _onData;
    _input.onClosed = _onClosed;
  }

  String read([int len]) {
    String result = _decoder.decoded(len);
    _checkInstallDataHandler();
    return result;
  }

  String readLine() {
    String decodedLine = _decoder.decodedLine;
    if (decodedLine == null) {
      if (_inputClosed) {
        // Last line might not have a line separator.
        decodedLine = _decoder.decoded();
        if (decodedLine != null &&
            decodedLine[decodedLine.length - 1] == '\r') {
          decodedLine = decodedLine.substring(0, decodedLine.length - 1);
        }
      }
    }
    _checkInstallDataHandler();
    return decodedLine;
  }

  int available() => _decoder.available();

  Encoding get encoding => _encoding;

  bool get closed => _inputClosed && _decoder.isEmpty;

  void set onData(void callback()) {
    _clientDataHandler = callback;
    _clientLineHandler = null;
    _checkInstallDataHandler();
    _checkScheduleCallback();
  }

  void set onLine(void callback()) {
    _clientLineHandler = callback;
    _clientDataHandler = null;
    _checkInstallDataHandler();
    _checkScheduleCallback();
  }

  void set onClosed(void callback()) {
    _clientCloseHandler = callback;
  }

  void set onError(void callback(e)) {
    _input.onError = callback;
    _decoder.onError = (e) {
      _clientCloseHandler = null;
      _input.close();
      callback(e);
    };
  }

  void _onData() {
    _readData();
    if (!_decoder.isEmpty && _clientDataHandler != null) {
      _clientDataHandler();
    }
    if (_decoder.lineBreaks > 0 && _clientLineHandler != null) {
      _clientLineHandler();
    }
    _checkScheduleCallback();
    _checkInstallDataHandler();
  }

  void _onClosed() {
    _inputClosed = true;
    _decoder.done();
    if (_decoder.isEmpty && _clientCloseHandler !=  null) {
      _clientCloseHandler();
    } else {
      _checkScheduleCallback();
    }
  }

  void _readData() {
    List<int> data = _input.read();
    if (data != null) {
      _decoder.write(data);
    }
  }

  void _checkInstallDataHandler() {
    if (_inputClosed ||
        (_clientDataHandler == null && _clientLineHandler == null)) {
      _input.onData = null;
    } else if (_clientDataHandler != null) {
      if (_decoder.isEmpty) {
        _input.onData = _onData;
      } else {
        _input.onData = null;
      }
    } else {
      assert(_clientLineHandler != null);
      if (_decoder.lineBreaks == 0) {
        _input.onData = _onData;
      } else {
        _input.onData = null;
      }
    }
  }

  // TODO(sgjesse): Find a better way of scheduling callbacks from
  // the event loop.
  void _checkScheduleCallback() {
    void issueDataCallback(Timer timer) {
      _scheduledDataCallback = null;
      if (_clientDataHandler != null) {
        _clientDataHandler();
        _checkScheduleCallback();
      }
    }

    void issueLineCallback(Timer timer) {
      _scheduledLineCallback = null;
      if (_clientLineHandler != null) {
        _clientLineHandler();
        _checkScheduleCallback();
      }
    }

    void issueCloseCallback(Timer timer) {
      _scheduledCloseCallback = null;
      if (!_closed) {
        if (_clientCloseHandler != null) _clientCloseHandler();
        _closed = true;
      }
    }

    if (!_closed) {
      // Schedule data callback if string data available.
      if (_clientDataHandler != null &&
          !_decoder.isEmpty &&
          _scheduledDataCallback == null) {
        if (_scheduledLineCallback != null) {
          _scheduledLineCallback.cancel();
        }
        _scheduledDataCallback = new Timer(0, issueDataCallback);
      }

      // Schedule line callback if a line is available.
      if (_clientLineHandler != null &&
          (_decoder.lineBreaks > 0 || (!_decoder.isEmpty && _inputClosed)) &&
          _scheduledLineCallback == null) {
        if (_scheduledDataCallback != null) {
          _scheduledDataCallback.cancel();
        }
        _scheduledLineCallback = new Timer(0, issueLineCallback);
      }

      // Schedule close callback if no more data and input is closed.
      if (_decoder.isEmpty &&
          _inputClosed &&
          _scheduledCloseCallback == null) {
        _scheduledCloseCallback = new Timer(0, issueCloseCallback);
      }
    }
  }

  InputStream _input;
  Encoding _encoding;
  _StringDecoder _decoder;
  bool _inputClosed = false;  // Is the underlying input stream closed?
  bool _closed = false;  // Is this stream closed.
  bool _eof = false;  // Has all data been read from the decoder?
  Timer _scheduledDataCallback;
  Timer _scheduledLineCallback;
  Timer _scheduledCloseCallback;
  Function _clientDataHandler;
  Function _clientLineHandler;
  Function _clientCloseHandler;
}
