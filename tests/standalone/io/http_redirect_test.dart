// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//

import "dart:io";
import "dart:uri";

HttpServer setupServer() {
  HttpServer server = new HttpServer();
  server.listen("127.0.0.1", 0, backlog: 5);

  void addRedirectHandler(int number, int statusCode) {
    server.addRequestHandler(
       (HttpRequest request) => request.path == "/$number",
       (HttpRequest request, HttpResponse response) {
       response.headers.set(HttpHeaders.LOCATION,
                            "http://127.0.0.1:${server.port}/${number + 1}");
       response.statusCode = statusCode;
       response.outputStream.close();
     });
  }

  // Setup simple redirect.
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/redirect",
     (HttpRequest request, HttpResponse response) {
       response.headers.set(HttpHeaders.LOCATION,
                            "http://127.0.0.1:${server.port}/location");
       response.statusCode = HttpStatus.MOVED_PERMANENTLY;
       response.outputStream.close();
     }
  );
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/location",
     (HttpRequest request, HttpResponse response) {
       response.outputStream.close();
     }
  );

  // Setup redirect chain.
  int n = 1;
  addRedirectHandler(n++, HttpStatus.MOVED_PERMANENTLY);
  addRedirectHandler(n++, HttpStatus.MOVED_TEMPORARILY);
  addRedirectHandler(n++, HttpStatus.SEE_OTHER);
  addRedirectHandler(n++, HttpStatus.TEMPORARY_REDIRECT);
  for (int i = n; i < 10; i++) {
    addRedirectHandler(i, HttpStatus.MOVED_PERMANENTLY);
  }

  // Setup redirect loop.
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/A",
     (HttpRequest request, HttpResponse response) {
       response.headers.set(HttpHeaders.LOCATION,
                            "http://127.0.0.1:${server.port}/B");
       response.statusCode = HttpStatus.MOVED_PERMANENTLY;
       response.outputStream.close();
     }
  );
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/B",
     (HttpRequest request, HttpResponse response) {
       response.headers.set(HttpHeaders.LOCATION,
                            "http://127.0.0.1:${server.port}/A");
       response.statusCode = HttpStatus.MOVED_TEMPORARILY;
       response.outputStream.close();
     }
  );

  // Setup redirect checking headers.
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/src",
     (HttpRequest request, HttpResponse response) {
       Expect.equals("value", request.headers.value("X-Request-Header"));
       response.headers.set(HttpHeaders.LOCATION,
                            "http://127.0.0.1:${server.port}/target");
       response.statusCode = HttpStatus.MOVED_PERMANENTLY;
       response.outputStream.close();
     }
  );
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/target",
     (HttpRequest request, HttpResponse response) {
       Expect.equals("value", request.headers.value("X-Request-Header"));
       response.outputStream.close();
     }
  );

  // Setup redirect for 301 where POST should not redirect.
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/301src",
     (HttpRequest request, HttpResponse response) {
       Expect.equals("POST", request.method);
       response.headers.set(HttpHeaders.LOCATION,
                            "http://127.0.0.1:${server.port}/301target");
       response.statusCode = HttpStatus.MOVED_PERMANENTLY;
       response.outputStream.close();
     }
  );
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/301target",
     (HttpRequest request, HttpResponse response) {
       Expect.fail("Redirect of POST should not happen");
     }
  );

  // Setup redirect for 303 where POST should turn into GET.
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/303src",
     (HttpRequest request, HttpResponse response) {
       Expect.equals("POST", request.method);
       Expect.equals(10, request.contentLength);
       request.inputStream.onData = request.inputStream.read;
       request.inputStream.onClosed = () {
         response.headers.set(HttpHeaders.LOCATION,
                              "http://127.0.0.1:${server.port}/303target");
         response.statusCode = HttpStatus.SEE_OTHER;
         response.outputStream.close();
       };
     }
  );
  server.addRequestHandler(
     (HttpRequest request) => request.path == "/303target",
     (HttpRequest request, HttpResponse response) {
       Expect.equals("GET", request.method);
       response.outputStream.close();
     }
  );

  return server;
}

void checkRedirects(int redirectCount, HttpClientConnection conn) {
  if (redirectCount < 2) {
    Expect.isNull(conn.redirects);
  } else {
    Expect.equals(redirectCount - 1, conn.redirects.length);
    for (int i = 0; i < redirectCount - 2; i++) {
      Expect.equals(conn.redirects[i].location.path, "/${i + 2}");
    }
  }
}

void testManualRedirect() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  int redirectCount = 0;
  HttpClientConnection conn =
     client.getUrl(Uri.parse("http://127.0.0.1:${server.port}/1"));
  conn.followRedirects = false;
  conn.onResponse = (HttpClientResponse response) {
    response.inputStream.onData = response.inputStream.read;
    response.inputStream.onClosed = () {
      redirectCount++;
      if (redirectCount < 10) {
        Expect.isTrue(response.isRedirect);
        checkRedirects(redirectCount, conn);
        conn.redirect();
      } else {
        Expect.equals(HttpStatus.NOT_FOUND, response.statusCode);
        server.close();
        client.shutdown();
      }
    };
  };
}

void testManualRedirectWithHeaders() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  int redirectCount = 0;
  HttpClientConnection conn =
     client.getUrl(Uri.parse("http://127.0.0.1:${server.port}/src"));
  conn.followRedirects = false;
  conn.onRequest = (HttpClientRequest request) {
    request.headers.add("X-Request-Header", "value");
    request.outputStream.close();
  };
  conn.onResponse = (HttpClientResponse response) {
    response.inputStream.onData = response.inputStream.read;
    response.inputStream.onClosed = () {
      redirectCount++;
      if (redirectCount < 2) {
        Expect.isTrue(response.isRedirect);
        conn.redirect();
      } else {
        Expect.equals(HttpStatus.OK, response.statusCode);
        server.close();
        client.shutdown();
      }
    };
  };
}

void testAutoRedirect() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  var requestCount = 0;

  void onRequest(HttpClientRequest request) {
    requestCount++;
    request.outputStream.close();
  }

  void onResponse(HttpClientResponse response) {
    response.inputStream.onData =
        () => Expect.fail("Response data not expected");
    response.inputStream.onClosed = () {
      Expect.equals(1, requestCount);
      server.close();
      client.shutdown();
    };
  };

  HttpClientConnection conn =
      client.getUrl(
          Uri.parse("http://127.0.0.1:${server.port}/redirect"));
  conn.onRequest = onRequest;
  conn.onResponse = onResponse;
  conn.onError = (e) => Expect.fail("Error not expected ($e)");
}

void testAutoRedirectWithHeaders() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  var requestCount = 0;

  void onRequest(HttpClientRequest request) {
    requestCount++;
    request.headers.add("X-Request-Header", "value");
    request.outputStream.close();
  };

  void onResponse(HttpClientResponse response) {
    response.inputStream.onData =
        () => Expect.fail("Response data not expected");
    response.inputStream.onClosed = () {
      Expect.equals(1, requestCount);
      server.close();
      client.shutdown();
    };
  };

  HttpClientConnection conn =
      client.getUrl(Uri.parse("http://127.0.0.1:${server.port}/src"));
  conn.onRequest = onRequest;
  conn.onResponse = onResponse;
  conn.onError = (e) => Expect.fail("Error not expected ($e)");
}

void testAutoRedirect301POST() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  var requestCount = 0;

  void onRequest(HttpClientRequest request) {
    requestCount++;
    request.outputStream.close();
  };

  void onResponse(HttpClientResponse response) {
    Expect.equals(HttpStatus.MOVED_PERMANENTLY, response.statusCode);
    response.inputStream.onData =
        () => Expect.fail("Response data not expected");
    response.inputStream.onClosed = () {
      Expect.equals(1, requestCount);
      server.close();
      client.shutdown();
    };
  };

  HttpClientConnection conn =
      client.postUrl(
          Uri.parse("http://127.0.0.1:${server.port}/301src"));
  conn.onRequest = onRequest;
  conn.onResponse = onResponse;
  conn.onError = (e) => Expect.fail("Error not expected ($e)");
}

void testAutoRedirect303POST() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  var requestCount = 0;

  void onRequest(HttpClientRequest request) {
    requestCount++;
    request.contentLength = 10;
    request.outputStream.write(new List<int>.fixedLength(10, fill: 0));
    request.outputStream.close();
  };

  void onResponse(HttpClientResponse response) {
    Expect.equals(HttpStatus.OK, response.statusCode);
    response.inputStream.onData =
        () => Expect.fail("Response data not expected");
    response.inputStream.onClosed = () {
      Expect.equals(1, requestCount);
      server.close();
      client.shutdown();
    };
  };

  HttpClientConnection conn =
      client.postUrl(
          Uri.parse("http://127.0.0.1:${server.port}/303src"));
  conn.onRequest = onRequest;
  conn.onResponse = onResponse;
  conn.onError = (e) => Expect.fail("Error not expected ($e)");
}

void testAutoRedirectLimit() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  HttpClientConnection conn =
      client.getUrl(Uri.parse("http://127.0.0.1:${server.port}/1"));
  conn.onResponse = (HttpClientResponse response) {
    response.inputStream.onData = () => Expect.fail("Response not expected");
    response.inputStream.onClosed = () => Expect.fail("Response not expected");
  };
  conn.onError = (e) {
    Expect.isTrue(e is RedirectLimitExceededException);
    Expect.equals(5, e.redirects.length);
    server.close();
    client.shutdown();
  };
}

void testRedirectLoop() {
  HttpServer server = setupServer();
  HttpClient client = new HttpClient();

  int redirectCount = 0;
  HttpClientConnection conn =
      client.getUrl(Uri.parse("http://127.0.0.1:${server.port}/A"));
  conn.onResponse = (HttpClientResponse response) {
    response.inputStream.onData = () => Expect.fail("Response not expected");
    response.inputStream.onClosed = () => Expect.fail("Response not expected");
  };
  conn.onError = (e) {
    Expect.isTrue(e is RedirectLoopException);
    Expect.equals(2, e.redirects.length);
    server.close();
    client.shutdown();
  };
}

main() {
  testManualRedirect();
  testManualRedirectWithHeaders();
  testAutoRedirect();
  testAutoRedirectWithHeaders();
  testAutoRedirect301POST();
  testAutoRedirect303POST();
  testAutoRedirectLimit();
  testRedirectLoop();
}
