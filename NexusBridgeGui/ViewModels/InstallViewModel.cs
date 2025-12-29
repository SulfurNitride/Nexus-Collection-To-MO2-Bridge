using System;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using NexusBridgeGui.Services;

namespace NexusBridgeGui.ViewModels;

public partial class InstallViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    private readonly SettingsService _settings;

    [ObservableProperty]
    private string _collectionUrl = "";

    [ObservableProperty]
    private string _apiKey = "";

    [ObservableProperty]
    private bool _canProceed;

    public InstallViewModel(SettingsService settings)
    {
        _settings = settings;
        ApiKey = settings.ApiKey;
        UpdateCanProceed();
    }

    partial void OnCollectionUrlChanged(string value) => UpdateCanProceed();
    partial void OnApiKeyChanged(string value) => UpdateCanProceed();

    private void UpdateCanProceed()
    {
        CanProceed = !string.IsNullOrWhiteSpace(CollectionUrl) && !string.IsNullOrWhiteSpace(ApiKey);
    }

    [RelayCommand]
    private void Next()
    {
        if (CanProceed)
        {
            // Save API key for next time
            _settings.ApiKey = ApiKey;
            _settings.Save();

            NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Mo2Setup, CollectionUrl));
        }
    }

    [RelayCommand]
    private void GoBack()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.MainMenu));
    }
}
