using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using NexusBridgeGui.Services;

namespace NexusBridgeGui.ViewModels;

public partial class Mo2SetupViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    private readonly SettingsService _settings;
    private readonly string _collectionUrl;
    private Window? _window;

    [ObservableProperty]
    private string _mo2Path = "";

    [ObservableProperty]
    private string _profileName = "";

    [ObservableProperty]
    private bool _isValidMo2Folder;

    [ObservableProperty]
    private bool _directoryExists;

    [ObservableProperty]
    private string _validationMessage = "";

    [ObservableProperty]
    private bool _overrideValidation;

    [ObservableProperty]
    private bool _canStartInstall;

    public ObservableCollection<string> ExistingProfiles { get; } = new();

    public Mo2SetupViewModel(SettingsService settings, string collectionUrl)
    {
        _settings = settings;
        _collectionUrl = collectionUrl;
        Mo2Path = settings.Mo2Path;
        ProfileName = settings.ProfileName;
        ValidateMo2Path();
    }

    public void SetWindow(Window window)
    {
        _window = window;
    }

    partial void OnMo2PathChanged(string value)
    {
        ValidateMo2Path();
        UpdateCanStartInstall();
    }

    partial void OnProfileNameChanged(string value) => UpdateCanStartInstall();
    partial void OnOverrideValidationChanged(bool value) => UpdateCanStartInstall();

    private void UpdateCanStartInstall()
    {
        bool hasPath = !string.IsNullOrWhiteSpace(Mo2Path);
        bool hasProfile = !string.IsNullOrWhiteSpace(ProfileName);
        bool validOrOverridden = IsValidMo2Folder || (DirectoryExists && OverrideValidation);

        CanStartInstall = hasPath && hasProfile && validOrOverridden;
    }

    private void ValidateMo2Path()
    {
        ExistingProfiles.Clear();
        DirectoryExists = false;
        OverrideValidation = false;

        if (string.IsNullOrWhiteSpace(Mo2Path))
        {
            IsValidMo2Folder = false;
            ValidationMessage = "";
            return;
        }

        if (!Directory.Exists(Mo2Path))
        {
            IsValidMo2Folder = false;
            DirectoryExists = false;
            ValidationMessage = "Directory does not exist";
            return;
        }

        DirectoryExists = true;

        // Check for MO2 indicators
        var modsDir = Path.Combine(Mo2Path, "mods");
        var profilesDir = Path.Combine(Mo2Path, "profiles");

        if (!Directory.Exists(modsDir) && !Directory.Exists(profilesDir))
        {
            IsValidMo2Folder = false;
            ValidationMessage = "Not a valid MO2 directory (missing mods/profiles folders)";
            return;
        }

        IsValidMo2Folder = true;
        ValidationMessage = "Valid MO2 installation detected";

        // Load existing profiles
        if (Directory.Exists(profilesDir))
        {
            var profiles = Directory.GetDirectories(profilesDir)
                .Select(Path.GetFileName)
                .Where(n => n != null)
                .Cast<string>()
                .OrderBy(n => n);

            foreach (var profile in profiles)
            {
                ExistingProfiles.Add(profile);
            }
        }
    }

    [RelayCommand]
    private async Task BrowseFolder()
    {
        if (_window == null) return;

        var folders = await _window.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Select MO2 Directory",
            AllowMultiple = false
        });

        if (folders.Count > 0)
        {
            Mo2Path = folders[0].Path.LocalPath;
        }
    }

    [RelayCommand]
    private void StartInstall()
    {
        if (!CanStartInstall)
            return;

        _settings.Mo2Path = Mo2Path;
        _settings.ProfileName = ProfileName;
        _settings.Save();

        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Confirm, _collectionUrl));
    }

    [RelayCommand]
    private void GoBack()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Install));
    }
}
