// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart.io;

/**
 * Output streams are used to write data sequentially to a data
 * destination e.g. a connected socket or an open file.
 *
 * An output stream provides internal buffering of the data written
 * through all calls to [write] and [writeFrom] if data cannot be
 * written immediately to the communication channel. The callback set
 * through [onNoPendingWrites] can be used to to keep the rate of
 * writing in sync with the rate the system can actually write data to
 * the underlying communication channel.
 */
abstract class OutputStream {
  /**
   * Writes the content of [buffer] to the stream. If [copyBuffer] is
   * false ownership of the specified buffer is passed to the system
   * and the caller should not change it afterwards. The default value
   * for [copyBuffer] is true.
   *
   * Returns true if the data could be written to the underlying
   * communication channel immediately. Otherwise the data is buffered
   * by the output stream and will be sent as soon as possible.
   */
  bool write(List<int> buffer, [bool copyBuffer]);

  /**
   * Writes [len] bytes from buffer [buffer] starting at offset
   * [offset] to the output stream. If [offset] is not specified the
   * default is 0. If [len] is not specified the default is the length
   * of the buffer minus [offset] (i.e. writing from offset to the end
   * of the buffer). The system will copy the data to be written so
   * the caller can safely change [buffer] afterwards.
   *
   * Returns true if the data could be written to the underlying
   * communication channel immediately. Otherwise the data is buffered
   * by the output stream and will be sent as soon as possible.
   */
  bool writeFrom(List<int> buffer, [int offset, int len]);

  /**
   * Write a string to the stream using the given [encoding].The
   * default encoding is UTF-8 - [:Encoding.UTF_8:].
   *
   * Returns true if the data could be written to the underlying
   * communication channel immediately. Otherwise the data is buffered
   * by the output stream and will be sent as soon as possible.
   */
  bool writeString(String string, [Encoding encoding]);

  /**
   * Flushes data from any internal buffers as soon as possible. Note
   * that the actual meaning of calling [flush] will depend on the
   * actual type of the underlying communication channel.
   */
  void flush();

  /**
   * Signal that no more data will be written to the output stream. When all
   * buffered data has been written out to the communication channel, the
   * channel will be closed and the [onClosed] callback will be called.
   */
  void close();

  /**
   * Close the communication channel immediately ignoring any buffered
   * data.
   */
  void destroy();

  /**
   * Returns whether the stream has been closed by calling close(). If true, no
   * more data may be written to the output stream, but there still may be
   * buffered data that has not been written to the communication channel. The
   * onClosed handler will only be called once all data has been written out.
   */
  bool get closed;

  /**
   * Sets the handler that gets called when the internal OS buffers
   * have been flushed. This callback can be used to keep the rate of
   * writing in sync with the rate the system can write data to the
   * underlying communication channel.
   */
  void set onNoPendingWrites(void callback());

  /**
   * Sets the handler that gets called when the underlying communication channel
   * has been closed and all the buffered data has been sent.
   */
  void set onClosed(void callback());

  /**
   * Sets the handler that gets called when the underlying
   * communication channel gets into some kind of error situation.
   */
  void set onError(void callback(e));
}

