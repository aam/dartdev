/*
 * Copyright (c) 2012, the Dart project authors.
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
package com.google.dart.tools.deploy;

import com.google.dart.tools.core.DartCore;
import com.google.dart.tools.core.internal.perf.DartEditorCommandLineManager;
import com.google.dart.tools.core.internal.perf.InstrumentationDataCarrier;
import com.google.dart.tools.core.internal.perf.Performance;
import com.google.dart.tools.debug.core.util.ResourceServerManager;

import org.eclipse.core.runtime.Platform;
import org.eclipse.equinox.app.IApplication;
import org.eclipse.equinox.app.IApplicationContext;
import org.eclipse.jface.dialogs.MessageDialog;
import org.eclipse.jface.util.Util;
import org.eclipse.osgi.service.datalocation.Location;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.ui.IWorkbench;
import org.eclipse.ui.PlatformUI;
import org.eclipse.ui.internal.WorkbenchPlugin;

import java.io.File;
import java.io.IOException;
import java.net.ServerSocket;
import java.net.URL;
import java.util.ArrayList;

/**
 * This class controls all aspects of the application's execution.
 */
@SuppressWarnings("restriction")
public class DartIDEApplication implements IApplication {

  private static class ConfirmMultipleEditorsDialog extends MessageDialog {

    public ConfirmMultipleEditorsDialog(Shell parentShell) {
      super(
          parentShell,
          WorkbenchMessages.DartIDEApplication_already_running_dialog_title,
          null,
          WorkbenchMessages.DartIDEApplication_already_running_dialog_msg,
          MessageDialog.QUESTION,
          new String[] {
              WorkbenchMessages.DartIDEApplication_already_running_dialog_run_button,
              WorkbenchMessages.DartIDEApplication_already_running_dialog_cancel_button},
          1);
    }

  }

  private static long approximateStartTime = System.currentTimeMillis();

  /**
   * Value taken from <code>DartEditorCommandLineManager.PERF_FLAG</code>.
   * <p>
   * NOTE that we can't reference DartEditorCommandLineManager directly since doing so will force
   * the dart core bundle to load (prematurely) before we've started the platform in
   * {@link #start(IApplicationContext)}.
   */
  private static final String PERF_FLAG = "-perf"; //DartEditorCommandLineManager.PERF_FLAG //$NON-NLS-1$

  //a flag to cache whether we're measuring perf, used to delay loading of dart core
  private boolean perfFlagSet = false;;

  @Override
  public Object start(IApplicationContext context) throws Exception {

    Display display = PlatformUI.createDisplay();

    if (isWorkspaceServerPortBound()) {

      final Shell shell = WorkbenchPlugin.getSplashShell(display);

      final boolean[] runAnyway = new boolean[1];

      Display.getDefault().syncExec(new Runnable() {
        @Override
        public void run() {
          runAnyway[0] = new ConfirmMultipleEditorsDialog(shell).open() == 0;
        }
      });

      if (!runAnyway[0]) {
        return EXIT_OK;
      }

    }

    try {
      setWorkspaceLocation();

      // processor must be created before we start event loop
      DelayedEventsProcessor processor = new DelayedEventsProcessor(display);

      parseApplicationArgs();

      // Now that the start time of the Editor has been recorded from the command line, we can
      // record the time taken to start the Application
      if (perfFlagSet) {
        System.out.println("Dart Editor build " + DartCore.getBuildIdOrDate()); //$NON-NLS-1$
        Performance.TIME_TO_START_ECLIPSE.log(DartEditorCommandLineManager.getStartTime());
      }

      //Push the approximate start time to the carrier
      InstrumentationDataCarrier.setApproximateStartTime(approximateStartTime);

      int returnCode = PlatformUI.createAndRunWorkbench(display, new ApplicationWorkbenchAdvisor(
          processor));
      if (returnCode == PlatformUI.RETURN_RESTART) {
        return IApplication.EXIT_RESTART;
      } else {
        return IApplication.EXIT_OK;
      }
    } finally {
      display.dispose();
    }
  }

  @Override
  public void stop() {
    if (!PlatformUI.isWorkbenchRunning()) {
      return;
    }
    final IWorkbench workbench = PlatformUI.getWorkbench();
    final Display display = workbench.getDisplay();
    display.syncExec(new Runnable() {
      @Override
      public void run() {
        if (!display.isDisposed()) {
          workbench.close();
        }
      }
    });
  }

  private boolean isWorkspaceServerPortBound() {

    try {
      new ServerSocket(ResourceServerManager.PREFERRED_PORT).close();
    } catch (IOException e) {
      return true;
    }

    return false;
  }

  /**
   * Parse the command line arguments, and store them in {@link DartEditorCommandLineManager}.
   */
  private void parseApplicationArgs() {
    // get application arguments 
    String args[] = Platform.getApplicationArgs();
//    for (String arg : args) {
//      if (arg.equals("-hello")) {
//        System.out.println("Hello");
//      }
//    }
    ArrayList<File> fileSet = new ArrayList<File>(args.length);
    // if we find the PERF_FLAG at any point, set DartPerformance.MEASURE_PERFORMANCE to true
    // for all other arguments, add the argument as a java.io.File, if it is a file that exists,
    // and isn't in the set already
    for (int i = 0; i < args.length; i++) {
      String arg = args[i];
      if (arg.equals(PERF_FLAG)) {
        perfFlagSet = true;
        DartEditorCommandLineManager.MEASURE_PERFORMANCE = true;
        boolean failedToGetStartTime = false;
        // Now record the start time
        if (i + 1 < args.length) {
          // if there is at least one more 
          String nextArg = args[i + 1];
          try {
            long startTime = Long.valueOf(nextArg);
            DartEditorCommandLineManager.setStartTime(startTime);
            i++;
          } catch (NumberFormatException e) {
            failedToGetStartTime = true;
          }
        } else {
          failedToGetStartTime = true;
        }
        if (failedToGetStartTime) {
          System.err.println("Could not retrieve milliseconds from epoch time from command " //$NON-NLS-1$
              + "line, the value should be passed after the \"" //$NON-NLS-1$
              + DartEditorCommandLineManager.PERF_FLAG + "\" flag, recording the start time " //$NON-NLS-1$
              + "of the Dart Editor *now*- at the application init time."); //$NON-NLS-1$
          DartEditorCommandLineManager.setStartTime(System.currentTimeMillis());
        }
        for (int j = 0; j < args.length; j++) {
          String arg2 = args[j];
          if (arg2.equals(DartEditorCommandLineManager.KILL_AFTER_PERF_FLAG)) {
            DartEditorCommandLineManager.KILL_AFTER_PERF = true;
            break;
          }
        }
      } else if (arg.length() > 0 && arg.charAt(0) != '-') {
        File file = new File(arg);
        if (fileSet.contains(file)) {
          continue;
        }
        if (file.exists()) {
          fileSet.add(file);
        } else {
          // else, make an attempt at constructing the file using the relative path to the Dart
          // Editor install directory
          File eclipseInstallFile = new File(Platform.getInstallLocation().getURL().getFile());
          file = new File(eclipseInstallFile, arg);
          if (fileSet.contains(file)) {
            continue;
          }
          if (file.exists()) {
            fileSet.add(file);
          } else {
            System.out.println("Input \"" + arg //$NON-NLS-1$
                + "\" could not be parsed as a valid file, file inputs on the command line " //$NON-NLS-1$
                + "need to be absolute paths for files or folders that exist, or relative to " //$NON-NLS-1$
                + "the directory that the Dart Editor is installed."); //$NON-NLS-1$
          }
        }
      }
    }
    if (perfFlagSet) {
      DartEditorCommandLineManager.setFileSet(fileSet);
    }
  }

  private void setWorkspaceLocation() {
    Location workspaceLocation = Platform.getInstanceLocation();

    if (!workspaceLocation.isSet()) {
      File userHomeDir = new File(System.getProperty("user.home")); //$NON-NLS-1$
      URL workspaceUrl;

      try {
        if (Util.isMac()) {
          workspaceUrl = new URL("file", null, System.getProperty("user.home") //$NON-NLS-1$ //$NON-NLS-2$
              + "/Library/Application Support/DartEditor"); //$NON-NLS-1$
        } else if (Util.isWindows()) {
          File workspaceDir = new File(userHomeDir, "DartEditor"); //$NON-NLS-1$
          workspaceUrl = workspaceDir.toURI().toURL();
        } else {
          File workspaceDir = new File(userHomeDir, ".dartEditor"); //$NON-NLS-1$
          workspaceUrl = workspaceDir.toURI().toURL();
        }

        workspaceLocation.set(workspaceUrl, true);
      } catch (IllegalStateException e) {
        // This generally happens in a runtime workbench, when the application has not been launched
        // with -data @noDefault. workspaceLocation.set() cannot be called twice.
        //Activator.logError(e);
      } catch (IOException e) {
        Activator.logError(e);
      }
    }
  }

}
