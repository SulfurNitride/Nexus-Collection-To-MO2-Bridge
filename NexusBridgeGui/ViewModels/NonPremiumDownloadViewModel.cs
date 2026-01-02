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

public partial class NonPremiumDownloadViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    private readonly string _collectionUrl;
    private readonly string _mo2Path;
    private readonly string _profileName;
    private readonly string _game;
    private readonly List<DownloadQueueItem> _downloadQueue;
    private int _currentIndex;

    [ObservableProperty]
    private string _currentModName = "";

    [ObservableProperty]
    private int _currentModId;

    [ObservableProperty]
    private int _currentFileId;

    [ObservableProperty]
    private string _currentFileSize = "";

    [ObservableProperty]
    private int _queuePosition;

    [ObservableProperty]
    private int _queueTotal;

    [ObservableProperty]
    private string _statusMessage = "";

    [ObservableProperty]
    private bool _isDownloading;

    [ObservableProperty]
    private bool _downloadComplete;

    [ObservableProperty]
    private string _downloadProgress = "";

    [ObservableProperty]
    private bool _hasError;

    [ObservableProperty]
    private string _errorMessage = "";

    [ObservableProperty]
    private bool _isHandlerRegistered;

    [ObservableProperty]
    private bool _isWaitingForUrl;

    private System.Timers.Timer? _urlPollTimer;
    private string _receivedNxmUrl = "";

    public NonPremiumDownloadViewModel(
        string collectionUrl,
        string mo2Path,
        string profileName,
        string game,
        List<DownloadQueueItem> downloadQueue)
    {
        _collectionUrl = collectionUrl;
        _mo2Path = mo2Path;
        _profileName = profileName;
        _game = game;
        _downloadQueue = downloadQueue;
        _currentIndex = 0;

        QueueTotal = downloadQueue.Count;

        // Check handler registration status
        IsHandlerRegistered = ProtocolHandlerService.IsRegistered();

        UpdateCurrentMod();

        // Start polling for URLs
        StartUrlPolling();

        // Auto-open browser for first mod
        OpenInBrowser();
    }

    private void StartUrlPolling()
    {
        _urlPollTimer = new System.Timers.Timer(500); // Check every 500ms
        _urlPollTimer.Elapsed += (s, e) =>
        {
            var url = NxmUrlReceiverService.ConsumePendingUrl();
            if (!string.IsNullOrEmpty(url))
            {
                Dispatcher.UIThread.Post(() => OnUrlReceived(url));
            }
        };
        _urlPollTimer.Start();
    }

    private void OnUrlReceived(string url)
    {
        if (IsDownloading || DownloadComplete)
            return;

        // Validate the URL matches expected mod/file
        var match = Regex.Match(url, @"nxm://([^/]+)/mods/(\d+)/files/(\d+)");
        if (!match.Success)
        {
            HasError = true;
            ErrorMessage = "Invalid nxm:// URL received";
            IsWaitingForUrl = false;
            return;
        }

        int urlModId = int.Parse(match.Groups[2].Value);
        int urlFileId = int.Parse(match.Groups[3].Value);

        if (urlModId != CurrentModId || urlFileId != CurrentFileId)
        {
            HasError = true;
            ErrorMessage = $"Wrong mod received!\nExpected: Mod {CurrentModId}, File {CurrentFileId}\nGot: Mod {urlModId}, File {urlFileId}\n\nClick 'Retry' to try again.";
            IsWaitingForUrl = false;
            return;
        }

        // URL is valid - start download automatically
        _receivedNxmUrl = url;
        IsWaitingForUrl = false;
        StatusMessage = "Download link received!";
        _ = StartDownload();
    }

    public void Cleanup()
    {
        _urlPollTimer?.Stop();
        _urlPollTimer?.Dispose();
    }

    private void UpdateCurrentMod()
    {
        if (_currentIndex < _downloadQueue.Count)
        {
            var item = _downloadQueue[_currentIndex];
            CurrentModName = item.ModName;
            CurrentModId = item.ModId;
            CurrentFileId = item.FileId;
            CurrentFileSize = FormatBytes(item.FileSize);
            QueuePosition = _currentIndex + 1;
            StatusMessage = "";
            _receivedNxmUrl = "";
            HasError = false;
            DownloadComplete = false;
            IsWaitingForUrl = true;
        }
        else
        {
            // All done - go to install
            Cleanup();
            NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Progress, _collectionUrl));
        }
    }

    private static string FormatBytes(long bytes)
    {
        if (bytes >= 1024L * 1024 * 1024)
            return $"{bytes / (1024.0 * 1024 * 1024):F1} GB";
        else if (bytes >= 1024L * 1024)
            return $"{bytes / (1024.0 * 1024):F1} MB";
        else if (bytes > 0)
            return $"{bytes / 1024.0:F1} KB";
        else
            return "Unknown";
    }

    [RelayCommand]
    private void RegisterHandler()
    {
        var (success, message) = ProtocolHandlerService.Register();
        if (success)
        {
            IsHandlerRegistered = true;
            StatusMessage = "Handler registered successfully!";
        }
        else
        {
            HasError = true;
            ErrorMessage = message;
        }
    }

    [RelayCommand]
    private void OpenInBrowser()
    {
        // Add &nmm=1 to auto-trigger the download dialog on Nexus
        string url = $"https://www.nexusmods.com/{_game}/mods/{CurrentModId}?tab=files&file_id={CurrentFileId}&nmm=1";

        HasError = false;
        IsWaitingForUrl = true;

        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            }
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                Process.Start("xdg-open", url);
            }
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
            {
                Process.Start("open", url);
            }
        }
        catch (Exception ex)
        {
            HasError = true;
            ErrorMessage = $"Failed to open browser: {ex.Message}";
            IsWaitingForUrl = false;
        }
    }

    private async Task StartDownload()
    {
        if (string.IsNullOrEmpty(_receivedNxmUrl))
        {
            HasError = true;
            ErrorMessage = "No download URL received";
            return;
        }

        HasError = false;
        IsDownloading = true;
        DownloadProgress = "Starting download...";

        var nexusBridge = FindNexusBridge();
        if (string.IsNullOrEmpty(nexusBridge))
        {
            HasError = true;
            ErrorMessage = "NexusBridge executable not found!";
            IsDownloading = false;
            return;
        }

        // Strip trailing backslashes from path - "path\" causes \" to be interpreted as escaped quote
        var mo2PathClean = _mo2Path.TrimEnd('\\', '/');
        var startInfo = new ProcessStartInfo
        {
            FileName = nexusBridge,
            Arguments = $"\"{_collectionUrl}\" \"{mo2PathClean}\" --profile \"{_profileName}\" --nxm \"{_receivedNxmUrl}\"",
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
                HasError = true;
                ErrorMessage = "Failed to start download process";
                IsDownloading = false;
                return;
            }

            process.OutputDataReceived += (s, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data))
                {
                    Dispatcher.UIThread.Post(() =>
                    {
                        // Parse download progress
                        if (e.Data.Contains("Downloading:") || e.Data.Contains("Downloaded"))
                        {
                            DownloadProgress = e.Data.Trim();
                        }
                        else if (e.Data.Contains("NXM_DOWNLOAD_COMPLETE"))
                        {
                            DownloadComplete = true;
                        }
                    });
                }
            };

            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            await process.WaitForExitAsync();

            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsDownloading = false;

                if (process.ExitCode == 0)
                {
                    DownloadComplete = true;
                    StatusMessage = $"Downloaded! ({QueuePosition}/{QueueTotal})";

                    // Auto-advance to next mod after short delay
                    Task.Delay(1000).ContinueWith(_ =>
                    {
                        Dispatcher.UIThread.Post(() =>
                        {
                            if (DownloadComplete)
                            {
                                NextMod();
                            }
                        });
                    });
                }
                else
                {
                    HasError = true;
                    ErrorMessage = "Download failed. The link may have expired - click 'Retry' to try again.";
                }
            });
        }
        catch (Exception ex)
        {
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsDownloading = false;
                HasError = true;
                ErrorMessage = $"Error: {ex.Message}";
            });
        }
    }

    [RelayCommand]
    private void NextMod()
    {
        _currentIndex++;
        UpdateCurrentMod();
        // Auto-open browser for next mod if not done
        if (_currentIndex < _downloadQueue.Count)
        {
            OpenInBrowser();
        }
    }

    [RelayCommand]
    private void SkipMod()
    {
        _currentIndex++;
        UpdateCurrentMod();
        // Auto-open browser for next mod if not done
        if (_currentIndex < _downloadQueue.Count)
        {
            OpenInBrowser();
        }
    }

    [RelayCommand]
    private void Cancel()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Confirm, _collectionUrl));
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
}
