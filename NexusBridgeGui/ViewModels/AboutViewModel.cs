using System;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using NexusBridgeGui.Services;

namespace NexusBridgeGui.ViewModels;

public partial class AboutViewModel : ViewModelBase
{
    public event EventHandler<NavigationEventArgs>? NavigateRequested;

    public string Version => "0.0.2";

    [ObservableProperty]
    private bool _isProtocolHandlerRegistered;

    [ObservableProperty]
    private string _protocolStatusMessage = "";

    public AboutViewModel()
    {
        UpdateProtocolStatus();
    }

    private void UpdateProtocolStatus()
    {
        IsProtocolHandlerRegistered = ProtocolHandlerService.IsRegistered();
        ProtocolStatusMessage = IsProtocolHandlerRegistered
            ? "nxm:// URLs will open with NexusBridge"
            : "Click to register nxm:// URL handler";
    }

    [RelayCommand]
    private void RegisterProtocolHandler()
    {
        var (success, message) = ProtocolHandlerService.Register();
        ProtocolStatusMessage = message;
        if (success)
            UpdateProtocolStatus();
    }

    [RelayCommand]
    private void UnregisterProtocolHandler()
    {
        var (success, message) = ProtocolHandlerService.Unregister();
        ProtocolStatusMessage = message;
        if (success)
            UpdateProtocolStatus();
    }

    [RelayCommand]
    private void GoBack()
    {
        NavigateRequested?.Invoke(this, new NavigationEventArgs(NavigationTarget.MainMenu));
    }
}
