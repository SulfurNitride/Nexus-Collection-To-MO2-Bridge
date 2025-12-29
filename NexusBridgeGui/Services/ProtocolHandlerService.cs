using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace NexusBridgeGui.Services;

public static class ProtocolHandlerService
{
    private const string ProtocolScheme = "nxm";

    public static bool IsRegistered()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            return IsRegisteredLinux();
        else if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return IsRegisteredWindows();
        return false;
    }

    public static (bool success, string message) Register()
    {
        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                return RegisterLinux();
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return RegisterWindows();
            return (false, "Unsupported platform");
        }
        catch (Exception ex)
        {
            return (false, $"Error: {ex.Message}");
        }
    }

    public static (bool success, string message) Unregister()
    {
        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                return UnregisterLinux();
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return UnregisterWindows();
            return (false, "Unsupported platform");
        }
        catch (Exception ex)
        {
            return (false, $"Error: {ex.Message}");
        }
    }

    #region Linux Implementation

    private static string GetDesktopFilePath()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return Path.Combine(home, ".local", "share", "applications", "nexusbridge-nxm.desktop");
    }

    private static bool IsRegisteredLinux()
    {
        // Check if we're actually the default handler, not just if our file exists
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "xdg-mime",
                Arguments = $"query default x-scheme-handler/{ProtocolScheme}",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                CreateNoWindow = true
            };
            var process = Process.Start(psi);
            if (process != null)
            {
                string output = process.StandardOutput.ReadToEnd().Trim();
                process.WaitForExit();
                return output == "nexusbridge-nxm.desktop";
            }
        }
        catch { }

        return false;
    }

    private static string GetMimeappsListPath()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return Path.Combine(home, ".config", "mimeapps.list");
    }

    private static (bool, string) RegisterLinux()
    {
        var exePath = Process.GetCurrentProcess().MainModule?.FileName
            ?? Path.Combine(AppContext.BaseDirectory, "NexusBridgeGui");

        var desktopContent = $@"[Desktop Entry]
Name=NexusBridge
Comment=Nexus Mods Collection Installer
Exec=""{exePath}"" --nxm-url %u
Terminal=false
Type=Application
MimeType=x-scheme-handler/{ProtocolScheme};
NoDisplay=true
Categories=Utility;
";

        var desktopFilePath = GetDesktopFilePath();
        var directory = Path.GetDirectoryName(desktopFilePath);

        if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
            Directory.CreateDirectory(directory);

        File.WriteAllText(desktopFilePath, desktopContent);

        // Register with xdg-mime
        var process = Process.Start(new ProcessStartInfo
        {
            FileName = "xdg-mime",
            Arguments = $"default nexusbridge-nxm.desktop x-scheme-handler/{ProtocolScheme}",
            UseShellExecute = false,
            CreateNoWindow = true
        });
        process?.WaitForExit();

        // Also directly update mimeapps.list for better compatibility
        try
        {
            var mimeappsPath = GetMimeappsListPath();
            var mimeType = $"x-scheme-handler/{ProtocolScheme}";
            var lines = new List<string>();
            bool inDefaultApps = false;
            bool foundMimeType = false;

            if (File.Exists(mimeappsPath))
            {
                foreach (var line in File.ReadAllLines(mimeappsPath))
                {
                    if (line.Trim() == "[Default Applications]")
                    {
                        inDefaultApps = true;
                        lines.Add(line);
                    }
                    else if (line.StartsWith("[") && line.EndsWith("]"))
                    {
                        // New section - if we were in Default Apps and didn't find our entry, add it
                        if (inDefaultApps && !foundMimeType)
                        {
                            lines.Add($"{mimeType}=nexusbridge-nxm.desktop");
                            foundMimeType = true;
                        }
                        inDefaultApps = false;
                        lines.Add(line);
                    }
                    else if (inDefaultApps && line.StartsWith($"{mimeType}="))
                    {
                        // Replace existing entry
                        lines.Add($"{mimeType}=nexusbridge-nxm.desktop");
                        foundMimeType = true;
                    }
                    else
                    {
                        lines.Add(line);
                    }
                }
            }

            // If no Default Applications section or no entry found, add it
            if (!foundMimeType)
            {
                if (!lines.Any(l => l.Trim() == "[Default Applications]"))
                {
                    lines.Add("[Default Applications]");
                }
                lines.Add($"{mimeType}=nexusbridge-nxm.desktop");
            }

            File.WriteAllLines(mimeappsPath, lines);
        }
        catch
        {
            // Fall back to just xdg-mime
        }

        // Update desktop database
        var updateDb = Process.Start(new ProcessStartInfo
        {
            FileName = "update-desktop-database",
            Arguments = Path.GetDirectoryName(desktopFilePath),
            UseShellExecute = false,
            CreateNoWindow = true
        });
        updateDb?.WaitForExit();

        // Verify registration worked
        if (IsRegisteredLinux())
        {
            return (true, "Protocol handler registered successfully.\nnxm:// URLs will now open with NexusBridge.");
        }
        else
        {
            return (false, "Registration may have failed. Try closing your browser and clicking Re-register.\n\nIf it still doesn't work, another application may be overriding the nxm:// handler.");
        }
    }

    private static (bool, string) UnregisterLinux()
    {
        var desktopFilePath = GetDesktopFilePath();

        if (File.Exists(desktopFilePath))
        {
            File.Delete(desktopFilePath);

            // Update desktop database
            var updateDb = Process.Start(new ProcessStartInfo
            {
                FileName = "update-desktop-database",
                Arguments = Path.GetDirectoryName(desktopFilePath),
                UseShellExecute = false,
                CreateNoWindow = true
            });
            updateDb?.WaitForExit();
        }

        return (true, "Protocol handler unregistered.");
    }

    #endregion

    #region Windows Implementation

#pragma warning disable CA1416 // Platform compatibility - these methods are only called on Windows

    private static bool IsRegisteredWindows()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey($@"Software\Classes\{ProtocolScheme}");
            return key != null;
        }
        catch
        {
            return false;
        }
    }

    private static (bool, string) RegisterWindows()
    {
        var exePath = Process.GetCurrentProcess().MainModule?.FileName
            ?? Path.Combine(AppContext.BaseDirectory, "NexusBridgeGui.exe");

        try
        {
            // Create the protocol key
            using var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{ProtocolScheme}");
            key.SetValue("", "URL:NXM Protocol");
            key.SetValue("URL Protocol", "");

            // Set the icon
            using var iconKey = key.CreateSubKey("DefaultIcon");
            iconKey.SetValue("", $"\"{exePath}\",0");

            // Set the command to execute
            using var commandKey = key.CreateSubKey(@"shell\open\command");
            commandKey.SetValue("", $"\"{exePath}\" --nxm-url \"%1\"");

            return (true, "Protocol handler registered successfully.\nnxm:// URLs will now open with NexusBridge.");
        }
        catch (Exception ex)
        {
            return (false, $"Failed to register: {ex.Message}\nTry running as administrator.");
        }
    }

    private static (bool, string) UnregisterWindows()
    {
        try
        {
            Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree($@"Software\Classes\{ProtocolScheme}", false);
            return (true, "Protocol handler unregistered.");
        }
        catch (Exception ex)
        {
            return (false, $"Failed to unregister: {ex.Message}");
        }
    }

#pragma warning restore CA1416

    #endregion
}
