// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Echo server test program to test socket streams.
//
// VMOptions=
// VMOptions=--short_socket_read
// VMOptions=--short_socket_write
// VMOptions=--short_socket_read --short_socket_write

library ServerTest;

import "dart:io";
import "dart:isolate";
part "testing_server.dart";

class EchoServerGame {

  static const MSGSIZE = 10;
  static const MESSAGES = 100;
  static const FIRSTCHAR = 65;

  EchoServerGame.start()
      : _receivePort = new ReceivePort(),
        _sendPort = null,
        _buffer = new List<int>.fixedLength(MSGSIZE),
        _messages = 0 {
    for (int i = 0; i < MSGSIZE; i++) {
      _buffer[i] = FIRSTCHAR + i;
    }
    _sendPort = spawnFunction(startEchoServer);
    initialize();
  }

  void sendData() {

    void errorHandler(Exception e) {
      Expect.fail("Socket error $e");
    }

    void connectHandler() {

      OutputStream stream = _socket.outputStream;

      void dataSent() {
        InputStream inputStream = _socket.inputStream;
        int offset = 0;
        List<int> data;

        void onClosed() {
          Expect.equals(MSGSIZE, offset);
          _messages++;
          if (_messages < MESSAGES) {
            sendData();
          } else {
            shutdown();
          }
        }

        void onData() {
          // Test both read and readInto.
          int bytesRead = 0;
          if (_messages % 2 == 0) {
            bytesRead = inputStream.readInto(data, offset, MSGSIZE - offset);
            for (int i = 0; i < offset + bytesRead; i++) {
              Expect.equals(FIRSTCHAR + i, data[i]);
            }
          } else {
            data = inputStream.read();
            bytesRead = data.length;
            for (int i = 0; i < data.length; i++) {
              Expect.equals(FIRSTCHAR + i + offset, data[i]);
            }
          }

          offset += bytesRead;
        }

        if (_messages % 2 == 0) data = new List<int>.fixedLength(MSGSIZE);
        inputStream.onData = onData;
        inputStream.onClosed = onClosed;
      }

      _socket.onError = errorHandler;

      // Test both write and writeFrom in different forms.
      switch (_messages % 4) {
        case 0:
          stream.write(_buffer);
          break;
        case 1:
          stream.write(_buffer, false);
          break;
        case 2:
          stream.writeFrom(_buffer);
          break;
        case 3:
          Expect.equals(0, _buffer.length % 2);
          stream.writeFrom(_buffer, 0, _buffer.length ~/ 2);
          stream.writeFrom(_buffer, _buffer.length ~/ 2);
          break;
      }
      stream.close();
      dataSent();
    }

    _socket = new Socket(TestingServer.HOST, _port);
    if (_socket != null) {
      _socket.onConnect = connectHandler;
    } else {
      Expect.fail("socket creation failed");
    }
  }

  void initialize() {
    _receivePort.receive((var message, SendPort replyTo) {
      _port = message;
      sendData();
    });
    _sendPort.send(TestingServer.INIT, _receivePort.toSendPort());
  }

  void shutdown() {
    _sendPort.send(TestingServer.SHUTDOWN, _receivePort.toSendPort());
    _receivePort.close();
  }

  int _port;
  ReceivePort _receivePort;
  SendPort _sendPort;
  Socket _socket;
  List<int> _buffer;
  int _messages;
}


void startEchoServer() {
  var server = new EchoServer();
  port.receive(server.dispatch);
}


class EchoServer extends TestingServer {

  static const int MSGSIZE = EchoServerGame.MSGSIZE;

  void onConnection(Socket connection) {
    InputStream inputStream;
    List<int> buffer = new List<int>.fixedLength(MSGSIZE);
    int offset = 0;

    void dataReceived() {
      int bytesRead;
      OutputStream outputStream = connection.outputStream;
      bytesRead = inputStream.readInto(buffer, offset, MSGSIZE - offset);
      if (bytesRead > 0) {
        offset += bytesRead;
        for (int i = 0; i < offset; i++) {
          Expect.equals(EchoServerGame.FIRSTCHAR + i, buffer[i]);
        }
        if (offset == MSGSIZE) {
          outputStream.write(buffer);
          outputStream.close();
        }
      }
    }

    void errorHandler(Exception e) {
      Expect.fail("Socket error $e");
    }

    inputStream = connection.inputStream;
    inputStream.onData = dataReceived;
    connection.onError = errorHandler;
  }
}

main() {
  EchoServerGame echoServerGame = new EchoServerGame.start();
}
