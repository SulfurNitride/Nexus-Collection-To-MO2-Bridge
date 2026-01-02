using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace NexusBridgeGui.Services;

// Suppress CA1416 - the code has runtime guards for platform-specific calls
#pragma warning disable CA1416

public static class WindowsLongPathService
{
    private const string RegistryPath = @"SYSTEM\CurrentControlSet\Control\FileSystem";
    private const string RegistryValue = "LongPathsEnabled";

    public static bool IsWindows => RuntimeInformation.IsOSPlatform(OSPlatform.Windows);

    public static bool IsLongPathEnabled()
    {
        if (!IsWindows)
            return true; // Linux/Mac don't have this limitation

        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(RegistryPath);
            if (key != null)
            {
                var value = key.GetValue(RegistryValue);
                if (value is int intValue)
                    return intValue == 1;
            }
        }
        catch
        {
            // Can't read registry, assume not enabled
        }

        return false;
    }

    public static (bool success, string message) EnableLongPaths()
    {
        if (!IsWindows)
            return (true, "Not needed on this platform");

        try
        {
            // Try to set it directly first (requires admin)
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(RegistryPath, true);
            if (key != null)
            {
                key.SetValue(RegistryValue, 1, Microsoft.Win32.RegistryValueKind.DWord);
                return (true, "Long path support enabled! A system restart may be required for full effect.");
            }
        }
        catch
        {
            // Direct access failed, try with .reg file approach
        }

        // Create and run a .reg file which will trigger UAC prompt
        try
        {
            var regFilePath = Path.Combine(Path.GetTempPath(), "enable_longpath.reg");

            // Create the .reg file
            var regContent = "Windows Registry Editor Version 5.00\r\n\r\n" +
                "[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\FileSystem]\r\n" +
                "\"LongPathsEnabled\"=dword:00000001\r\n";
            File.WriteAllText(regFilePath, regContent);

            // Run regedit with /s (silent) - will still trigger UAC
            var psi = new ProcessStartInfo
            {
                FileName = "regedit.exe",
                Arguments = $"/s \"{regFilePath}\"",
                UseShellExecute = true,
                Verb = "runas" // Request admin elevation
            };

            var process = Process.Start(psi);
            process?.WaitForExit(30000); // Wait up to 30 seconds

            // Clean up
            try { File.Delete(regFilePath); } catch { }

            // Check if it worked
            if (IsLongPathEnabled())
            {
                return (true, "Long path support enabled! A system restart may be required for full effect.");
            }
            else
            {
                return (false, "Failed to enable long paths. The UAC prompt may have been cancelled.\n\n" +
                    "Please try again and click 'Yes' on the admin prompt.");
            }
        }
        catch (Exception ex)
        {
            return (false, $"Error: {ex.Message}\n\nPlease enable manually via Registry Editor:\n" +
                "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\\LongPathsEnabled = 1");
        }
    }
}

#pragma warning restore CA1416
