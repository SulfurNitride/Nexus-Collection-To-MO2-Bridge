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

    /// <summary>
    /// Returns true if ANY application is registered to handle nxm:// URLs (not just us).
    /// Use this to avoid prompting users who already have a dedicated handler.
    /// </summary>
    public static bool HasAnyHandler()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            return HasAnyHandlerLinux();
        else if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return HasAnyHandlerWindows();
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

    private static bool HasAnyHandlerLinux()
    {
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
                return !string.IsNullOrEmpty(output);
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

    private static string GetLocalMimeappsListPath()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return Path.Combine(home, ".local", "share", "applications", "mimeapps.list");
    }

    private static void UpdateMimeappsList(string mimeappsPath, string mimeType)
    {
        // Read existing content, removing ALL existing nxm entries from ALL sections
        var sections = new Dictionary<string, List<string>>();
        string currentSection = "";

        if (File.Exists(mimeappsPath))
        {
            foreach (var line in File.ReadAllLines(mimeappsPath))
            {
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    currentSection = line;
                    if (!sections.ContainsKey(currentSection))
                        sections[currentSection] = new List<string>();
                }
                else if (!string.IsNullOrWhiteSpace(line) && !line.StartsWith($"{mimeType}="))
                {
                    // Keep all lines EXCEPT existing nxm handler entries
                    if (!sections.ContainsKey(currentSection))
                        sections[currentSection] = new List<string>();
                    sections[currentSection].Add(line);
                }
                // Lines starting with mimeType= are REMOVED (not added to output)
            }
        }

        // Ensure [Default Applications] section exists and add our entry FIRST
        if (!sections.ContainsKey("[Default Applications]"))
            sections["[Default Applications]"] = new List<string>();
        sections["[Default Applications]"].Insert(0, $"{mimeType}=nexusbridge-nxm.desktop");

        // Also add to [Added Associations] for systems that check there first
        if (!sections.ContainsKey("[Added Associations]"))
            sections["[Added Associations]"] = new List<string>();
        sections["[Added Associations]"].Insert(0, $"{mimeType}=nexusbridge-nxm.desktop");

        // Write back - [Default Applications] FIRST for priority
        var output = new List<string>();

        // Write [Default Applications] first
        if (sections.ContainsKey("[Default Applications]"))
        {
            output.Add("[Default Applications]");
            output.AddRange(sections["[Default Applications]"]);
            sections.Remove("[Default Applications]");
        }

        // Write [Added Associations] second
        if (sections.ContainsKey("[Added Associations]"))
        {
            output.Add("[Added Associations]");
            output.AddRange(sections["[Added Associations]"]);
            sections.Remove("[Added Associations]");
        }

        // Write other sections
        foreach (var section in sections)
        {
            if (!string.IsNullOrEmpty(section.Key))
            {
                output.Add(section.Key);
                output.AddRange(section.Value);
            }
        }

        // Ensure directory exists
        var dir = Path.GetDirectoryName(mimeappsPath);
        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
            Directory.CreateDirectory(dir);

        File.WriteAllLines(mimeappsPath, output);
    }

    private static (bool, string) RegisterLinux()
    {
        var log = new System.Text.StringBuilder();

        // Use Environment.ProcessPath for single-file apps (MainModule.FileName returns temp extraction path)
        var exePath = Environment.ProcessPath
            ?? Path.Combine(AppContext.BaseDirectory, "NexusBridgeGui");

        log.AppendLine($"Executable: {exePath}");
        log.AppendLine($"Exists: {File.Exists(exePath)}");

        // Verify the executable exists
        if (!File.Exists(exePath))
        {
            return (false, $"Executable not found at: {exePath}\n\nTry running from the extracted folder.");
        }

        var desktopFilePath = GetDesktopFilePath();
        log.AppendLine($"Desktop file: {desktopFilePath}");

        // Clean up any existing registration first
        try
        {
            if (File.Exists(desktopFilePath))
            {
                File.Delete(desktopFilePath);
                log.AppendLine("Deleted old .desktop file");
            }
        }
        catch (Exception ex) { log.AppendLine($"Delete old failed: {ex.Message}"); }

        var desktopContent = $@"[Desktop Entry]
Name=NexusBridge
Comment=Nexus Mods Collection Installer
Exec=""{exePath}"" --nxm-url %u
Terminal=false
Type=Application
MimeType=x-scheme-handler/{ProtocolScheme};
Categories=Utility;
";

        var directory = Path.GetDirectoryName(desktopFilePath);

        try
        {
            if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
                Directory.CreateDirectory(directory);

            File.WriteAllText(desktopFilePath, desktopContent);
            log.AppendLine($"Wrote .desktop file: {File.Exists(desktopFilePath)}");
        }
        catch (Exception ex)
        {
            return (false, $"Failed to write .desktop file: {ex.Message}\n\nPath: {desktopFilePath}\n\n{log}");
        }

        // Register with xdg-mime
        try
        {
            var process = Process.Start(new ProcessStartInfo
            {
                FileName = "xdg-mime",
                Arguments = $"default nexusbridge-nxm.desktop x-scheme-handler/{ProtocolScheme}",
                UseShellExecute = false,
                CreateNoWindow = true
            });
            process?.WaitForExit();
            log.AppendLine($"xdg-mime: exit code {process?.ExitCode}");
        }
        catch (Exception ex)
        {
            log.AppendLine($"xdg-mime failed: {ex.Message}");
        }

        // Try gio mime (more reliable on GNOME-based systems)
        try
        {
            var gioProcess = Process.Start(new ProcessStartInfo
            {
                FileName = "gio",
                Arguments = $"mime x-scheme-handler/{ProtocolScheme} nexusbridge-nxm.desktop",
                UseShellExecute = false,
                CreateNoWindow = true
            });
            gioProcess?.WaitForExit();
            log.AppendLine($"gio mime: exit code {gioProcess?.ExitCode}");
        }
        catch (Exception ex) { log.AppendLine($"gio failed: {ex.Message}"); }

        // Update BOTH mimeapps.list locations for maximum compatibility
        var mimeType = $"x-scheme-handler/{ProtocolScheme}";

        // ~/.config/mimeapps.list (primary location for most desktop environments)
        try
        {
            UpdateMimeappsList(GetMimeappsListPath(), mimeType);
            log.AppendLine($"Updated: {GetMimeappsListPath()}");
        }
        catch (Exception ex) { log.AppendLine($"mimeapps.list failed: {ex.Message}"); }

        // ~/.local/share/applications/mimeapps.list (older location, still used by some apps)
        try
        {
            UpdateMimeappsList(GetLocalMimeappsListPath(), mimeType);
            log.AppendLine($"Updated: {GetLocalMimeappsListPath()}");
        }
        catch (Exception ex) { log.AppendLine($"local mimeapps.list failed: {ex.Message}"); }

        // Clear mimeinfo.cache to force refresh
        try
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var cacheFile = Path.Combine(home, ".local", "share", "applications", "mimeinfo.cache");
            if (File.Exists(cacheFile))
            {
                File.Delete(cacheFile);
                log.AppendLine("Deleted mimeinfo.cache");
            }
        }
        catch (Exception ex) { log.AppendLine($"cache delete failed: {ex.Message}"); }

        // Update desktop database
        try
        {
            var updateDb = Process.Start(new ProcessStartInfo
            {
                FileName = "update-desktop-database",
                Arguments = Path.GetDirectoryName(desktopFilePath),
                UseShellExecute = false,
                CreateNoWindow = true
            });
            updateDb?.WaitForExit();
            log.AppendLine($"update-desktop-database: exit code {updateDb?.ExitCode}");
        }
        catch (Exception ex) { log.AppendLine($"update-desktop-database failed: {ex.Message}"); }

        // Get current handler
        string currentHandler = "unknown";
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
            var p = Process.Start(psi);
            if (p != null)
            {
                currentHandler = p.StandardOutput.ReadToEnd().Trim();
                p.WaitForExit();
            }
        }
        catch { }
        log.AppendLine($"Current handler: {currentHandler}");

        // Verify registration worked
        if (IsRegisteredLinux())
        {
            return (true, $"Protocol handler registered successfully.\nnxm:// URLs will now open with NexusBridge.\n\n--- Debug Log ---\n{log}");
        }
        else
        {
            return (false, $"Registration failed.\n\nCurrent handler: {currentHandler}\nExpected: nexusbridge-nxm.desktop\n\nDesktop file: {desktopFilePath}\nExecutable: {exePath}\n\nTry closing your browser and running again.\n\n--- Debug Log ---\n{log}");
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
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey($@"Software\Classes\{ProtocolScheme}\shell\open\command");
            if (key == null) return false;

            var command = key.GetValue("")?.ToString() ?? "";
            var exePath = Environment.ProcessPath ?? "NexusBridgeGui.exe";
            return command.Contains(exePath, StringComparison.OrdinalIgnoreCase) ||
                   command.Contains("NexusBridgeGui", StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private static bool HasAnyHandlerWindows()
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
        // Use Environment.ProcessPath for single-file apps (MainModule.FileName returns temp extraction path)
        var exePath = Environment.ProcessPath
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
