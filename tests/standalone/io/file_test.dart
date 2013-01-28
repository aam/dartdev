// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Dart test program for testing file I/O.

import 'dart:async';
import 'dart:io';
import 'dart:isolate';

class MyListOfOneElement implements List {
  int _value;
  MyListOfOneElement(this._value);
  int get length => 1;
  operator [](int index) => _value;
}

class FileTest {
  static Directory tempDirectory;
  static int numLiveAsyncTests = 0;
  static ReceivePort port;

  static void asyncTestStarted() { ++numLiveAsyncTests; }
  static void asyncTestDone(String name) {
    --numLiveAsyncTests;
    if (numLiveAsyncTests == 0) {
      deleteTempDirectory();
      port.close();
    }
  }

  static void createTempDirectory(Function doNext) {
    new Directory('').createTemp().then((temp) {
      tempDirectory = temp;
      doNext();
    });
  }

  static void deleteTempDirectory() {
    tempDirectory.deleteSync(recursive: true);
  }

  // Test for file read functionality.
  static void testReadStream() {
    // Read a file and check part of it's contents.
    String filename = getFilename("bin/file_test.cc");
    File file = new File(filename);
    Expect.isTrue('$file'.contains(file.name));
    InputStream input = file.openInputStream();
    input.onData = () {
      List<int> buffer = new List<int>.fixedLength(42);
      int bytesRead = input.readInto(buffer, 0, 12);
      Expect.equals(12, bytesRead);
      bytesRead = input.readInto(buffer, 12, 30);
      input.close();
      Expect.equals(30, bytesRead);
      Expect.equals(47, buffer[0]);  // represents '/' in the file.
      Expect.equals(47, buffer[1]);  // represents '/' in the file.
      Expect.equals(32, buffer[2]);  // represents ' ' in the file.
      Expect.equals(67, buffer[3]);  // represents 'C' in the file.
      Expect.equals(111, buffer[4]);  // represents 'o' in the file.
      Expect.equals(112, buffer[5]);  // represents 'p' in the file.
      Expect.equals(121, buffer[6]);  // represents 'y' in the file.
      Expect.equals(114, buffer[7]);  // represents 'r' in the file.
      Expect.equals(105, buffer[8]);  // represents 'i' in the file.
      Expect.equals(103, buffer[9]);  // represents 'g' in the file.
      Expect.equals(104, buffer[10]);  // represents 'h' in the file.
      Expect.equals(116, buffer[11]);  // represents 't' in the file.
    };
  }

  // Test for file read and write functionality.
  static void testReadWriteStream() {
    asyncTestStarted();

    // Read a file.
    String inFilename = getFilename("tests/vm/data/fixed_length_file");
    File file;
    InputStream input;
    int bytesRead;

    // Test reading all using readInto.
    var file1 = new File(inFilename);
    var input1 = file1.openInputStream();
    List<int> buffer1;
    input1.onData = () {
      buffer1 = new List<int>.fixedLength(42);
      bytesRead = input1.readInto(buffer1, 0, 42);
      Expect.equals(42, bytesRead);
    };
    input1.onError = (e) { throw e; };
    input1.onClosed = () {
      Expect.isTrue(input1.closed);

      // Test reading all using readInto and read.
      var file2 = new File(inFilename);
      var input2 = file2.openInputStream();
      input2.onData = () {
        bytesRead = input2.readInto(buffer1, 0, 21);
        Expect.equals(21, bytesRead);
        buffer1 = input2.read();
        Expect.equals(21, buffer1.length);
      };
      input2.onError = (e) { throw e; };
      input2.onClosed = () {
        Expect.isTrue(input2.closed);

        // Test reading all using read and readInto.
        var file3 = new File(inFilename);
        var input3 = file3.openInputStream();
        input3.onData = () {
          buffer1 = input3.read(21);
          Expect.equals(21, buffer1.length);
          bytesRead = input3.readInto(buffer1, 0, 21);
          Expect.equals(21, bytesRead);
        };
        input3.onError = (e) { throw e; };
        input3.onClosed = () {
          Expect.isTrue(input3.closed);

          // Test reading all using read.
          var file4 = new File(inFilename);
          var input4 = file4.openInputStream();
          input4.onData = () {
            buffer1 = input4.read();
            Expect.equals(42, buffer1.length);
          };
          input4.onError = (e) { throw e; };
          input4.onClosed = () {
            Expect.isTrue(input4.closed);

            // Write the contents of the file just read into another file.
            String outFilename =
                tempDirectory.path.concat("/out_read_write_stream");
            file = new File(outFilename);
            OutputStream output = file.openOutputStream();
            bool writeDone = output.writeFrom(buffer1, 0, 42);
            Expect.equals(false, writeDone);
            output.onNoPendingWrites = () {
              output.close();
              output.onClosed = () {
                // Now read the contents of the file just written.
                List<int> buffer2 = new List<int>.fixedLength(42);
                var file6 = new File(outFilename);
                var input6 = file6.openInputStream();
                input6.onData = () {
                  bytesRead = input6.readInto(buffer2, 0, 42);
                  Expect.equals(42, bytesRead);
                  // Now compare the two buffers to check if they are identical.
                  for (int i = 0; i < buffer1.length; i++) {
                    Expect.equals(buffer1[i],  buffer2[i]);
                  }
                };
                input6.onError = (e) { throw e; };
                input6.onClosed = () {
                  // Delete the output file.
                  file6.deleteSync();
                  Expect.isFalse(file6.existsSync());
                  asyncTestDone("testReadWriteStream");
                };
              };
            };
          };
        };
      };
    };
  }

  // Test for file stream buffered handling of large files.
  static void testReadWriteStreamLargeFile() {
    asyncTestStarted();

    // Create the test data - arbitrary binary data.
    List<int> buffer = new List<int>.fixedLength(100000);
    for (var i = 0; i < buffer.length; ++i) {
      buffer[i] = i % 256;
    }
    String filename =
        tempDirectory.path.concat("/out_read_write_stream_large_file");
    File file = new File(filename);
    OutputStream output = file.openOutputStream();
    // Test a write immediately after the output stream is created.
    output.writeFrom(buffer, 0, 20000);

    output.onNoPendingWrites = () {
      output.writeFrom(buffer, 20000, 60000);
      output.writeFrom(buffer, 80000, 20000);
      output.onNoPendingWrites = () {
        output.writeFrom(buffer, 0, 0);
        output.writeFrom(buffer, 0, 0);
        output.writeFrom(buffer, 0, 100000);
        output.close();
      };
    };
    output.onClosed = () {
      InputStream input = file.openInputStream();
      int position = 0;
      final int expectedLength = 200000;
      // Start an independent asynchronous check on the length.
      asyncTestStarted();
      file.length().then((len) {
        Expect.equals(expectedLength, len);
        asyncTestDone('testReadWriteStreamLargeFile: length check');
      });

      List<int> inputBuffer =
          new List<int>.fixedLength(expectedLength + 100000);
      // Immediate read should read 0 bytes.
      Expect.equals(0, input.available());
      Expect.equals(false, input.closed);
      int bytesRead = input.readInto(inputBuffer);
      Expect.equals(0, bytesRead);
      Expect.equals(0, input.available());
      Expect.isFalse(input.closed);
      input.onError = (e) {
        print('Error handler called on input in testReadWriteStreamLargeFile');
        print('with error $e');
        throw e;
      };
      input.onData = () {
        Expect.isFalse(input.closed);
        bytesRead = input.readInto(inputBuffer, position,
                                   inputBuffer.length - position);
        position += bytesRead;
        // The buffer is large enough to hold all available data.
        // So there should be no data left to read.
        Expect.equals(0, input.available());
        bytesRead = input.readInto(inputBuffer, position,
                                   expectedLength - position);
        Expect.equals(0, bytesRead);
        Expect.equals(0, input.available());
        Expect.isFalse(input.closed);
      };
      input.onClosed = () {
        Expect.equals(0, input.available());
        Expect.isTrue(input.closed);
        input.close();  // This should be safe to call.

        Expect.equals(expectedLength, position);
        for (int i = 0; i < position; ++i) {
          Expect.equals(buffer[i % buffer.length], inputBuffer[i]);
        }

        Future testPipeDone = testPipe(file, buffer);

        Future futureDeleted = testPipeDone.then((ignored) => file.delete());
        futureDeleted.then((ignored) {
            asyncTestDone('testReadWriteStreamLargeFile: main test');
        }).catchError((e) {
          print('Exception while deleting ReadWriteStreamLargeFile file');
          print('Exception $e');
        });
      };
      // Try a read again after handlers are set.
      bytesRead = input.readInto(inputBuffer);
      Expect.equals(0, bytesRead);
      Expect.equals(0, input.available());
      Expect.isFalse(input.closed);
    };
  }

  static Future testPipe(File file, buffer) {
    String outputFilename = '${file.name}_copy';
    File outputFile = new File(outputFilename);
    InputStream input = file.openInputStream();
    OutputStream output = outputFile.openOutputStream();
    input.pipe(output);
    Completer done = new Completer();
    output.onClosed = () {
      InputStream copy = outputFile.openInputStream();
      int position = 0;
      copy.onData = () {
        var data;
        while ((data = copy.read()) != null) {
          for (int value in data) {
            Expect.equals(buffer[position % buffer.length], value);
            position++;
          }
        }
      };
      copy.onClosed = () {
        Expect.equals(2 * buffer.length, position);
        outputFile.delete().then((ignore) { done.complete(null); });
      };
    };
    return done.future;
  }

  static void testRead() {
    ReceivePort port = new ReceivePort();
    // Read a file and check part of it's contents.
    String filename = getFilename("bin/file_test.cc");
    File file = new File(filename);
    file.open(FileMode.READ).then((RandomAccessFile file) {
      List<int> buffer = new List<int>.fixedLength(10);
      file.readList(buffer, 0, 5).then((bytes_read) {
        Expect.equals(5, bytes_read);
        file.readList(buffer, 5, 5).then((bytes_read) {
          Expect.equals(5, bytes_read);
          Expect.equals(47, buffer[0]);  // represents '/' in the file.
          Expect.equals(47, buffer[1]);  // represents '/' in the file.
          Expect.equals(32, buffer[2]);  // represents ' ' in the file.
          Expect.equals(67, buffer[3]);  // represents 'C' in the file.
          Expect.equals(111, buffer[4]);  // represents 'o' in the file.
          Expect.equals(112, buffer[5]);  // represents 'p' in the file.
          Expect.equals(121, buffer[6]);  // represents 'y' in the file.
          Expect.equals(114, buffer[7]);  // represents 'r' in the file.
          Expect.equals(105, buffer[8]);  // represents 'i' in the file.
          Expect.equals(103, buffer[9]);  // represents 'g' in the file.
          file.close().then((ignore) => port.close());
        });
      });
    });
  }

  static void testReadSync() {
    // Read a file and check part of it's contents.
    String filename = getFilename("bin/file_test.cc");
    RandomAccessFile file = (new File(filename)).openSync();
    List<int> buffer = new List<int>.fixedLength(42);
    int bytes_read = 0;
    bytes_read = file.readListSync(buffer, 0, 12);
    Expect.equals(12, bytes_read);
    bytes_read = file.readListSync(buffer, 12, 30);
    Expect.equals(30, bytes_read);
    Expect.equals(47, buffer[0]);  // represents '/' in the file.
    Expect.equals(47, buffer[1]);  // represents '/' in the file.
    Expect.equals(32, buffer[2]);  // represents ' ' in the file.
    Expect.equals(67, buffer[3]);  // represents 'C' in the file.
    Expect.equals(111, buffer[4]);  // represents 'o' in the file.
    Expect.equals(112, buffer[5]);  // represents 'p' in the file.
    Expect.equals(121, buffer[6]);  // represents 'y' in the file.
    Expect.equals(114, buffer[7]);  // represents 'r' in the file.
    Expect.equals(105, buffer[8]);  // represents 'i' in the file.
    Expect.equals(103, buffer[9]);  // represents 'g' in the file.
    Expect.equals(104, buffer[10]);  // represents 'h' in the file.
    Expect.equals(116, buffer[11]);  // represents 't' in the file.
  }

  // Test for file read and write functionality.
  static void testReadWrite() {
    // Read a file.
    String inFilename = getFilename("tests/vm/data/fixed_length_file");
    final File file = new File(inFilename);
    file.open(FileMode.READ).then((openedFile) {
      List<int> buffer1 = new List<int>.fixedLength(42);
      openedFile.readList(buffer1, 0, 42).then((bytes_read) {
        Expect.equals(42, bytes_read);
        openedFile.close().then((ignore) {
          // Write the contents of the file just read into another file.
          String outFilename = tempDirectory.path.concat("/out_read_write");
          final File file2 = new File(outFilename);
          file2.create().then((ignore) {
            file2.fullPath().then((s) {
              Expect.isTrue(new File(s).existsSync());
              if (s[0] != '/' && s[0] != '\\' && s[1] != ':') {
                Expect.fail("Not a full path");
              }
              file2.open(FileMode.WRITE).then((openedFile2) {
                openedFile2.writeList(buffer1, 0, bytes_read).then((ignore) {
                  openedFile2.close().then((ignore) {
                    List<int> buffer2 = new List<int>.fixedLength(bytes_read);
                    final File file3 = new File(outFilename);
                    file3.open(FileMode.READ).then((openedFile3) {
                      openedFile3.readList(buffer2, 0, 42).then((bytes_read) {
                        Expect.equals(42, bytes_read);
                        openedFile3.close().then((ignore) {
                          // Now compare the two buffers to check if they
                          // are identical.
                          Expect.equals(buffer1.length, buffer2.length);
                          for (int i = 0; i < buffer1.length; i++) {
                            Expect.equals(buffer1[i],  buffer2[i]);
                          }
                          // Delete the output file.
                          final file4 = file3;
                          file4.delete().then((ignore) {
                            file4.exists().then((exists) {
                              Expect.isFalse(exists);
                              asyncTestDone("testReadWrite");
                            });
                          });
                        });
                      });
                    });
                  });
                });
              });
            });
          });
        });
      });
    });
    asyncTestStarted();
  }

  static void testWriteAppend() {
    String content = "foobar";
    String filename = tempDirectory.path.concat("/write_append");
    File file = new File(filename);
    file.createSync();
    Expect.isTrue(new File(filename).existsSync());
    List<int> buffer = content.charCodes;
    RandomAccessFile openedFile = file.openSync(FileMode.WRITE);
    openedFile.writeListSync(buffer, 0, buffer.length);
    openedFile.closeSync();
    // Reopen the file in write mode to ensure that we overwrite the content.
    openedFile = (new File(filename)).openSync(FileMode.WRITE);
    openedFile.writeListSync(buffer, 0, buffer.length);
    Expect.equals(content.length, openedFile.lengthSync());
    openedFile.closeSync();
    // Open the file in append mode and ensure that we do not overwrite
    // the existing content.
    openedFile = (new File(filename)).openSync(FileMode.APPEND);
    openedFile.writeListSync(buffer, 0, buffer.length);
    Expect.equals(content.length * 2, openedFile.lengthSync());
    openedFile.closeSync();
    file.deleteSync();
  }

  static void testOutputStreamWriteAppend() {
    String content = "foobar";
    String filename = tempDirectory.path.concat("/outstream_write_append");
    File file = new File(filename);
    file.createSync();
    List<int> buffer = content.charCodes;
    OutputStream outStream = file.openOutputStream();
    outStream.write(buffer);
    outStream.onNoPendingWrites = () {
      outStream.close();
      outStream.onClosed = () {
        File file2 = new File(filename);
        OutputStream appendingOutput =
            file2.openOutputStream(FileMode.APPEND);
        appendingOutput.write(buffer);
        appendingOutput.onNoPendingWrites = () {
          appendingOutput.close();
          appendingOutput.onClosed = () {
            File file3 = new File(filename);
            file3.open(FileMode.READ).then((RandomAccessFile openedFile) {
              openedFile.length().then((int length) {
                Expect.equals(content.length * 2, length);
                openedFile.close().then((ignore) {
                  file3.delete().then((ignore) {
                    asyncTestDone("testOutputStreamWriteAppend");
                  });
                });
              });
            });
          };
        };
      };
    };
    asyncTestStarted();
  }

  // Test for file read and write functionality.
  static void testOutputStreamWriteString() {
    String content = "foobar";
    String filename = tempDirectory.path.concat("/outstream_write_string");
    File file = new File(filename);
    file.createSync();
    List<int> buffer = content.charCodes;
    OutputStream outStream = file.openOutputStream();
    outStream.writeString("abcdABCD");
    outStream.writeString("abcdABCD", Encoding.UTF_8);
    outStream.writeString("abcdABCD", Encoding.ISO_8859_1);
    outStream.writeString("abcdABCD", Encoding.ASCII);
    outStream.writeString("æøå", Encoding.UTF_8);
    outStream.onNoPendingWrites = () {
      outStream.close();
      outStream.onClosed = () {
        RandomAccessFile raf = file.openSync();
        Expect.equals(38, raf.lengthSync());
        raf.close().then((ignore) {
          asyncTestDone("testOutputStreamWriteString");
        });
      };
    };
    asyncTestStarted();
  }


  static void testReadWriteSync() {
    // Read a file.
    String inFilename = getFilename("tests/vm/data/fixed_length_file");
    RandomAccessFile file = (new File(inFilename)).openSync();
    List<int> buffer1 = new List<int>.fixedLength(42);
    int bytes_read = 0;
    int bytes_written = 0;
    bytes_read = file.readListSync(buffer1, 0, 42);
    Expect.equals(42, bytes_read);
    file.closeSync();
    // Write the contents of the file just read into another file.
    String outFilename = tempDirectory.path.concat("/out_read_write_sync");
    File outFile = new File(outFilename);
    outFile.createSync();
    String path = outFile.fullPathSync();
    if (path[0] != '/' && path[0] != '\\' && path[1] != ':') {
      Expect.fail("Not a full path");
    }
    Expect.isTrue(new File(path).existsSync());
    RandomAccessFile openedFile = outFile.openSync(FileMode.WRITE);
    openedFile.writeListSync(buffer1, 0, bytes_read);
    openedFile.closeSync();
    // Now read the contents of the file just written.
    List<int> buffer2 = new List<int>.fixedLength(bytes_read);
    openedFile = (new File(outFilename)).openSync();
    bytes_read = openedFile.readListSync(buffer2, 0, 42);
    Expect.equals(42, bytes_read);
    openedFile.closeSync();
    // Now compare the two buffers to check if they are identical.
    Expect.equals(buffer1.length, buffer2.length);
    for (int i = 0; i < buffer1.length; i++) {
      Expect.equals(buffer1[i],  buffer2[i]);
    }
    // Delete the output file.
    outFile.deleteSync();
    Expect.isFalse(outFile.existsSync());
  }

  static void testReadEmptyFileSync() {
    String fileName = tempDirectory.path.concat("/empty_file_sync");
    File file = new File(fileName);
    file.createSync();
    RandomAccessFile openedFile = file.openSync();
    Expect.equals(-1, openedFile.readByteSync());
    openedFile.closeSync();
    file.deleteSync();
  }

  static void testReadEmptyFile() {
    String fileName = tempDirectory.path.concat("/empty_file");
    File file = new File(fileName);
    asyncTestStarted();
    file.create().then((ignore) {
      file.open(FileMode.READ).then((RandomAccessFile openedFile) {
        var readByteFuture = openedFile.readByte();
        readByteFuture.then((int byte) {
          Expect.equals(-1, byte);
          openedFile.close().then((ignore) {
            asyncTestDone("testReadEmptyFile");
          });
        });
      });
    });
  }

  // Test for file write of different types of lists.
  static void testWriteVariousLists() {
    asyncTestStarted();
    final String fileName = "${tempDirectory.path}/testWriteVariousLists";
    final File file = new File(fileName);
    file.create().then((ignore) {
      file.open(FileMode.WRITE).then((RandomAccessFile openedFile) {
        // Write bytes from 0 to 7.
        openedFile.writeList([0], 0, 1);
        openedFile.writeList(const [1], 0, 1);
        openedFile.writeList(new MyListOfOneElement(2), 0, 1);
        var x = 12345678901234567890123456789012345678901234567890;
        var y = 12345678901234567890123456789012345678901234567893;
        openedFile.writeList([y - x], 0, 1);
        openedFile.writeList([260], 0, 1);  // 260 = 256 + 4 = 0x104.
        openedFile.writeList(const [261], 0, 1);
        openedFile.writeList(new MyListOfOneElement(262), 0, 1);
        x = 12345678901234567890123456789012345678901234567890;
        y = 12345678901234567890123456789012345678901234568153;
        openedFile.writeList([y - x], 0, 1).then((ignore) {
          openedFile.close().then((ignore) {
            // Check the written bytes.
            final File file2 = new File(fileName);
            var openedFile2 = file2.openSync();
            var length = openedFile2.lengthSync();
            Expect.equals(8, length);
            List data = new List.fixedLength(length);
            openedFile2.readListSync(data, 0, length);
            for (var i = 0; i < data.length; i++) {
              Expect.equals(i, data[i]);
            }
            openedFile2.closeSync();
            file2.deleteSync();
            asyncTestDone("testWriteVariousLists");
          });
        });
      });
    });
  }

  static void testDirectory() {
    asyncTestStarted();

    // Port to verify that the test completes.
    var port = new ReceivePort();
    port.receive((message, replyTo) {
      port.close();
      Expect.equals(1, message);
      asyncTestDone("testDirectory");
    });

    var tempDir = tempDirectory.path;
    var file = new File("${tempDir}/testDirectory");
    var errors = 0;
    var dirFuture = file.directory();
    dirFuture.then((d) => Expect.fail("non-existing file"))
    .catchError((e) {
      file.create().then((ignore) {
        file.directory().then((Directory d) {
          d.exists().then((exists) {
            Expect.isTrue(exists);
            Expect.isTrue(d.path.endsWith(tempDir));
            file.delete().then((ignore) {
              var fileDir = new File(".");
              var dirFuture2 = fileDir.directory();
              dirFuture2.then((d) => Expect.fail("non-existing file"))
              .catchError((e) {
                var fileDir = new File(tempDir);
                var dirFuture3 = fileDir.directory();
                dirFuture3.then((d) => Expect.fail("non-existing file"))
                .catchError((e) {
                  port.toSendPort().send(1);
                });
              });
            });
          });
        });
      });
    });
  }

  static void testDirectorySync() {
    var tempDir = tempDirectory.path;
    var file = new File("${tempDir}/testDirectorySync");
    // Non-existing file should throw exception.
    Expect.throws(file.directorySync, (e) { return e is FileIOException; });
    file.createSync();
    // Check that the path of the returned directory is the temp directory.
    Directory d = file.directorySync();
    Expect.isTrue(d.existsSync());
    Expect.isTrue(d.path.endsWith(tempDir));
    file.deleteSync();
    // Directories should throw exception.
    var file_dir = new File(".");
    Expect.throws(file_dir.directorySync, (e) { return e is FileIOException; });
    file_dir = new File(tempDir);
    Expect.throws(file_dir.directorySync, (e) { return e is FileIOException; });
  }

  // Test for file length functionality.
  static void testLength() {
    var port = new ReceivePort();
    String filename = getFilename("tests/vm/data/fixed_length_file");
    File file = new File(filename);
    RandomAccessFile openedFile = file.openSync();
    openedFile.length().then((length) {
      Expect.equals(42, length);
      openedFile.close().then((ignore) => port.close());
    });
    file.length().then((length) {
      Expect.equals(42, length);
    });
  }

  static void testLengthSync() {
    String filename = getFilename("tests/vm/data/fixed_length_file");
    File file = new File(filename);
    RandomAccessFile openedFile = file.openSync();
    Expect.equals(42, file.lengthSync());
    Expect.equals(42, openedFile.lengthSync());
    openedFile.closeSync();
  }

  // Test for file position functionality.
  static void testPosition() {
    var port = new ReceivePort();
    String filename = getFilename("tests/vm/data/fixed_length_file");
    RandomAccessFile input = (new File(filename)).openSync();
    input.position().then((position) {
      Expect.equals(0, position);
      List<int> buffer = new List<int>.fixedLength(100);
      input.readList(buffer, 0, 12).then((bytes_read) {
        input.position().then((position) {
          Expect.equals(12, position);
          input.readList(buffer, 12, 6).then((bytes_read) {
            input.position().then((position) {
              Expect.equals(18, position);
              input.setPosition(8).then((ignore) {
                input.position().then((position) {
                  Expect.equals(8, position);
                  input.close().then((ignore) => port.close());
                });
              });
            });
          });
        });
      });
    });
  }

  static void testPositionSync() {
    String filename = getFilename("tests/vm/data/fixed_length_file");
    RandomAccessFile input = (new File(filename)).openSync();
    Expect.equals(0, input.positionSync());
    List<int> buffer = new List<int>.fixedLength(100);
    input.readListSync(buffer, 0, 12);
    Expect.equals(12, input.positionSync());
    input.readListSync(buffer, 12, 6);
    Expect.equals(18, input.positionSync());
    input.setPositionSync(8);
    Expect.equals(8, input.positionSync());
    input.closeSync();
  }

  static void testTruncate() {
    File file = new File(tempDirectory.path.concat("/out_truncate"));
    List buffer = const [65, 65, 65, 65, 65, 65, 65, 65, 65, 65];
    file.open(FileMode.WRITE).then((RandomAccessFile openedFile) {
      openedFile.writeList(buffer, 0, 10).then((ignore) {
        openedFile.length().then((length) {
          Expect.equals(10, length);
          openedFile.truncate(5).then((ignore) {
            openedFile.length().then((length) {
              Expect.equals(5, length);
              openedFile.close().then((ignore) {
                file.delete().then((ignore) {
                  file.exists().then((exists) {
                    Expect.isFalse(exists);
                    asyncTestDone("testTruncate");
                  });
                });
              });
            });
          });
        });
      });
    });
    asyncTestStarted();
  }

  static void testTruncateSync() {
    File file = new File(tempDirectory.path.concat("/out_truncate_sync"));
    List buffer = const [65, 65, 65, 65, 65, 65, 65, 65, 65, 65];
    RandomAccessFile openedFile = file.openSync(FileMode.WRITE);
    openedFile.writeListSync(buffer, 0, 10);
    Expect.equals(10, openedFile.lengthSync());
    openedFile.truncateSync(5);
    Expect.equals(5, openedFile.lengthSync());
    openedFile.closeSync();
    file.deleteSync();
    Expect.isFalse(file.existsSync());
  }

  // Tests exception handling after file was closed.
  static void testCloseException() {
    bool exceptionCaught = false;
    bool wrongExceptionCaught = false;
    File input = new File(tempDirectory.path.concat("/out_close_exception"));
    RandomAccessFile openedFile = input.openSync(FileMode.WRITE);
    openedFile.closeSync();
    try {
      openedFile.readByteSync();
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      openedFile.writeByteSync(1);
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      openedFile.writeStringSync("Test");
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(100);
      openedFile.readListSync(buffer, 0, 10);
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(100);
      openedFile.writeListSync(buffer, 0, 10);
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      openedFile.positionSync();
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      openedFile.lengthSync();
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      openedFile.flushSync();
    } on FileIOException catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    input.deleteSync();
  }

  // Tests stream exception handling after file was closed.
  static void testCloseExceptionStream() {
    asyncTestStarted();
    List<int> buffer = new List<int>.fixedLength(42);
    File file =
        new File(tempDirectory.path.concat("/out_close_exception_stream"));
    file.createSync();
    InputStream input = file.openInputStream();
    input.onClosed = () {
      Expect.isTrue(input.closed);
      Expect.equals(0, input.readInto(buffer, 0, 12));
      OutputStream output = file.openOutputStream();
      output.close();
      Expect.throws(() => output.writeFrom(buffer, 0, 12));
      output.onClosed = () {
        file.deleteSync();
        asyncTestDone("testCloseExceptionStream");
      };
    };
  }

  // Tests buffer out of bounds exception.
  static void testBufferOutOfBoundsException() {
    bool exceptionCaught = false;
    bool wrongExceptionCaught = false;
    File file =
        new File(tempDirectory.path.concat("/out_buffer_out_of_bounds"));
    RandomAccessFile openedFile = file.openSync(FileMode.WRITE);
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.readListSync(buffer, 0, 12);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.readListSync(buffer, 6, 6);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.readListSync(buffer, -1, 1);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.readListSync(buffer, 0, -1);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.writeListSync(buffer, 0, 12);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.writeListSync(buffer, 6, 6);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.writeListSync(buffer, -1, 1);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    exceptionCaught = false;
    try {
      List<int> buffer = new List<int>.fixedLength(10);
      openedFile.writeListSync(buffer, 0, -1);
    } on RangeError catch (ex) {
      exceptionCaught = true;
    } on Exception catch (ex) {
      wrongExceptionCaught = true;
    }
    Expect.equals(true, exceptionCaught);
    Expect.equals(true, !wrongExceptionCaught);
    openedFile.closeSync();
    file.deleteSync();
  }

  static void testOpenDirectoryAsFile() {
    var f = new File('.');
    var future = f.open(FileMode.READ);
    future.then((r) => Expect.fail('Directory opened as file'))
          .catchError((e) {});
  }

  static void testOpenDirectoryAsFileSync() {
    var f = new File('.');
    try {
      f.openSync();
      Expect.fail("Expected exception opening directory as file");
    } catch (e) {
      Expect.isTrue(e is FileIOException);
    }
  }

  static void testOpenFileFromPath() {
    var name = getFilename("tests/vm/data/fixed_length_file");
    var path = new Path(name);
    var f = new File.fromPath(path);
    Expect.isTrue(f.existsSync());
    name = f.fullPathSync();
    path = new Path(name);
    var g = new File.fromPath(path);
    Expect.isTrue(g.existsSync());
    Expect.equals(name, g.fullPathSync());
  }

  static void testReadAsBytes() {
    var port = new ReceivePort();
    port.receive((result, replyTo) {
      port.close();
      Expect.equals(42, result);
    });
    var name = getFilename("tests/vm/data/fixed_length_file");
    var f = new File(name);
    f.readAsBytes().then((bytes) {
      Expect.isTrue(new String.fromCharCodes(bytes).endsWith("42 bytes."));
      port.toSendPort().send(bytes.length);
    });
  }

  static void testReadAsBytesEmptyFile() {
    var port = new ReceivePort();
    port.receive((result, replyTo) {
      port.close();
      Expect.equals(0, result);
    });
    var name = getFilename("tests/vm/data/empty_file");
    var f = new File(name);
    f.readAsBytes().then((bytes) {
      port.toSendPort().send(bytes.length);
    });
  }

  static void testReadAsBytesSync() {
    var name = getFilename("tests/vm/data/fixed_length_file");
    var bytes = new File(name).readAsBytesSync();
    Expect.isTrue(new String.fromCharCodes(bytes).endsWith("42 bytes."));
    Expect.equals(bytes.length, 42);
  }

  static void testReadAsBytesSyncEmptyFile() {
    var name = getFilename("tests/vm/data/empty_file");
    var bytes = new File(name).readAsBytesSync();
    Expect.equals(bytes.length, 0);
  }

  static void testReadAsText() {
    var port = new ReceivePort();
    port.receive((result, replyTo) {
      port.close();
      Expect.equals(1, result);
    });
    var name = getFilename("tests/vm/data/fixed_length_file");
    var f = new File(name);
    f.readAsString(Encoding.UTF_8).then((text) {
      Expect.isTrue(text.endsWith("42 bytes."));
      Expect.equals(42, text.length);
      var name = getDataFilename("tests/standalone/io/read_as_text.dat");
      var f = new File(name);
      f.readAsString(Encoding.UTF_8).then((text) {
        Expect.equals(6, text.length);
        var expected = [955, 120, 46, 32, 120, 10];
        Expect.listEquals(expected, text.charCodes);
        f.readAsString(Encoding.ISO_8859_1).then((text) {
          Expect.equals(7, text.length);
          var expected = [206, 187, 120, 46, 32, 120, 10];
          Expect.listEquals(expected, text.charCodes);
          var readAsStringFuture = f.readAsString(Encoding.ASCII);
          readAsStringFuture.then((text) {
            Expect.fail("Non-ascii char should cause error");
          }).catchError((e) {
            port.toSendPort().send(1);
          });
        });
      });
    });
  }

  static void testReadAsTextEmptyFile() {
    var port = new ReceivePort();
    port.receive((result, replyTo) {
      port.close();
      Expect.equals(0, result);
    });
    var name = getFilename("tests/vm/data/empty_file");
    var f = new File(name);
    f.readAsString(Encoding.UTF_8).then((text) {
      port.toSendPort().send(text.length);
      return true;
    });
  }

  static void testReadAsTextSync() {
    var name = getFilename("tests/vm/data/fixed_length_file");
    var text = new File(name).readAsStringSync();
    Expect.isTrue(text.endsWith("42 bytes."));
    Expect.equals(42, text.length);
    name = getDataFilename("tests/standalone/io/read_as_text.dat");
    text = new File(name).readAsStringSync();
    Expect.equals(6, text.length);
    var expected = [955, 120, 46, 32, 120, 10];
    Expect.listEquals(expected, text.charCodes);
    Expect.throws(() { new File(name).readAsStringSync(Encoding.ASCII); });
    text = new File(name).readAsStringSync(Encoding.ISO_8859_1);
    expected = [206, 187, 120, 46, 32, 120, 10];
    Expect.equals(7, text.length);
    Expect.listEquals(expected, text.charCodes);
  }

  static void testReadAsTextSyncEmptyFile() {
    var name = getFilename("tests/vm/data/empty_file");
    var text = new File(name).readAsStringSync();
    Expect.equals(0, text.length);
  }

  static void testReadAsLines() {
    var port = new ReceivePort();
    port.receive((result, replyTo) {
      port.close();
      Expect.equals(42, result);
    });
    var name = getFilename("tests/vm/data/fixed_length_file");
    var f = new File(name);
    f.readAsLines(Encoding.UTF_8).then((lines) {
      Expect.equals(1, lines.length);
      var line = lines[0];
      Expect.isTrue(line.endsWith("42 bytes."));
      port.toSendPort().send(line.length);
    });
  }

  static void testReadAsLinesSync() {
    var name = getFilename("tests/vm/data/fixed_length_file");
    var lines = new File(name).readAsLinesSync();
    Expect.equals(1, lines.length);
    var line = lines[0];
    Expect.isTrue(line.endsWith("42 bytes."));
    Expect.equals(42, line.length);
    name = getDataFilename("tests/standalone/io/readline_test1.dat");
    lines = new File(name).readAsLinesSync();
    Expect.equals(10, lines.length);
  }


  static void testReadAsErrors() {
    var port = new ReceivePort();
    port.receive((message, _) {
      port.close();
      Expect.equals(1, message);
    });
    var f = new File('.');
    Expect.throws(f.readAsBytesSync, (e) => e is FileIOException);
    Expect.throws(f.readAsStringSync, (e) => e is FileIOException);
    Expect.throws(f.readAsLinesSync, (e) => e is FileIOException);
    var readAsBytesFuture = f.readAsBytes();
    readAsBytesFuture.then((bytes) => Expect.fail("no bytes expected"))
    .catchError((e) {
      var readAsStringFuture = f.readAsString(Encoding.UTF_8);
      readAsStringFuture.then((text) => Expect.fail("no text expected"))
      .catchError((e) {
        var readAsLinesFuture = f.readAsLines(Encoding.UTF_8);
        readAsLinesFuture.then((lines) => Expect.fail("no lines expected"))
        .catchError((e) {
          port.toSendPort().send(1);
        });
      });
    });
  }

  static void testLastModified() {
    var port = new ReceivePort();
    new File(new Options().executable).lastModified().then((modified) {
      Expect.isTrue(modified is DateTime);
      Expect.isTrue(modified < new DateTime.now());
      port.close();
    });
  }

  static void testLastModifiedSync() {
    var modified = new File(new Options().executable).lastModifiedSync();
    Expect.isTrue(modified is DateTime);
    Expect.isTrue(modified < new DateTime.now());
  }

  // Test that opens the same file for writing then for appending to test
  // that the file is not truncated when opened for appending.
  static void testAppend() {
    var file = new File('${tempDirectory.path}/out_append');
    file.open(FileMode.WRITE).then((openedFile) {
      openedFile.writeString("asdf").then((ignore) {
        openedFile.close().then((ignore) {
          file.open(FileMode.APPEND).then((openedFile) {
            openedFile.length().then((length) {
              Expect.equals(4, length);
              openedFile.writeString("asdf").then((ignore) {
                openedFile.length().then((length) {
                  Expect.equals(8, length);
                  openedFile.close().then((ignore) {
                    file.delete().then((ignore) {
                      file.exists().then((exists) {
                        Expect.isFalse(exists);
                        asyncTestDone("testAppend");
                      });
                    });
                  });
                });
              });
            });
          });
        });
      });
    });
    asyncTestStarted();
  }

  static void testAppendSync() {
    var file = new File('${tempDirectory.path}/out_append_sync');
    var openedFile = file.openSync(FileMode.WRITE);
    openedFile.writeStringSync("asdf");
    Expect.equals(4, openedFile.lengthSync());
    openedFile.closeSync();
    openedFile = file.openSync(FileMode.WRITE);
    openedFile.setPositionSync(4);
    openedFile.writeStringSync("asdf");
    Expect.equals(8, openedFile.lengthSync());
    openedFile.closeSync();
    file.deleteSync();
    Expect.isFalse(file.existsSync());
  }

  static void testWriteStringUtf8() {
    var file = new File('${tempDirectory.path}/out_write_string');
    var string = new String.fromCharCodes([0x192]);
    file.open(FileMode.WRITE).then((openedFile) {
      openedFile.writeString(string).then((_) {
        openedFile.length().then((l) {
          Expect.equals(2, l);
          openedFile.close().then((_) {
            file.open(FileMode.APPEND).then((openedFile) {
              openedFile.setPosition(2).then((_) {
                openedFile.writeString(string).then((_) {
                  openedFile.length().then((l) {
                    Expect.equals(4, l);
                    openedFile.close().then((_) {
                      file.readAsString().then((readBack) {
                        Expect.stringEquals(readBack, '$string$string');
                        file.delete().then((_) {
                          file.exists().then((e) {
                           Expect.isFalse(e);
                           asyncTestDone("testWriteStringUtf8");
                          });
                        });
                      });
                    });
                  });
                });
              });
            });
          });
        });
      });
    });
    asyncTestStarted();
  }

  static void testWriteStringUtf8Sync() {
    var file = new File('${tempDirectory.path}/out_write_string_sync');
    var string = new String.fromCharCodes([0x192]);
    var openedFile = file.openSync(FileMode.WRITE);
    openedFile.writeStringSync(string);
    Expect.equals(2, openedFile.lengthSync());
    openedFile.closeSync();
    openedFile = file.openSync(FileMode.APPEND);
    openedFile.setPositionSync(2);
    openedFile.writeStringSync(string);
    Expect.equals(4, openedFile.lengthSync());
    openedFile.closeSync();
    var readBack = file.readAsStringSync();
    Expect.stringEquals(readBack, '$string$string');
    file.deleteSync();
    Expect.isFalse(file.existsSync());
  }

  // Helper method to be able to run the test from the runtime
  // directory, or the top directory.
  static String getFilename(String path) =>
      new File(path).existsSync() ? path : 'runtime/$path';

  static String getDataFilename(String path) =>
      new File(path).existsSync() ? path : '../$path';

  // Main test entrypoint.
  static testMain() {
    port = new ReceivePort();
    testRead();
    testReadSync();
    testReadStream();
    testLength();
    testLengthSync();
    testPosition();
    testPositionSync();
    testOpenDirectoryAsFile();
    testOpenDirectoryAsFileSync();
    testOpenFileFromPath();
    testReadAsBytes();
    testReadAsBytesEmptyFile();
    testReadAsBytesSync();
    testReadAsBytesSyncEmptyFile();
    testReadAsText();
    testReadAsTextEmptyFile();
    testReadAsTextSync();
    testReadAsTextSyncEmptyFile();
    testReadAsLines();
    testReadAsLinesSync();
    testReadAsErrors();
    testLastModified();
    testLastModifiedSync();

    createTempDirectory(() {
      testReadWrite();
      testReadWriteSync();
      testReadWriteStream();
      testReadEmptyFileSync();
      testReadEmptyFile();
      testReadWriteStreamLargeFile();
      testTruncate();
      testTruncateSync();
      testCloseException();
      testCloseExceptionStream();
      testBufferOutOfBoundsException();
      testAppend();
      testAppendSync();
      testWriteAppend();
      testOutputStreamWriteAppend();
      testOutputStreamWriteString();
      testWriteVariousLists();
      testDirectory();
      testDirectorySync();
      testWriteStringUtf8();
      testWriteStringUtf8Sync();
    });
  }
}

main() {
  FileTest.testMain();
}
