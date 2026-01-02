using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Avalonia.Threading;
using NexusBridgeGui.Services;

namespace NexusBridgeGui.ViewModels;

// Download queue item for non-premium mode
public record DownloadQueueItem(int ModId, int FileId, long FileSize, string ModName);

public partial class ConfirmViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    private readonly string _collectionUrl;
    private readonly string _mo2Path;
    private readonly string _profileName;

    [ObservableProperty]
    private bool _isLoading = true;

    [ObservableProperty]
    private bool _hasError;

    [ObservableProperty]
    private string _errorMessage = "";

    [ObservableProperty]
    private string _collectionName = "";

    [ObservableProperty]
    private string _game = "";

    [ObservableProperty]
    private int _totalMods;

    [ObservableProperty]
    private int _toDownload;

    [ObservableProperty]
    private int _alreadyHave;

    [ObservableProperty]
    private int _skipped;

    [ObservableProperty]
    private string _downloadSize = "";

    [ObservableProperty]
    private string _installSize = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowNonPremiumDownload))]
    [NotifyPropertyChangedFor(nameof(ShowNonPremiumContinue))]
    private bool _isPremium;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowNonPremiumDownload))]
    [NotifyPropertyChangedFor(nameof(ShowNonPremiumContinue))]
    private bool _needsDownloads;

    // Computed visibility for non-premium buttons
    public bool ShowNonPremiumDownload => !IsPremium && NeedsDownloads;
    public bool ShowNonPremiumContinue => !IsPremium && !NeedsDownloads;

    // Windows Long Path support
    [ObservableProperty]
    private bool _showLongPathWarning;

    [ObservableProperty]
    private string _longPathStatus = "";

    [ObservableProperty]
    private bool _isEnablingLongPath;

    // Download queue for non-premium mode
    public List<DownloadQueueItem> DownloadQueue { get; } = new();

    public ConfirmViewModel(string collectionUrl, string mo2Path, string profileName)
    {
        _collectionUrl = collectionUrl;
        _mo2Path = mo2Path;
        _profileName = profileName;
        Task.Run(QueryCollection);
    }

    private async Task QueryCollection()
    {
        var nexusBridge = FindNexusBridge();
        if (string.IsNullOrEmpty(nexusBridge))
        {
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsLoading = false;
                HasError = true;
                ErrorMessage = $"NexusBridge CLI executable not found!\n\n{GetSearchedPaths()}";
            });
            return;
        }

        // Strip trailing backslashes from path - "path\" causes \"  to be interpreted as escaped quote
        var mo2PathClean = _mo2Path.TrimEnd('\\', '/');
        var arguments = $"\"{_collectionUrl}\" \"{mo2PathClean}\" --query --profile \"{_profileName}\"";

        // Debug: Write command to file for troubleshooting
        try {
            var debugPath = Path.Combine(Path.GetDirectoryName(nexusBridge) ?? ".", "debug_command.txt");
            File.WriteAllText(debugPath, $"FileName: {nexusBridge}\nArguments: {arguments}\n");
        } catch { }

        var startInfo = new ProcessStartInfo
        {
            FileName = nexusBridge,
            Arguments = arguments,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(nexusBridge) // Set working dir to CLI location
        };

        try
        {
            var process = Process.Start(startInfo);
            if (process == null)
            {
                await Dispatcher.UIThread.InvokeAsync(() =>
                {
                    IsLoading = false;
                    HasError = true;
                    ErrorMessage = "Failed to start NexusBridge process";
                });
                return;
            }

            string output = await process.StandardOutput.ReadToEndAsync();
            string error = await process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();

            // Parse the query output
            bool foundResult = false;
            bool isPremiumUser = false;
            int toDownloadCount = 0;

            foreach (var line in output.Split('\n'))
            {
                var trimmedLine = line.Trim();

                if (trimmedLine.StartsWith("TOTAL_MODS:"))
                {
                    foundResult = true;
                    if (int.TryParse(trimmedLine.Replace("TOTAL_MODS:", "").Trim(), out int val))
                        await Dispatcher.UIThread.InvokeAsync(() => TotalMods = val);
                }
                else if (trimmedLine.StartsWith("TO_DOWNLOAD:"))
                {
                    if (int.TryParse(trimmedLine.Replace("TO_DOWNLOAD:", "").Trim(), out int val))
                    {
                        toDownloadCount = val;
                        await Dispatcher.UIThread.InvokeAsync(() => ToDownload = val);
                    }
                }
                else if (trimmedLine.StartsWith("ALREADY_HAVE:"))
                {
                    if (int.TryParse(trimmedLine.Replace("ALREADY_HAVE:", "").Trim(), out int val))
                        await Dispatcher.UIThread.InvokeAsync(() => AlreadyHave = val);
                }
                else if (trimmedLine.StartsWith("SKIPPED:"))
                {
                    if (int.TryParse(trimmedLine.Replace("SKIPPED:", "").Trim(), out int val))
                        await Dispatcher.UIThread.InvokeAsync(() => Skipped = val);
                }
                else if (trimmedLine.StartsWith("DOWNLOAD_BYTES:"))
                {
                    if (long.TryParse(trimmedLine.Replace("DOWNLOAD_BYTES:", "").Trim(), out long bytes))
                        await Dispatcher.UIThread.InvokeAsync(() => DownloadSize = FormatBytes(bytes));
                }
                else if (trimmedLine.StartsWith("INSTALL_BYTES:"))
                {
                    if (long.TryParse(trimmedLine.Replace("INSTALL_BYTES:", "").Trim(), out long bytes))
                        await Dispatcher.UIThread.InvokeAsync(() => InstallSize = FormatBytes(bytes));
                }
                else if (trimmedLine.StartsWith("COLLECTION_NAME:"))
                {
                    string name = trimmedLine.Replace("COLLECTION_NAME:", "").Trim();
                    await Dispatcher.UIThread.InvokeAsync(() => CollectionName = name);
                }
                else if (trimmedLine.StartsWith("GAME:"))
                {
                    string game = trimmedLine.Replace("GAME:", "").Trim();
                    await Dispatcher.UIThread.InvokeAsync(() => Game = game);
                }
                else if (trimmedLine.Contains("Premium: Yes"))
                {
                    isPremiumUser = true;
                }
                else if (trimmedLine.StartsWith("QUEUE_ITEM:"))
                {
                    // Parse: QUEUE_ITEM:modId:fileId:fileSize:modName
                    var parts = trimmedLine.Replace("QUEUE_ITEM:", "").Split(':', 4);
                    if (parts.Length >= 4 &&
                        int.TryParse(parts[0], out int modId) &&
                        int.TryParse(parts[1], out int fileId) &&
                        long.TryParse(parts[2], out long fileSize))
                    {
                        DownloadQueue.Add(new DownloadQueueItem(modId, fileId, fileSize, parts[3]));
                    }
                }
            }

            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsPremium = isPremiumUser;
                NeedsDownloads = toDownloadCount > 0;
            });

            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsLoading = false;

                // Check Windows Long Path support
                if (WindowsLongPathService.IsWindows && !WindowsLongPathService.IsLongPathEnabled())
                {
                    ShowLongPathWarning = true;
                    LongPathStatus = "Windows long path support is disabled. Some mods with deeply nested files may fail to install.";
                }

                if (!foundResult)
                {
                    HasError = true;
                    ErrorMessage = "Failed to query collection.";

                    // Show exit code
                    if (process.ExitCode != 0)
                        ErrorMessage += $"\n\nExit code: {process.ExitCode}";

                    // Show stderr if any
                    if (!string.IsNullOrEmpty(error))
                        ErrorMessage += $"\n\nError output:\n{error}";

                    // Show stdout if no stderr (might have useful info)
                    if (string.IsNullOrEmpty(error) && !string.IsNullOrEmpty(output))
                        ErrorMessage += $"\n\nOutput:\n{output}";

                    // Show path for debugging
                    ErrorMessage += $"\n\nCLI path: {nexusBridge}";
                }
            });
        }
        catch (Exception ex)
        {
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsLoading = false;
                HasError = true;
                ErrorMessage = $"Error starting CLI: {ex.Message}\n\nCLI path: {nexusBridge}";
            });
        }
    }

    private static string FormatBytes(long bytes)
    {
        if (bytes >= 1024L * 1024 * 1024)
            return $"{bytes / (1024.0 * 1024 * 1024):F1} GB";
        else if (bytes >= 1024L * 1024)
            return $"{bytes / (1024.0 * 1024):F1} MB";
        else
            return $"{bytes / 1024.0:F1} KB";
    }

    private static string FindNexusBridge()
    {
        var exeDir = AppContext.BaseDirectory;
        var exeName = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "NexusBridge.exe" : "NexusBridge";

        var sameDirPath = Path.Combine(exeDir, exeName);
        if (File.Exists(sameDirPath))
            return sameDirPath;

        var cwdPath = Path.Combine(Directory.GetCurrentDirectory(), exeName);
        if (File.Exists(cwdPath))
            return cwdPath;

        return "";
    }

    // For debugging - show where we looked
    private static string GetSearchedPaths()
    {
        var exeDir = AppContext.BaseDirectory;
        var exeName = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "NexusBridge.exe" : "NexusBridge";
        return $"Searched:\n- {Path.Combine(exeDir, exeName)}\n- {Path.Combine(Directory.GetCurrentDirectory(), exeName)}";
    }

    [RelayCommand]
    private void Confirm()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Progress, _collectionUrl));
    }

    [RelayCommand]
    private async Task EnableLongPaths()
    {
        IsEnablingLongPath = true;
        LongPathStatus = "Requesting administrator access...";

        await Task.Run(() =>
        {
            var (success, message) = WindowsLongPathService.EnableLongPaths();

            Dispatcher.UIThread.Post(() =>
            {
                IsEnablingLongPath = false;
                LongPathStatus = message;

                if (success)
                {
                    ShowLongPathWarning = false;
                }
            });
        });
    }

    [RelayCommand]
    private void StartNonPremiumDownload()
    {
        // Navigate to non-premium download view with queue
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.NonPremiumDownload, _collectionUrl));
    }

    [RelayCommand]
    private void GoBack()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Mo2Setup, _collectionUrl));
    }

    // Expose queue for passing to NonPremiumDownloadViewModel
    public string CollectionUrlForNav => _collectionUrl;
    public string Mo2PathForNav => _mo2Path;
    public string ProfileNameForNav => _profileName;
}
