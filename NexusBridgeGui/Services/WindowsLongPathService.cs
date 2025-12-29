using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace NexusBridgeGui.Services;

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
            // Direct access failed, try with elevation
        }

        // Launch PowerShell with elevation to set the registry key
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "powershell.exe",
                Arguments = $"-Command \"Start-Process powershell -Verb RunAs -ArgumentList '-Command Set-ItemProperty -Path HKLM:\\\\{RegistryPath.Replace("\\", "\\\\")} -Name {RegistryValue} -Value 1 -Type DWord'\"",
                UseShellExecute = true,
                CreateNoWindow = false
            };

            var process = Process.Start(psi);
            process?.WaitForExit(10000); // Wait up to 10 seconds

            // Check if it worked
            if (IsLongPathEnabled())
            {
                return (true, "Long path support enabled! A system restart may be required for full effect.");
            }
            else
            {
                return (false, "Failed to enable long paths. Please run as administrator or enable manually:\n\n" +
                    "1. Open Registry Editor (regedit)\n" +
                    "2. Navigate to: HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\n" +
                    "3. Set LongPathsEnabled to 1");
            }
        }
        catch (Exception ex)
        {
            return (false, $"Error: {ex.Message}\n\nPlease enable manually via Registry Editor:\n" +
                "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\\LongPathsEnabled = 1");
        }
    }
}
