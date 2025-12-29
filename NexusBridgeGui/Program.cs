using System;
using System.IO;
using Avalonia;
using NexusBridgeGui.Services;

namespace NexusBridgeGui;

class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        string? nxmUrl = null;

        // Check for --nxm-url argument (from protocol handler)
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--nxm-url" && i + 1 < args.Length)
            {
                nxmUrl = args[i + 1];
                break;
            }
            // Also handle case where URL is passed directly (some browsers do this)
            if (args[i].StartsWith("nxm://"))
            {
                nxmUrl = args[i];
                break;
            }
        }

        // If we have an nxm URL, write it to the shared file and exit
        // The running instance will pick it up via polling
        if (!string.IsNullOrEmpty(nxmUrl))
        {
            NxmUrlReceiverService.SetPendingUrl(nxmUrl);
            // Exit - don't start a new window, let the running instance handle it
            return;
        }

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}
