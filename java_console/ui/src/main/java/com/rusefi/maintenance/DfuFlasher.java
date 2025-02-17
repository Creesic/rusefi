package com.rusefi.maintenance;

import com.opensr5.ini.IniFileModel;
import com.rusefi.ConsoleUI;
import com.rusefi.Launcher;
import com.rusefi.Timeouts;
import com.rusefi.autodetect.PortDetector;
import com.rusefi.autodetect.SerialAutoChecker;
import com.rusefi.io.DfuHelper;
import com.rusefi.io.IoStream;
import com.rusefi.io.serial.BufferedSerialIoStream;
import com.rusefi.ui.StatusWindow;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.*;
import java.awt.*;
import java.io.*;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Function;

import static com.rusefi.StartupFrame.appendBundleName;

/**
 * @see FirmwareFlasher
 */
public class DfuFlasher {
    private static final String DFU_BINARY_LOCATION = Launcher.TOOLS_PATH + File.separator + "STM32_Programmer_CLI/bin";
    private static final String DFU_BINARY = "STM32_Programmer_CLI.exe";

    public static void doAutoDfu(Object selectedItem, JComponent parent) {
        if (selectedItem == null) {
            JOptionPane.showMessageDialog(parent, "Failed to locate serial ports");
            return;
        }
        String port = selectedItem.toString();

        StatusWindow wnd = createStatusWindow();

        AtomicBoolean isSignatureValidated = rebootToDfu(parent, port, wnd);
        if (isSignatureValidated == null) return;
        if (isSignatureValidated.get()) {
            if (!ProgramSelector.IS_WIN) {
                wnd.append("Switched to DFU mode!");
                wnd.append("rusEFI console can only program on Windows");
                return;
            }
            submitAction(() -> {
                timeForDfuSwitch(wnd);
                executeDFU(wnd);
            });
        } else {
            wnd.append("Please use manual DFU to change bundle type.");
        }
    }

    private static void submitAction(Runnable r) {
        ExecHelper.submitAction(r, DfuFlasher.class + " thread");
    }

    @Nullable
    public static AtomicBoolean rebootToDfu(JComponent parent, String port, StatusWindow wnd) {
        AtomicBoolean isSignatureValidated = new AtomicBoolean(true);
        if (!PortDetector.isAutoPort(port)) {
            wnd.append("Using selected " + port + "\n");
            IoStream stream = BufferedSerialIoStream.openPort(port);
            AtomicReference<String> signature = new AtomicReference<>();
            new SerialAutoChecker(PortDetector.DetectorMode.DETECT_TS, port, new CountDownLatch(1)).checkResponse(stream, new Function<SerialAutoChecker.CallbackContext, Void>() {
                @Override
                public Void apply(SerialAutoChecker.CallbackContext callbackContext) {
                    signature.set(callbackContext.getSignature());
                    return null;
                }
            });
            if (signature.get() == null) {
                JOptionPane.showMessageDialog(ConsoleUI.getFrame(), "rusEFI has not responded on selected " + port + "\n" +
                        "Maybe try automatic serial port detection?");
                return null;
            }
            boolean isSignatureValidatedLocal = DfuHelper.sendDfuRebootCommand(parent, signature.get(), stream, wnd);
            isSignatureValidated.set(isSignatureValidatedLocal);
        } else {
            wnd.append("Auto-detecting port...\n");
            // instead of opening the just-detected port we execute the command using the same stream we used to discover port
            // it's more reliable this way
            port = PortDetector.autoDetectSerial(callbackContext -> {
                boolean isSignatureValidatedLocal = DfuHelper.sendDfuRebootCommand(parent, callbackContext.getSignature(), callbackContext.getStream(), wnd);
                isSignatureValidated.set(isSignatureValidatedLocal);
                return null;
            }).getSerialPort();
            if (port == null) {
                JOptionPane.showMessageDialog(ConsoleUI.getFrame(), "rusEFI serial port not detected");
                return null;
            } else {
                wnd.append("Detected rusEFI on " + port + "\n");
            }
        }
        return isSignatureValidated;
    }

    @NotNull
    protected static StatusWindow createStatusWindow() {
        StatusWindow wnd = new StatusWindow();
        wnd.showFrame(appendBundleName("DFU status " + Launcher.CONSOLE_VERSION));
        return wnd;
    }

    public static void runDfuErase() {
        StatusWindow wnd = createStatusWindow();
        submitAction(() -> {
            ExecHelper.executeCommand(DFU_BINARY_LOCATION,
                    getDfuEraseCommand(),
                    DFU_BINARY, wnd, new StringBuffer());
            // it's a lengthy operation let's signal end
            Toolkit.getDefaultToolkit().beep();
        });
    }

    public static void runDfuProgramming() {
        StatusWindow wnd = createStatusWindow();
        submitAction(() -> executeDFU(wnd));
    }

    private static void executeDFU(StatusWindow wnd) {
        StringBuffer stdout = new StringBuffer();
        String errorResponse;
        try {
            errorResponse = ExecHelper.executeCommand(DFU_BINARY_LOCATION,
                    getDfuWriteCommand(),
                    DFU_BINARY, wnd, stdout);
        } catch (FileNotFoundException e) {
            wnd.append("ERROR: " + e);
            wnd.setErrorState(true);
            return;
        }
        if (stdout.toString().contains("Download verified successfully")) {
            // looks like sometimes we are not catching the last line of the response? 'Upgrade' happens before 'Verify'
            wnd.append("SUCCESS!");
            wnd.append("Please power cycle device to exit DFU mode");
        } else if (stdout.toString().contains("Target device not found")) {
            wnd.append("ERROR: Device not connected or STM32 Bootloader driver not installed?");
            wnd.append("ERROR: Please try installing drivers using 'Install Drivers' button on rusEFI splash screen");
            wnd.append("ERROR: Alternatively please install drivers using Device Manager pointing at 'drivers/silent_st_drivers/DFU_Driver' folder");
            appendDeviceReport(wnd);
            wnd.setErrorState(true);
        } else {
            wnd.append(stdout.length() + " / " + errorResponse.length());
            wnd.append("ERROR: does not look like DFU has worked!");
            appendDeviceReport(wnd);
            wnd.setErrorState(true);
        }
    }

    private static void appendDeviceReport(StatusWindow wnd) {
        for (String line : getDevicesReport()) {
            if (line.contains("STM Device in DFU Mode")) {
                wnd.append(" ******************************************************************");
                wnd.append(" ************* YOU NEED TO REMOVE LEGACY DFU DRIVER ***************");
                wnd.append(" ******************************************************************");
            }
            wnd.append("Devices: " + line);
        }
    }

    private static void timeForDfuSwitch(StatusWindow wnd) {
        wnd.append("Giving time for USB enumeration...");
        try {
            // two seconds not enough on my Windows 10
            Thread.sleep(3 * Timeouts.SECOND);
        } catch (InterruptedException e) {
            throw new IllegalStateException(e);
        }
    }

    private static String getDfuWriteCommand() throws FileNotFoundException {
        String prefix = "rusefi";
        String suffix = ".bin";
        String fileName = IniFileModel.findFile(Launcher.INPUT_FILES_PATH, prefix, suffix);
        if (fileName == null)
            throw new FileNotFoundException("File not found " + prefix + "*" + suffix);
        // we need quotes in case if absolute path contains spaces
        String hexAbsolutePath = quote(new File(fileName).getAbsolutePath());

        return DFU_BINARY_LOCATION + "/" + DFU_BINARY + " -c port=usb1 -w " + hexAbsolutePath + " 0x08000000 -v -s";
    }

    private static String quote(String absolutePath) {
        return "\"" + absolutePath + "\"";
    }

    private static String getDfuEraseCommand() {
        return DFU_BINARY_LOCATION + "/" + DFU_BINARY + " -c port=usb1 -e all";
    }

    @NotNull
    static List<String> getDevicesReport() {
        // todo: assert windows 10, explicit message if not
        List<String> report = new ArrayList<>();

        try {
            Process powerShellProcess = Runtime.getRuntime().exec("powershell \"Get-PnpDevice -PresentOnly\"");
            // Getting the results
            powerShellProcess.getOutputStream().close();

            String line;
            BufferedReader stdout = new BufferedReader(new InputStreamReader(powerShellProcess.getInputStream()));
            while ((line = stdout.readLine()) != null) {
                String lowerCase = line.toLowerCase();
                if (!lowerCase.contains("stm32") && !lowerCase.contains("dfu") && !lowerCase.contains("rusefi"))
                    continue;
                report.add(line);
            }
            stdout.close();
            return report;
        } catch (IOException e) {
            return Collections.emptyList();
        }
    }
}
