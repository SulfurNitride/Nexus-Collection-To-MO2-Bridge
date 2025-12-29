using System;
using CommunityToolkit.Mvvm.Input;

namespace NexusBridgeGui.ViewModels;

public partial class MainMenuViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    [RelayCommand]
    private void InstallCollection()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.Install));
    }

    [RelayCommand]
    private void OpenAbout()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.About));
    }

    [RelayCommand]
    private void Exit()
    {
        Environment.Exit(0);
    }
}
