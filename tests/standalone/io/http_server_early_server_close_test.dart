// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "dart:async";
import "dart:io";
import "dart:isolate";

class Server {
  Server() {
    HttpServer server = new HttpServer();
    server.listen("127.0.0.1", 0);
    port = server.port;
    server.defaultRequestHandler =
        (HttpRequest request, HttpResponse response) {
          new Timer(0, (timer) => server.close());
        };
    server.onError = (e) {
      Expect.fail("No server errors expected: $e");
    };
  }
  int port;
}

class Client {
  Client(int port) {
    ReceivePort r = new ReceivePort();
    HttpClient client = new HttpClient();
    HttpClientConnection c = client.get("127.0.0.1", port, "/");
    c.onRequest = (HttpClientRequest request) {
      request.outputStream.close();
    };
    c.onResponse = (HttpClientResponse response) {
      Expect.fail("Response should not be given, as not data was returned.");
    };
    c.onError = (e) {
      r.close();
    };
  }
}

main() {
  Server server = new Server();
  new Client(server.port);
}
