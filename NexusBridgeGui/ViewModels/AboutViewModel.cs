using System;
using CommunityToolkit.Mvvm.Input;

namespace NexusBridgeGui.ViewModels;

public partial class AboutViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    public string Version => "2.0";

    [RelayCommand]
    private void GoBack()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.MainMenu));
    }
}
