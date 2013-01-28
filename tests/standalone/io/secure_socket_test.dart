// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// VMOptions=
// VMOptions=--short_socket_read
// The --short_socket_write option does not work with external server
// www.google.dk.  Add this to the test when we have secure server sockets.
// See TODO below.

import "dart:isolate";
import "dart:io";

void WriteAndClose(Socket socket, String message) {
  var data = message.charCodes;
  int written = 0;
  void write() {
    written += socket.writeList(data, written, data.length - written);
    if (written < data.length) {
      socket.onWrite = write;
    } else {
      socket.close(true);
    }
  }
  write();
}

void main() {
  ReceivePort keepAlive = new ReceivePort();
  SecureSocket.initialize();
  // TODO(3593): Use a Dart HTTPS server for this test.
  // When we use a Dart HTTPS server, allow --short_socket_write. The flag
  // causes fragmentation of the client hello message, which doesn't seem to
  // work with www.google.dk.
  var secure = new SecureSocket("www.google.dk", 443);
  List<String> chunks = <String>[];
  secure.onConnect = () {
    WriteAndClose(secure, "GET / HTTP/1.0\r\nHost: www.google.dk\r\n\r\n");
  };
  var useReadList;  // Mutually recursive onData callbacks.
  void useRead() {
    var data = secure.read();
    var received = new String.fromCharCodes(data);
    chunks.add(received);
    secure.onData = useReadList;
  }
  useReadList = () {
    var buffer = new List.fixedLength(2000);
    int len = secure.readList(buffer, 0, 2000);
    var received = new String.fromCharCodes(buffer.getRange(0, len));
    chunks.add(received);
    secure.onData = useRead;
  };
  secure.onData = useRead;
  secure.onClosed = () {
    String fullPage = Strings.concatAll(chunks);
    Expect.isTrue(fullPage.contains('</body></html>'));
    keepAlive.close();
  };
}
