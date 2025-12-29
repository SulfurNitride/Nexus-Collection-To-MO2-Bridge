using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Avalonia.Threading;

namespace NexusBridgeGui.ViewModels;

public partial class ProgressViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    private readonly string _collectionUrl;
    private readonly string _mo2Path;
    private Process? _process;

    // For speed calculation
    private DateTime _downloadStartTime;
    private double _lastBytesDownloaded;
    private DateTime _lastSpeedUpdate;

    [ObservableProperty]
    private string _phase = "Starting...";

    [ObservableProperty]
    private double _progress;

    [ObservableProperty]
    private int _totalMods;

    [ObservableProperty]
    private int _toDownload;

    [ObservableProperty]
    private int _downloaded;

    [ObservableProperty]
    private int _toInstall;

    [ObservableProperty]
    private int _installed;

    [ObservableProperty]
    private int _skipped;

    [ObservableProperty]
    private int _failed;

    [ObservableProperty]
    private bool _isRunning = true;

    [ObservableProperty]
    private bool _hasError;

    // Download stats
    [ObservableProperty]
    private double _downloadedMb;

    [ObservableProperty]
    private double _totalDownloadMb;

    [ObservableProperty]
    private string _downloadSpeed = "";

    [ObservableProperty]
    private string _eta = "";

    public ObservableCollection<string> LogMessages { get; } = new();

    public ProgressViewModel(string collectionUrl, string mo2Path)
    {
        _collectionUrl = collectionUrl;
        _mo2Path = mo2Path;
        Task.Run(RunInstallation);
    }

    private async Task RunInstallation()
    {
        AddLog($"Starting installation...");
        AddLog($"Collection: {_collectionUrl}");
        AddLog($"MO2 Path: {_mo2Path}");

        var nexusBridge = FindNexusBridge();
        if (string.IsNullOrEmpty(nexusBridge))
        {
            AddLog("ERROR: NexusBridge executable not found!");
            AddLog("Make sure NexusBridge is in the same directory as NexusBridgeGui");
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsRunning = false;
                HasError = true;
                Phase = "Error!";
            });
            return;
        }

        AddLog($"Using: {nexusBridge}");

        var startInfo = new ProcessStartInfo
        {
            FileName = nexusBridge,
            Arguments = $"\"{_collectionUrl}\" \"{_mo2Path}\" --yes",
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };

        try
        {
            _process = Process.Start(startInfo);
            if (_process == null)
            {
                AddLog("ERROR: Failed to start NexusBridge process");
                await Dispatcher.UIThread.InvokeAsync(() =>
                {
                    IsRunning = false;
                    HasError = true;
                    Phase = "Error!";
                });
                return;
            }

            _process.OutputDataReceived += (s, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data))
                {
                    ProcessLine(e.Data);
                }
            };

            _process.ErrorDataReceived += (s, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data))
                {
                    ProcessLine(e.Data);
                }
            };

            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();

            await _process.WaitForExitAsync();
        }
        catch (Exception ex)
        {
            AddLog($"ERROR: {ex.Message}");
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                HasError = true;
                Phase = "Error!";
            });
        }

        await Dispatcher.UIThread.InvokeAsync(() =>
        {
            IsRunning = false;
            if (!HasError && Phase != "Complete!")
            {
                Phase = "Complete!";
                Progress = 1.0;
            }
        });
    }

    private void ProcessLine(string line)
    {
        Dispatcher.UIThread.Post(() =>
        {
            // Parse phase and progress info
            if (line.Contains("Mods:"))
            {
                if (int.TryParse(ExtractNumber(line, "Mods:"), out int mods))
                    TotalMods = mods;
            }
            else if (line.Contains("Phase 1: Scanning"))
            {
                Phase = "Scanning archives...";
            }
            else if (line.Contains("Need to download"))
            {
                if (int.TryParse(ExtractNumber(line, "Need to download"), out int count))
                    ToDownload = count;
            }
            else if (line.Contains("Phase 1b: Downloading"))
            {
                Phase = "Downloading...";
                _downloadStartTime = DateTime.Now;
                _lastSpeedUpdate = DateTime.Now;
                if (int.TryParse(ExtractNumber(line, "Downloading"), out int count))
                    ToDownload = count;
            }
            else if (line.Contains("] Downloading:") && line.Contains("[") && !line.Contains(" MB ("))
            {
                // "[X/Y] Downloading: ModName" - a new download started
                Downloaded++;
                UpdateProgress();
            }
            // Parse download progress: "Downloading: 45.2 / 120.5 MB (37%)"
            else if (line.Contains("Downloading:") && line.Contains(" MB ("))
            {
                var match = Regex.Match(line, @"(\d+\.?\d*)\s*/\s*(\d+\.?\d*)\s*MB");
                if (match.Success)
                {
                    if (double.TryParse(match.Groups[1].Value, out double current) &&
                        double.TryParse(match.Groups[2].Value, out double total))
                    {
                        DownloadedMb = current;
                        if (total > TotalDownloadMb)
                            TotalDownloadMb = total;

                        UpdateDownloadSpeed(current);
                    }
                }
            }
            // Parse total download size if provided: "Total download size: 45.2 GB"
            else if (line.Contains("Total download size:") || line.Contains("Download size:"))
            {
                var match = Regex.Match(line, @"(\d+\.?\d*)\s*(GB|MB|KB)");
                if (match.Success && double.TryParse(match.Groups[1].Value, out double size))
                {
                    string unit = match.Groups[2].Value;
                    TotalDownloadMb = unit switch
                    {
                        "GB" => size * 1024,
                        "KB" => size / 1024,
                        _ => size
                    };
                }
            }
            else if (line.Contains("Downloaded:") && line.Contains("Failed:"))
            {
                if (int.TryParse(ExtractNumber(line, "Downloaded:"), out int dl))
                    Downloaded = dl;
                Phase = "Downloads complete";
            }
            else if (line.Contains("Phase 2: Installing"))
            {
                Phase = "Installing mods...";
                DownloadSpeed = "";
                Eta = "";
                if (int.TryParse(ExtractNumber(line, "Installing"), out int count))
                    ToInstall = count;
            }
            else if (line.Contains(" - Done!") && line.Contains("]"))
            {
                Installed++;
                UpdateProgress();
            }
            else if (line.Contains("Generating plugins.txt") || line.Contains("Generating modlist.txt"))
            {
                Phase = "Generating load order...";
                Progress = 0.95;
            }
            else if (line.Contains("Skipped:") && line.Contains("already installed"))
            {
                if (int.TryParse(ExtractNumber(line, "Skipped:"), out int count))
                    Skipped = count;
            }
            else if (line.Contains("Failed:") && !line.Contains("Downloaded:"))
            {
                if (int.TryParse(ExtractNumber(line, "Failed:"), out int count))
                    Failed = count;
            }
            else if (line.Contains("Done!") && !line.Contains(" - Done!"))
            {
                Phase = "Complete!";
                Progress = 1.0;
            }
            else if (line.Contains("restart Mod Organizer"))
            {
                Phase = "Complete!";
                Progress = 1.0;
            }
            else if (line.Contains("Error:") || line.Contains("ERROR:"))
            {
                HasError = true;
            }

            // Filter download progress spam but keep useful messages
            if (!ShouldFilterLine(line))
            {
                AddLogDirect(line);
            }
        });
    }

    private void UpdateDownloadSpeed(double currentMb)
    {
        var now = DateTime.Now;
        var elapsed = (now - _lastSpeedUpdate).TotalSeconds;

        if (elapsed >= 1.0) // Update speed every second
        {
            double mbDelta = currentMb - _lastBytesDownloaded;
            double speedMbps = mbDelta / elapsed;

            if (speedMbps > 0)
            {
                DownloadSpeed = speedMbps >= 1
                    ? $"{speedMbps:F1} MB/s"
                    : $"{speedMbps * 1024:F0} KB/s";

                // Calculate ETA
                if (TotalDownloadMb > 0 && currentMb < TotalDownloadMb)
                {
                    double remaining = TotalDownloadMb - currentMb;
                    double secondsLeft = remaining / speedMbps;

                    if (secondsLeft < 60)
                        Eta = $"{secondsLeft:F0}s";
                    else if (secondsLeft < 3600)
                        Eta = $"{secondsLeft / 60:F0}m {secondsLeft % 60:F0}s";
                    else
                        Eta = $"{secondsLeft / 3600:F0}h {(secondsLeft % 3600) / 60:F0}m";
                }
            }

            _lastBytesDownloaded = currentMb;
            _lastSpeedUpdate = now;
        }
    }

    private bool ShouldFilterLine(string line)
    {
        // Keep "[X/Y] Downloading: ModName" lines
        if (line.Contains("] Downloading:") && line.Contains("[") && !line.Contains(" MB ("))
            return false;

        // Filter download progress lines (we track these separately)
        if (line.Contains("Downloading:") && line.Contains(" MB ("))
            return true;
        if (line.Contains("%)") && line.Length < 50)
            return true;
        if (line.Contains("nloading:"))
            return true;
        if (!string.IsNullOrEmpty(line) && line[0] == '\r')
            return true;
        if (line.Length < 30 && line.Contains(" / "))
            return true;

        return false;
    }

    private void UpdateProgress()
    {
        int total = ToDownload + (ToInstall > 0 ? ToInstall : ToDownload);
        if (total > 0)
        {
            Progress = (double)(Downloaded + Installed) / total;
        }
    }

    private static string? ExtractNumber(string line, string after)
    {
        int idx = line.IndexOf(after);
        if (idx < 0) return null;

        string sub = line[(idx + after.Length)..].TrimStart();
        string num = "";
        foreach (char c in sub)
        {
            if (char.IsDigit(c))
                num += c;
            else if (num.Length > 0)
                break;
        }
        return num;
    }

    private void AddLog(string message)
    {
        Dispatcher.UIThread.Post(() => AddLogDirect(message));
    }

    private void AddLogDirect(string message)
    {
        LogMessages.Add(message);
        if (LogMessages.Count > 100)
            LogMessages.RemoveAt(0);
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

    [RelayCommand]
    private void GoBack()
    {
        _process?.Kill();
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.MainMenu));
    }
}
