/*
 * Copyright 2012 Dart project authors.
 * 
 * Licensed under the Eclipse Public License v1.0 (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 * 
 * http://www.eclipse.org/legal/epl-v10.html
 * 
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */
package com.google.dart.tools.core.analysis;

import com.google.dart.compiler.PackageLibraryManager;
import com.google.dart.tools.core.DartCore;
import com.google.dart.tools.core.model.DartSdkManager;

import static com.google.dart.tools.core.analysis.AnalysisUtility.equalsOrContains;
import static com.google.dart.tools.core.analysis.AnalysisUtility.toFile;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.Reader;
import java.io.Writer;
import java.net.URI;
import java.net.URISyntaxException;
import java.util.Collection;
import java.util.HashSet;
import java.util.Iterator;

/**
 * Provides analysis of Dart code for Dart editor
 */
public class AnalysisServer {

  private static final String CACHE_V4_TAG = "v4";
  private static final String CACHE_V3_TAG = "v3";
  private static final String CACHE_V2_TAG = "v2";
  private static final String CACHE_V1_TAG = "v1";
  private static final String ANALYZE_CONTEXT_TAG = AnalyzeContextTask.class.getSimpleName();
  private static final String END_LIBRARIES_TAG = "</end-libraries>";
  private static final String END_QUEUE_TAG = "</end-queue>";

  /**
   * The library files being analyzed by the receiver. Synchronize against this object before
   * accessing it.
   */
  private final HashSet<File> libraryFiles = new HashSet<File>();

  /**
   * The outstanding tasks to be performed.
   */
  private final TaskQueue queue = new TaskQueue();

  /**
   * The object performing analysis tasks
   */
  private final TaskProcessor processor = new TaskProcessor(queue);

  /**
   * A context representing what is "saved on disk". Contents of this object should only be accessed
   * on the background thread.
   */
  private final SavedContext savedContext;

  /**
   * A singleton for background analysis of the savedContext
   */
  private final AnalyzeContextTask savedContextAnalysisTask;

  /**
   * A context representing what is "currently being edited". Contents of this object should only be
   * accessed on the background thread.
   */
  private final EditContext editContext;

  /**
   * Create a new instance that processes analysis tasks on a background thread
   * 
   * @param libraryManager the target (VM, Dartium, JS) against which user libraries are resolved
   */
  public AnalysisServer(PackageLibraryManager libraryManager) {
    if (libraryManager == null) {
      throw new IllegalArgumentException();
    }
    savedContext = new SavedContext(this, libraryManager);
    editContext = new EditContext(this, savedContext, libraryManager);
    savedContextAnalysisTask = new AnalyzeContextTask(this);
  }

  /**
   * Add an object to be notified when there are no tasks queued (or analyzing is <code>false</code>
   * ) and no tasks being performed.
   * 
   * @param listener the object to be notified
   */
  public void addIdleListener(IdleListener listener) {
    processor.addIdleListener(listener);
  }

  /**
   * Analyze the specified library, and keep that analysis current by tracking any changes. Also see
   * {@link Context#resolve(File, ResolveCallback)}.
   * 
   * @param libraryFile the library file (not <code>null</code>)
   */
  public void analyze(File libraryFile) {
    if (!DartSdkManager.getManager().hasSdk()) {
      return;
    }
    if (!libraryFile.isAbsolute()) {
      throw new IllegalArgumentException("File path must be absolute: " + libraryFile);
    }
    if (libraryFile.isDirectory()) {
      throw new IllegalArgumentException("Cannot analyze a directory: " + libraryFile);
    }
    synchronized (libraryFiles) {
      if (libraryFiles.add(libraryFile)) {
        // Append analysis task to the end of the queue so that any user requests take precedence
        queueAnalyzeContext();
      }
    }
  }

  /**
   * Called when file content has been modified or anything in the "packages" directory has changed.
   * Use {@link #discard(File)} if the file or directory content should no longer be analyzed.
   * 
   * @param file the file or directory (not <code>null</code>)
   */
  public void changed(File file) {
    file = file.getAbsoluteFile();
    queueNewTask(new FileChangedTask(this, file));
  }

  /**
   * Stop analyzing the specified library or all libraries in the specified directory tree.
   * 
   * @param file the library file (not <code>null</code>)
   */
  public void discard(File file) {
    file = file.getAbsoluteFile();

    // If this is a dart file, then discard the library
    // otherwise, discard all libraries in the specified directory tree
    synchronized (libraryFiles) {
      if (file.isFile() || (!file.exists() && DartCore.isDartLikeFileName(file.getName()))) {
        libraryFiles.remove(file);
      } else {
        Iterator<File> iter = libraryFiles.iterator();
        while (iter.hasNext()) {
          if (equalsOrContains(file, iter.next())) {
            iter.remove();
          }
        }
      }
    }

    queueNewTask(new DiscardTask(this, file));
  }

  /**
   * Answer analysis statistics.
   */
  public String getAnalysisStatus(String message) {
    Collection<PackageContext> packageContexts = savedContext.getPackageContexts();
    int taskCount = queue.size();
    int contextCount = packageContexts.size() + 1;
    int libraryCount = savedContext.getLibraryCount();
    for (PackageContext context : packageContexts) {
      libraryCount += context.getLibraryCount();
    }
    return message + ": " + taskCount + " tasks, " + libraryCount + " libraries, " + contextCount
        + " contexts";
  }

  /**
   * Answer the context containing analysis of Dart source currently being edited
   */
  public EditContext getEditContext() {
    return editContext;
  }

  /**
   * Answer the context containing analysis of Dart source on disk
   */
  public SavedContext getSavedContext() {
    return savedContext;
  }

  /**
   * Answer <code>true</code> if the receiver does not have any queued tasks and the receiver's
   * background thread is waiting for new tasks to be queued.
   */
  public boolean isIdle() {
    return processor.isIdle();
  }

  /**
   * TESTING: Answer <code>true</code> if information about the specified library is cached
   * 
   * @param file the library file (not <code>null</code>)
   */
  public boolean isLibraryCached(File file) {
    return savedContext.getCachedLibrary(file) != null;
  }

  /**
   * TESTING: Answer <code>true</code> if specified library has been resolved
   * 
   * @param file the library file (not <code>null</code>)
   */
  public boolean isLibraryResolved(File file) {
    Library library = savedContext.getCachedLibrary(file);
    return library != null && library.getLibraryUnit() != null;
  }

  /**
   * Reload the cached information from the previous session. This method must be called before
   * {@link #start()} has been called when the server is not yet running.
   * 
   * @return <code>true</code> if the cached information was successfully loaded, else
   *         <code>false</code>
   */
  public boolean readCache() {
    File cacheFile = getAnalysisStateFile();
    if (cacheFile.exists()) {
      try {
        BufferedReader reader = new BufferedReader(new FileReader(cacheFile));
        try {
          return readCache(reader);
        } finally {
          try {
            reader.close();
          } catch (IOException e) {
            DartCore.logError("Failed to close analysis cache: " + cacheFile, e);
          }
        }
      } catch (IOException e) {
        DartCore.logError("Failed to read analysis cache: " + cacheFile, e);
        //$FALL-THROUGH$
      }
    }
    reanalyze();
    return false;
  }

  /**
   * Called when all cached information should be discarded and all libraries reanalyzed. No
   * {@link AnalysisListener#discarded(AnalysisEvent)} events are sent when the information is
   * discarded.
   */
  public void reanalyze() {
    queueNewTask(new EverythingChangedTask(this));
  }

  public void removeIdleListener(IdleListener listener) {
    processor.removeIdleListener(listener);
  }

  /**
   * Scan the specified file or recursively scan the specified directory for libraries to analyze.
   * 
   * @param file the file or directory of files to scan (not <code>null</code>).
   * @param milliseconds the number of milliseconds to wait for the scan to complete.
   * @return <code>true</code> if the scan completed in the specified amount of time.
   */
  public boolean scan(File file, long milliseconds) {
    ScanCallback.Sync callback = new ScanCallback.Sync();
    scan(file, callback);
    return callback.waitForScan(milliseconds);
  }

  /**
   * Scan the specified file or recursively scan the specified directory for libraries to analyze.
   * The callback is used to report progress and check if the operation has been canceled.
   * 
   * @param file the file or directory of files to scan (not <code>null</code>)
   * @param callback for reporting progress and canceling the operation.
   */
  public void scan(File file, ScanCallback callback) {
    file = file.getAbsoluteFile();

    // If scanning a "packages" directory, then assume that the packages directory has just been
    // added and create a new application context if one does not already exist
    if (DartCore.isPackagesDirectory(file)) {
      File appDir = file.getParentFile();
      queueNewTask(new DiscardTask(this, appDir));
      queueNewTask(new ScanTask(this, appDir, callback));
    }

    // Otherwise scan the file/directory
    else {
      queueNewTask(new ScanTask(this, file, callback));
    }
  }

  /**
   * Start the background analysis process if it has not already been started
   */
  public void start() {
    if (!queue.setAnalyzing(true)) {
      processor.start();
    }
  }

  /**
   * Start processing low priority tasks when there are no requests
   */
  public void startIdleTaskProcessing() {
    processor.setIdleOperation(new Runnable() {
      @Override
      public void run() {
        while (queue.isAnalyzing() && queue.isEmpty()) {
          Task idleTask = savedContext.getIdleTask();
          if (idleTask == null) {
            break;
          }
          idleTask.perform();
        }
      }
    });
  }

  /**
   * Signal the background analysis thread to stop and wait for up to 5 seconds for it to do so.
   */
  public void stop() {
    if (queue.setAnalyzing(false)) {
      if (!processor.waitForIdle(5000)) {
        DartCore.logError("Gave up waiting for " + getClass().getSimpleName() + " to stop");
      }
    }
  }

  /**
   * Wait up to the specified number of milliseconds for the receiver to be idle. If the specified
   * number is less than or equal to zero, then this method returns immediately.
   * 
   * @param milliseconds the number of milliseconds to wait for idle
   * @return <code>true</code> if the receiver is idle
   */
  public boolean waitForIdle(long milliseconds) {
    return processor.waitForIdle(milliseconds);
  }

  /**
   * Write the cached information to the file used to store analysis state between sessions. This
   * method must be called after {@link #stop()} has been called when the server is not running.
   * 
   * @return <code>true</code> if successful, else false
   */
  public boolean writeCache() {
    File cacheFile = getAnalysisStateFile();
    try {
      BufferedWriter writer = new BufferedWriter(new FileWriter(cacheFile));
      try {
        writeCache(writer);
      } finally {
        try {
          writer.close();
        } catch (IOException e) {
          DartCore.logError("Failed to close analysis cache: " + cacheFile, e);
        }
      }
      return true;
    } catch (IOException e) {
      DartCore.logError("Failed to write analysis cache: " + cacheFile, e);
      return false;
    }
  }

  /**
   * Ensure that all libraries have been analyzed by adding an instance of
   * {@link AnalyzeContextTask} to the end of the queue if it has not already been added.
   */
  protected void queueAnalyzeContext() {
    if (!processor.addLastTaskAndWaitUntilRunning(savedContextAnalysisTask, 50)) {
      DartCore.logInformation("Gave up waiting for " + getClass().getSimpleName()
          + " to start analysis");
    }
  }

  /**
   * Queue sub task to analyze the specified library.
   */
  protected void queueAnalyzeSubTask(File libraryFile) {
    queueSubTask(new AnalyzeLibraryTask(this, libraryFile, null));
  }

  /**
   * Queue sub task to analyze the specified library if it is not already analyzed.
   */
  protected void queueAnalyzeSubTaskIfNew(File libraryFile) {
    synchronized (libraryFiles) {
      if (libraryFiles.add(libraryFile)) {
        queueAnalyzeSubTask(libraryFile);
      }
    }
  }

  /**
   * Answer the library files identified by {@link #analyze(File)}
   * 
   * @return an array of files (not <code>null</code>, contains no <code>null</code>s)
   */
  File[] getTrackedLibraryFiles() {
    synchronized (libraryFiles) {
      return libraryFiles.toArray(new File[libraryFiles.size()]);
    }
  }

  /**
   * Add a priority task to the front of the queue. Should *not* be called by the current task being
   * performed... use {@link #queueSubTask(Task)} instead.
   */
  void queueNewTask(Task task) {
    if (!processor.addNewTaskAndWaitUntilRunning(task, 50)) {
      DartCore.logInformation("Gave up waiting for " + getClass().getSimpleName()
          + " to start analysis");
    }
  }

  /**
   * Used by the current task being performed to add subtasks in a way that will not reduce the
   * priority of new tasks that have been queued while the current task is executing
   */
  void queueSubTask(Task subtask) {
    queue.addSubTask(subtask);
  }

  /**
   * Remove any tasks related to analysis of the specified file or directory and that do not have
   * callbacks. The assumption is that analysis tasks with explicit callbacks are related to user
   * requests and should be preserved. This should only be called from the background thread.
   * 
   * @param discarded the file or directory tree being affected (not <code>null</code>)
   */
  void removeBackgroundTasks(File discarded) {
    queue.removeBackgroundTasks(discarded);
  }

  private File getAnalysisStateFile() {
    return new File(DartCore.getPlugin().getStateLocation().toFile(), "analysis.cache");
  }

  /**
   * Reload the cached information from the specified file. This method must be called before
   * {@link #start()} has been called when the server is not yet running.
   * 
   * @return <code>true</code> if successful
   */
  private boolean readCache(Reader reader) throws IOException {
    if (queue.isAnalyzing()) {
      throw new IllegalStateException();
    }
    CacheReader cacheReader = new CacheReader(reader);

    int version;
    String line = cacheReader.readString();
    if (CACHE_V4_TAG.equals(line)) {
      version = 4;
    } else if (CACHE_V3_TAG.equals(line)) {
      version = 3;
    } else if (CACHE_V2_TAG.equals(line)) {
      version = 2;
    } else if (CACHE_V1_TAG.equals(line)) {
      version = 1;
    } else {
      throw new IOException("Expected cache version " + CACHE_V4_TAG + " but found " + line);
    }

    // Tracked libraries
    cacheReader.readFilePaths(libraryFiles, END_LIBRARIES_TAG);
    if (version == 1) {
      queueAnalyzeContext();
      return true;
    }

    // Cached libraries
    if (version == 2) {
      savedContext.readCache(cacheReader);
      queueAnalyzeContext();
      return true;
    }
    if (version == 3) {
      savedContext.readCache(cacheReader);
    } else {
      int packageContextCount = cacheReader.readInt();
      savedContext.readCache(cacheReader, packageContextCount);
    }

    // Queued tasks
    boolean tasksAdded = false;
    while (true) {
      String path = cacheReader.readString();
      if (path == null) {
        throw new IOException("Expected " + END_QUEUE_TAG + " but found EOF");
      }
      if (path.equals(END_QUEUE_TAG)) {
        break;
      }
      tasksAdded = true;
      if (path.equals(ANALYZE_CONTEXT_TAG)) {
        queueAnalyzeContext();
        continue;
      }
      File libraryFile = new File(path);
      queue.addLastTask(new AnalyzeLibraryTask(this, libraryFile, null));
    }

    // If no queued tasks, then at minimum resolve dart:core
    if (!tasksAdded) {
      URI dartCoreUri = null;
      try {
        dartCoreUri = new URI("dart:core");
      } catch (URISyntaxException e) {
        DartCore.logError(e);
        return true;
      }
      File dartCoreFile = toFile(savedContext, dartCoreUri);
      if (dartCoreFile != null) {
        queue.addLastTask(new AnalyzeLibraryTask(this, dartCoreFile, null));
      }
    }

    return true;
  }

  /**
   * Write the cached information to the specified file. This method must be called after
   * {@link #stop()} has been called when the server is not running.
   */
  private void writeCache(Writer writer) {
    if (queue.isAnalyzing()) {
      throw new IllegalStateException();
    }
    CacheWriter cacheWriter = new CacheWriter(writer);
    cacheWriter.writeString(CACHE_V4_TAG);
    cacheWriter.writeFilePaths(libraryFiles, END_LIBRARIES_TAG);

    savedContext.writeCache(cacheWriter);

    for (Task task : queue.getTasks()) {
      if (task instanceof AnalyzeLibraryTask) {
        AnalyzeLibraryTask libTask = (AnalyzeLibraryTask) task;
        cacheWriter.writeString(libTask.getRootLibraryFile().getPath());
      } else if (task instanceof AnalyzeContextTask) {
        cacheWriter.writeString(ANALYZE_CONTEXT_TAG);
      }
    }
    cacheWriter.writeString(END_QUEUE_TAG);
  }
}
