using System.Collections.Generic;
using CommunityToolkit.Mvvm.ComponentModel;
using NexusBridgeGui.Services;

namespace NexusBridgeGui.ViewModels;

public partial class MainWindowViewModel : ViewModelBase
{
    private readonly SettingsService _settings;
    private string _pendingCollectionUrl = "";
    private string _pendingGame = "";
    private List<DownloadQueueItem> _pendingDownloadQueue = new();

    [ObservableProperty]
    private ViewModelBase _currentView;

    public MainWindowViewModel()
    {
        _settings = new SettingsService();
        _settings.Load();

        var mainMenu = new MainMenuViewModel();
        mainMenu.NavigateRequested += OnNavigateRequested;
        CurrentView = mainMenu;
    }

    private void OnNavigateRequested(object? sender, NavigationEventArgs e)
    {
        // Store collection URL when moving through wizard
        if (e.CollectionUrl != null)
        {
            _pendingCollectionUrl = e.CollectionUrl;
        }

        // Store download queue when navigating from Confirm to NonPremiumDownload
        if (sender is ConfirmViewModel confirmVm && e.Target == NavigationTarget.NonPremiumDownload)
        {
            _pendingDownloadQueue = new List<DownloadQueueItem>(confirmVm.DownloadQueue);
            _pendingGame = confirmVm.Game;
        }

        ViewModelBase newView = e.Target switch
        {
            NavigationTarget.MainMenu => CreateMainMenuViewModel(),
            NavigationTarget.Install => CreateInstallViewModel(),
            NavigationTarget.Mo2Setup => CreateMo2SetupViewModel(_pendingCollectionUrl),
            NavigationTarget.Confirm => CreateConfirmViewModel(_pendingCollectionUrl),
            NavigationTarget.NonPremiumDownload => CreateNonPremiumDownloadViewModel(_pendingCollectionUrl),
            NavigationTarget.Progress => CreateProgressViewModel(_pendingCollectionUrl),
            NavigationTarget.About => CreateAboutViewModel(),
            _ => CurrentView
        };

        CurrentView = newView;
    }

    private MainMenuViewModel CreateMainMenuViewModel()
    {
        _pendingCollectionUrl = ""; // Reset on returning to main menu
        var vm = new MainMenuViewModel();
        vm.NavigateRequested += OnNavigateRequested;
        return vm;
    }

    private InstallViewModel CreateInstallViewModel()
    {
        var vm = new InstallViewModel(_settings);
        vm.NavigateRequested += OnNavigateRequested;
        return vm;
    }

    private Mo2SetupViewModel CreateMo2SetupViewModel(string collectionUrl)
    {
        var vm = new Mo2SetupViewModel(_settings, collectionUrl);
        vm.NavigateRequested += OnNavigateRequested;
        return vm;
    }

    private ConfirmViewModel CreateConfirmViewModel(string collectionUrl)
    {
        var vm = new ConfirmViewModel(collectionUrl, _settings.Mo2Path, _settings.ProfileName);
        vm.NavigateRequested += OnNavigateRequested;
        return vm;
    }

    private ProgressViewModel CreateProgressViewModel(string collectionUrl)
    {
        var vm = new ProgressViewModel(collectionUrl, _settings.Mo2Path, _settings.ProfileName);
        vm.NavigateRequested += OnNavigateRequested;
        return vm;
    }

    private NonPremiumDownloadViewModel CreateNonPremiumDownloadViewModel(string collectionUrl)
    {
        var vm = new NonPremiumDownloadViewModel(
            collectionUrl,
            _settings.Mo2Path,
            _settings.ProfileName,
            _pendingGame,
            _pendingDownloadQueue);
        vm.NavigateRequested += OnNavigateRequested;
        return vm;
    }

    private AboutViewModel CreateAboutViewModel()
    {
        var vm = new AboutViewModel();
        vm.NavigateRequested += OnNavigateRequested;
        return vm;
    }
}

public enum NavigationTarget
{
    MainMenu,
    Install,
    Mo2Setup,
    Confirm,
    NonPremiumDownload,
    Progress,
    About
}

public class NavigationEventArgs : System.EventArgs
{
    public NavigationTarget Target { get; }
    public string? CollectionUrl { get; }

    public NavigationEventArgs(NavigationTarget target, string? collectionUrl = null)
    {
        Target = target;
        CollectionUrl = collectionUrl;
    }
}
