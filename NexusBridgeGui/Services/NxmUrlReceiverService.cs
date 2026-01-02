using System;
using System.IO;
using System.Threading;

namespace NexusBridgeGui.Services;

public static class NxmUrlReceiverService
{
    // Use home directory instead of temp - single-file apps may have different temp paths
    private static readonly string PendingUrlFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        ".nexusbridge_pending_nxm.txt"
    );

    public static event EventHandler<string>? UrlReceived;

    public static string? PendingUrl { get; private set; }

    public static void SetPendingUrl(string url)
    {
        PendingUrl = url;
        // Also write to file for inter-process communication
        try
        {
            Console.WriteLine($"[NXM] Writing URL to: {PendingUrlFile}");
            File.WriteAllText(PendingUrlFile, url);
            Console.WriteLine($"[NXM] File written successfully");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[NXM] Failed to write file: {ex.Message}");
        }

        UrlReceived?.Invoke(null, url);
    }

    public static string? ConsumePendingUrl()
    {
        var url = PendingUrl;
        PendingUrl = null;

        // Also check file
        if (string.IsNullOrEmpty(url))
        {
            try
            {
                if (File.Exists(PendingUrlFile))
                {
                    url = File.ReadAllText(PendingUrlFile).Trim();
                    File.Delete(PendingUrlFile);
                }
            }
            catch
            {
                // Ignore file errors
            }
        }
        else
        {
            // Clean up file if we used memory
            try
            {
                if (File.Exists(PendingUrlFile))
                    File.Delete(PendingUrlFile);
            }
            catch { }
        }

        return string.IsNullOrEmpty(url) ? null : url;
    }

    public static void ClearPending()
    {
        PendingUrl = null;
        try
        {
            if (File.Exists(PendingUrlFile))
                File.Delete(PendingUrlFile);
        }
        catch { }
    }
}
