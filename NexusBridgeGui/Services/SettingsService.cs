using System;
using System.IO;
using System.Runtime.InteropServices;

namespace NexusBridgeGui.Services;

public class SettingsService
{
    public string ApiKey { get; set; } = "";
    public string Mo2Path { get; set; } = "";
    public string ProfileName { get; set; } = "";

    private static string GetConfigDir()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            return Path.Combine(appData, "NexusBridge");
        }
        else
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return Path.Combine(home, ".config", "nexusbridge");
        }
    }

    private static string GetDefaultMo2Path()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return Path.Combine(home, "Games", "MO2");
    }

    public void Load()
    {
        var configDir = GetConfigDir();
        Directory.CreateDirectory(configDir);

        var keyFile = Path.Combine(configDir, "apikey.txt");
        if (File.Exists(keyFile))
        {
            ApiKey = File.ReadAllText(keyFile).Trim();
        }

        var pathFile = Path.Combine(configDir, "mo2path.txt");
        if (File.Exists(pathFile))
        {
            Mo2Path = File.ReadAllText(pathFile).Trim();
        }

        var profileFile = Path.Combine(configDir, "profile.txt");
        if (File.Exists(profileFile))
        {
            ProfileName = File.ReadAllText(profileFile).Trim();
        }

        if (string.IsNullOrEmpty(Mo2Path))
        {
            Mo2Path = GetDefaultMo2Path();
        }
    }

    public void Save()
    {
        var configDir = GetConfigDir();
        Directory.CreateDirectory(configDir);

        File.WriteAllText(Path.Combine(configDir, "apikey.txt"), ApiKey);
        File.WriteAllText(Path.Combine(configDir, "mo2path.txt"), Mo2Path);
        File.WriteAllText(Path.Combine(configDir, "profile.txt"), ProfileName);
    }
}
