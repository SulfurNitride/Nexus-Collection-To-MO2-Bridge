using CommunityToolkit.Mvvm.ComponentModel;
using NexusBridgeGui.Services;

namespace NexusBridgeGui.ViewModels;

public partial class MainWindowViewModel : ViewModelBase
{
    private readonly SettingsService _settings;
    private string _pendingCollectionUrl = "";

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

        ViewModelBase newView = e.Target switch
        {
            NavigationTarget.MainMenu => CreateMainMenuViewModel(),
            NavigationTarget.Install => CreateInstallViewModel(),
            NavigationTarget.Mo2Setup => CreateMo2SetupViewModel(_pendingCollectionUrl),
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

    private ProgressViewModel CreateProgressViewModel(string collectionUrl)
    {
        var vm = new ProgressViewModel(collectionUrl, _settings.Mo2Path);
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
